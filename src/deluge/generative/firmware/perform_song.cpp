/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "generative/firmware/perform_song.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/sound_settings.h"
#include "generative/firmware/card_sounds.h"
#include "gui/views/view.h"
#include "io/midi/midi_follow.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/non_audio_drum.h"
#include "model/fx/stutterer.h"
#include "model/global_effectable/global_effectable.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_holder.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/multi_range/multi_range.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <cstring>
#include <new>

namespace deluge::generative::firmware {

namespace {

/// Samples shorter than this play once, as the sample browser sets them (spec §10.4).
constexpr uint32_t kOneShotMaxLengthMs = 2000;

/// The browser's lane names, unique within a Kit.
const char* laneName(Lane lane) {
	static const char* const kNames[kNumLanes] = {"KICK",     "SNARE", "CLAP",  "RIM",    "CLOSED HAT",
	                                              "OPEN HAT", "RIDE",  "CRASH", "TOM",    "PERC 1",
	                                              "PERC 2",   "RISER", "FALL",  "IMPACT", "SIREN"};
	return kNames[static_cast<int32_t>(lane)];
}

/// A private song's Kits are built while another song is current, so their constructors took sync defaults from
/// that song. Set them as they would be for `song` (spec §10.4).
void applySyncDefaults(ModControllableAudio* modControllable, Song* song) {
	int32_t magnitude = song->insideWorldTickMagnitude + song->insideWorldTickMagnitudeOffsetFromBPM;
	modControllable->delay.syncLevel = static_cast<SyncLevel>(8 - magnitude);
	modControllable->sidechain.syncLevel = static_cast<SyncLevel>(7 - magnitude);
}

/// Splits "SYNTHS/165 Acid Arp.XML" into folder and name, as the preset browser keeps them.
void splitPresetPath(const char* path, String* folder, String* name) {
	const char* slash = std::strrchr(path, '/');
	const char* base = slash ? slash + 1 : path;
	if (slash != nullptr) {
		folder->set(path, static_cast<int32_t>(slash - path));
	}
	const char* dot = std::strrchr(base, '.');
	name->set(base, dot ? static_cast<int32_t>(dot - base) : static_cast<int32_t>(std::strlen(base)));
}

/// Deletes an Output that was never linked into `song`'s Output list.
void deleteUnlinked(Song* song, Instrument* instrument) {
	instrument->prepareForHibernationOrDeletion();
	song->deleteOutput(instrument);
}

/// Gives `instrument` its empty, always-active, looping 1-bar session clip and links both into `song` (spec §12.8).
/// On failure the instrument is deleted.
Error link(Song* song, Instrument* instrument, InstrumentClip** clipOut) {
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	if (memory == nullptr) {
		deleteUnlinked(song, instrument);
		return Error::INSUFFICIENT_RAM;
	}
	auto* clip = new (memory) InstrumentClip(song);
	clip->loopLength = song->getBarLength(); // before setAudioInstrument(), which trims the ParamManager to it
	clip->activeIfNoSolo = true;
	Error error = clip->setAudioInstrument(instrument, song, true, nullptr);
	if (error != Error::NONE) {
		clip->~InstrumentClip();
		delugeDealloc(memory);
		deleteUnlinked(song, instrument);
		return error;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);
	if (instrument->type == OutputType::KIT) {
		clip->setupAsNewKitClipIfNecessary(modelStack); // one NoteRow per drum, in drum order
	}
	// The Generator owns the notes: a preset's saved arp goes off (spec §10.6).
	clip->arpSettings.preset = ArpPreset::OFF;
	clip->arpSettings.mode = ArpMode::OFF;

	error = song->sessionClips.insertClipAtIndex(clip, song->sessionClips.getNumElements());
	if (error != Error::NONE) {
		song->deleteClipObject(clip, false, InstrumentRemoval::NONE);
		deleteUnlinked(song, instrument);
		return error;
	}
	song->addOutput(instrument, false);
	instrument->setActiveClip(modelStack);
	if (song == currentSong && playbackHandler.isEitherClockActive()) {
		// A sound re-roll links into the song playing: the clip joins the session where playback is now.
		session.reSyncClip(modelStack, true);
	}
	*clipOut = clip;
	return Error::NONE;
}

const char* sourceName(SoundSource source) {
	switch (source) {
	case SoundSource::Pick:
		return "pick";
	case SoundSource::Fallback:
		return "fallback";
	case SoundSource::DefaultSynth:
		return "default";
	case SoundSource::Silent:
		return "silent";
	case SoundSource::CardError:
		break;
	}
	return "card_error";
}

/// What every build step reads, and the count of sounds that fell back to nothing.
struct Build {
	Song* song;
	const SoundPicks& sounds;
	const Palette& palette;
	CardFolders& folders;
	/// The runner job's chunks: the build yields and checks for cancel after each Kit row and each Output.
	Chunks& chunks;
	/// Parts on the default synth and played lanes left silent: one warning covers them all (spec §10.1).
	int32_t missing = 0;
};

/// Frees a drum that never joined a Kit, with any ParamManager backed up for it.
void deleteLooseDrum(Song* song, SoundDrum* drum) {
	song->deleteBackedUpParamManagersForModControllable(drum);
	void* memory = dynamic_cast<void*>(drum);
	drum->~SoundDrum();
	delugeDealloc(memory);
}

/// What the firmware's loaders share: the last path tried, owned so it outlives the core's fallback chain, and the
/// error that stopped loading.
class CardLoader : public SoundLoader {
public:
	/// The error that stopped loading, for a CardError outcome.
	[[nodiscard]] Error stopError() const { return stopError_ != Error::NONE ? stopError_ : Error::SD_CARD; }
	[[nodiscard]] const char* lastPath() const { return path_.c_str(); }

protected:
	/// Starts a load of `path`. False if the path can't be held, which counts as a missing file.
	bool begin(const char* path) { return path_.set(path); }

	LoadOutcome fail(Error error) {
		LoadOutcome outcome = loadOutcome(error);
		if (outcome == LoadOutcome::CardError) {
			stopError_ = error;
		}
		return outcome;
	}

	/// Finds a preset on the card. Missing unless it is there; a card error if the card isn't.
	LoadOutcome findPreset(FilePointer* filePointer, String* folder, String* name) {
		if (cardErrorInjected()) {
			return fail(Error::SD_CARD);
		}
		if (!StorageManager::fileExists(path_.c_str(), filePointer)) {
			return fail(StorageManager::initSD() == Error::NONE ? Error::FILE_NOT_FOUND : Error::SD_CARD);
		}
		splitPresetPath(path_.c_str(), folder, name);
		return LoadOutcome::Loaded;
	}

private:
	SoundPath path_;
	Error stopError_ = Error::NONE;
};

/// Loads a Kit row's sound for the core's fallback chain (spec §10.2, §10.5): samples into the row's own sample
/// drum, and an FX lane's synth preset into a new drum that replaces it. Nothing is added to the Kit here.
class RowLoader final : public CardLoader {
public:
	RowLoader(Song* song, SoundDrum* sampleDrum) : song_(song), sampleDrum_(sampleDrum) {}

	LoadOutcome loadSample(const char* path) override {
		if (!begin(path)) {
			return LoadOutcome::Missing;
		}
		if (cardErrorInjected()) {
			return fail(Error::SD_CARD);
		}
		MultiRange* range = sampleDrum_->sources[0].getOrCreateFirstRange();
		if (range == nullptr) {
			return fail(Error::INSUFFICIENT_RAM);
		}
		// Not Source::setOscType(): it writes soundEditor state, and setupAsSample() already set the type.
		AudioFileHolder* holder = range->getAudioFileHolder();
		holder->setAudioFile(nullptr);
		Error error = holder->filePath.set(path);
		if (error != Error::NONE) {
			return fail(error);
		}
		// manuallySelected = false leaves the sample browser's last-path state alone (spec §10.4).
		error = holder->loadFile(false, false, true, CLUSTER_ENQUEUE);
		if (error == Error::NONE && holder->audioFile == nullptr) {
			error = Error::FILE_NOT_FOUND;
		}
		if (error != Error::NONE) {
			holder->filePath.clear(); // a silent row names no file
			return fail(error);
		}
		auto* sample = static_cast<Sample*>(holder->audioFile);
		sampleDrum_->sources[0].repeatMode =
		    sample->getLengthInMSec() < kOneShotMaxLengthMs ? SampleRepeatMode::ONCE : SampleRepeatMode::CUT;
		return LoadOutcome::Loaded;
	}

	LoadOutcome loadPreset(const char* path) override {
		if (!begin(path)) {
			return LoadOutcome::Missing;
		}
		FilePointer filePointer;
		String folder;
		String name;
		LoadOutcome found = findPreset(&filePointer, &folder, &name);
		if (found != LoadOutcome::Loaded) {
			return found;
		}
		SoundDrum* drum = nullptr;
		Error error = StorageManager::loadSynthToDrum(song_, nullptr, true, &drum, &filePointer, &name, &folder);
		if (error == Error::NONE && drum == nullptr) {
			error = Error::FILE_CORRUPTED; // a close failure can report success with no drum
		}
		if (error != Error::NONE) {
			return fail(error);
		}
		// Loading backed up the drum's ParamManager in the song; the Kit clip takes it from there when it links.
		if (song_->getBackedUpParamManagerPreferablyWithClip(drum, nullptr) == nullptr) {
			deleteLooseDrum(song_, drum);
			return fail(Error::FILE_CORRUPTED);
		}
		drum->loadAllSamples(true);
		drum->path.set(&folder);
		drum->arpSettings.preset = ArpPreset::OFF; // the Generator owns the notes (spec §10.6)
		drum->arpSettings.mode = ArpMode::OFF;
		synthDrum_ = drum;
		return LoadOutcome::Loaded;
	}

	/// The synth drum an FX lane loaded, if any. The caller owns it.
	[[nodiscard]] SoundDrum* synthDrum() const { return synthDrum_; }

private:
	Song* song_;
	SoundDrum* sampleDrum_;
	SoundDrum* synthDrum_ = nullptr;
};

/// Whether `part` plays `lane`. Lanes it doesn't play keep their silent row and load nothing.
bool plays(const PartSpec& part, Lane lane) {
	for (const LaneSpec& spec : part.lanes) {
		if (spec.lane == lane) {
			return true;
		}
	}
	return false;
}

/// Builds one Kit row: a sample drum, or for an FX lane with no sample a synth drum from a pack preset. The drum is
/// added to the Kit with its ParamManager backed up in the song.
Error buildRow(Build& build, Kit* kit, const PartSpec& part, Lane lane, Drum** rowOut) {
	Song* song = build.song;
	ParamManagerForTimeline paramManager;
	Error error = paramManager.setupWithPatching();
	if (error != Error::NONE) {
		return error;
	}
	auto* drum = static_cast<SoundDrum*>(StorageManager::createNewDrum(DrumType::SOUND));
	if (drum == nullptr) {
		return Error::INSUFFICIENT_RAM;
	}
	Sound::initParams(&paramManager);
	drum->setupAsSample(&paramManager);

	RowLoader loader(song, drum);
	bool played = plays(part, lane);
	SoundLoad load;
	if (played) {
		load = loadLaneSound(build.palette, lane, build.sounds.lane[static_cast<int32_t>(lane)].c_str(), build.folders,
		                     loader);
		if (load.source == SoundSource::CardError) {
			deleteLooseDrum(song, drum);
			// A folder scan stops at a cancel too (spec §12.10).
			return build.folders.cancelled() ? Error::ABORTED_BY_USER : loader.stopError();
		}
		build.missing += load.warns() ? 1 : 0;
	}

	if (load.synth) {
		deleteLooseDrum(song, drum); // the preset's drum takes the row, with its own sync and settings as saved
		drum = loader.synthDrum();
		// Loading backed up the preset drum's ParamManager already (RowLoader checked it is there).
		applySoundSettings(*drum, *song->getBackedUpParamManagerPreferablyWithClip(drum, nullptr),
		                   laneSoundSettings(lane));
	}
	else {
		applySoundSettings(*drum, paramManager, laneSoundSettings(lane));
		song->backUpParamManager(drum, nullptr, &paramManager, true);
		applySyncDefaults(drum, song); // a sample row was built while another song was current (spec §10.4)
	}
	kit->addDrum(drum); // the Kit owns it from here
	drum->drumName = laneName(lane);
	if (lane == Lane::Kick) {
		drum->sideChainSendLevel = INT32_MAX; // as the sample browser does for kicks (spec §10.4)
	}
	if (played) {
		TEST_EVENT("gen_sound %s %s %s mode=%d prio=%d %s", part.name, laneName(lane), sourceName(load.source),
		           static_cast<int>(drum->polyphonic), static_cast<int>(drum->voicePriority),
		           load.source == SoundSource::Silent ? "-" : loader.lastPath());
	}
	*rowOut = drum;
	return Error::NONE;
}

/// Assembles a Kit with one sound per lane (spec §10.2, §10.4): a row for every lane of its kind in the fixed order,
/// so laneRow() holds. Lanes the genre doesn't play, or with nothing loadable, are silent sample rows.
Error buildKit(Build& build, const PartSpec& part, PartOutput* out) {
	Song* song = build.song;
	ParamManagerForTimeline kitParamManager;
	Kit* kit = static_cast<Kit*>(StorageManager::createNewInstrument(OutputType::KIT, &kitParamManager));
	if (kit == nullptr) {
		return Error::INSUFFICIENT_RAM;
	}
	kit->name.set(part.name);
	kit->dirPath.set("KITS");
	applySyncDefaults(kit, song);
	song->backUpParamManager(kit, nullptr, &kitParamManager, true);

	const bool fx = part.role == Role::FX;
	Error error = Error::NONE;
	for (int32_t row = 0; row < kitRows(fx) && error == Error::NONE; row++) {
		error = buildRow(build, kit, part, laneAtRow(fx, row), &out->drums[row]);
		// A row's loads (its fallback chain included) are one chunk; then the runner job yields (spec §12.10).
		if (error == Error::NONE && build.chunks.boundary()) {
			error = Error::ABORTED_BY_USER;
		}
	}
	if (error != Error::NONE) {
		deleteUnlinked(song, kit); // with every ParamManager backed up for it and its drums
		*out = PartOutput{};
		return error;
	}
	out->output = kit;
	return link(song, kit, &out->clip);
}

/// Loads a synth part's preset for the core's fallback chain (spec §10.1).
class SynthLoader final : public CardLoader {
public:
	explicit SynthLoader(Song* song) : song_(song) {}

	LoadOutcome loadPreset(const char* path) override {
		if (!begin(path)) {
			return LoadOutcome::Missing;
		}
		FilePointer filePointer;
		String folder;
		String name;
		LoadOutcome found = findPreset(&filePointer, &folder, &name);
		if (found != LoadOutcome::Loaded) {
			return found;
		}
		Instrument* instrument = nullptr;
		Error error = StorageManager::loadInstrumentFromFile(song_, nullptr, OutputType::SYNTH, true, &instrument,
		                                                     &filePointer, &name, &folder);
		if (error == Error::NONE && instrument == nullptr) {
			error = Error::FILE_CORRUPTED;
		}
		if (error != Error::NONE) {
			return fail(error);
		}
		synth_ = static_cast<SoundInstrument*>(instrument);
		return LoadOutcome::Loaded;
	}
	LoadOutcome loadSample(const char*) override { return LoadOutcome::Missing; }

	[[nodiscard]] SoundInstrument* synth() const { return synth_; }

private:
	Song* song_;
	SoundInstrument* synth_ = nullptr;
};

/// The Deluge default synth patch, for a part whose palette loads nothing (spec §10.1).
SoundInstrument* defaultSynth(Song* song) {
	ParamManagerForTimeline paramManager;
	auto* synth = static_cast<SoundInstrument*>(StorageManager::createNewInstrument(OutputType::SYNTH, &paramManager));
	if (synth == nullptr) {
		return nullptr;
	}
	synth->dirPath.set("SYNTHS");
	synth->name.set("0");
	synth->setupAsDefaultSynth(&paramManager);
	applySyncDefaults(synth, song); // built while another song is current; a loaded preset keeps its own (spec §10.4)
	song->backUpParamManager(synth, nullptr, &paramManager, true);
	return synth;
}

/// Loads a synth part's sound: its pick, the rest of its palette, then the default synth with a warning
/// (spec §10.1), with the part's load-time settings (spec §10.6).
Error loadSynth(Build& build, int32_t partIndex, const PartSpec& part, PartOutput* out) {
	Song* song = build.song;
	SynthLoader loader(song);
	SoundLoad load = loadPartSound(build.palette, part, build.sounds.part[partIndex].c_str(), loader);
	if (load.source == SoundSource::CardError) {
		return loader.stopError();
	}
	SoundInstrument* synth = loader.synth();
	if (load.source == SoundSource::DefaultSynth) {
		synth = defaultSynth(song);
		if (synth == nullptr) {
			return Error::INSUFFICIENT_RAM;
		}
	}
	build.missing += load.warns() ? 1 : 0;
	synth->defaultArpSettings.preset = ArpPreset::OFF; // link() clears the clip's copy (spec §10.6)
	synth->defaultArpSettings.mode = ArpMode::OFF;
	out->output = synth;
	Error error = link(song, synth, &out->clip);
	if (error != Error::NONE) {
		return error;
	}
	applySoundSettings(*synth, out->clip->paramManager, partSoundSettings(part));
	TEST_EVENT("gen_sound %s - %s mode=%d prio=%d arp=%d %s porta=%ld", part.name, sourceName(load.source),
	           static_cast<int>(synth->polyphonic), static_cast<int>(synth->voicePriority),
	           static_cast<int>(out->clip->arpSettings.mode),
	           load.source == SoundSource::DefaultSynth ? "-" : loader.lastPath(),
	           static_cast<long>(out->clip->paramManager.getUnpatchedParamSet()
	                                 ->params[deluge::modulation::params::UNPATCHED_PORTAMENTO]
	                                 .getCurrentValue()));
	return Error::NONE;
}

} // namespace

Error buildPerformSong(const Recipe& recipe, const SoundPicks& sounds, const Palette& palette, Song** songOut,
                       Roster* roster, Chunks& chunks, int32_t* soundsMissing) {
	*roster = Roster{};
	if (soundsMissing != nullptr) {
		*soundsMissing = 0;
	}
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	if (memory == nullptr) {
		return Error::INSUFFICIENT_RAM;
	}
	Song* song = new (memory) Song();
	Error error = song->paramManager.setupUnpatched();
	if (error != Error::NONE) {
		deletePrivateSong(song);
		return error;
	}
	GlobalEffectable::initParams(&song->paramManager);
	song->setupDefault();
	// Song() takes the player's Resolution default. Perform's song runs at the Generator's own 24 ticks per 16th, so
	// every event lands on its exact tick (spec §12.3).
	song->insideWorldTickMagnitude = kGeneratorTickMagnitude;
	song->insideWorldTickMagnitudeOffsetFromBPM = 0;
	song->setBPM(static_cast<float>(recipe.tempo) / 100.f, false);
	song->swingAmount = recipe.swing;
	song->swingInterval = static_cast<uint8_t>(swingSyncLevel(recipe.swingInterval));
	writeSongKey(song, {recipe.keyRoot, recipe.scale});
	song->inClipMinderViewOnLoad = false;
	song->lastClipInstanceEnteredStartPos = -1; // session playback, never arrangement (spec §12.1)

	const Genre& genre = *recipe.genre;
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()) && p < kMaxParts; p++) {
		int32_t missing = 0;
		error = buildPartOutput(song, genre, p, sounds, palette, &roster->parts[p], chunks, &missing);
		if (soundsMissing != nullptr) {
			*soundsMissing += missing;
		}
		// Each Output is built and linked in one go; the job yields and checks for cancel between them.
		if (error == Error::NONE && chunks.boundary()) {
			error = Error::ABORTED_BY_USER;
		}
		if (error != Error::NONE) {
			*roster = Roster{};
			deletePrivateSong(song);
			return error;
		}
	}
	*songOut = song;
	return Error::NONE;
}

Error buildPartOutput(Song* song, const Genre& genre, int32_t part, const SoundPicks& sounds, const Palette& palette,
                      PartOutput* out, Chunks& chunks, int32_t* soundsMissing) {
	*out = PartOutput{};
	CardFolders folders(&chunks);
	Build build{.song = song, .sounds = sounds, .palette = palette, .folders = folders, .chunks = chunks};
	const PartSpec& spec = genre.parts[part];
	const Error error = spec.lanes.empty() ? loadSynth(build, part, spec, out) : buildKit(build, spec, out);
	if (soundsMissing != nullptr) {
		*soundsMissing = build.missing;
	}
	return error;
}

bool partSilent(const PartOutput& part) {
	if (part.output == nullptr) {
		return true;
	}
	if (part.output->type != OutputType::KIT) {
		// Stock skips rendering a Sound only once its voices (release included), delay repeats, stutter, arp and mod
		// FX tail are done. Its reverb is the song's send, which rings on without it.
		return static_cast<SoundInstrument*>(part.output)->skippingRendering;
	}
	// A Kit runs its own FX chain every buffer, so it is silent only once no drum renders, its own delay has no
	// repeats left, it isn't stuttering, its arp holds no notes, and no non-audio drum has a voice (spec §12.9).
	auto* kit = static_cast<Kit*>(part.output);
	if (kit->drumsWithRenderingActive.getNumElements() != 0 || kit->delay.repeatsUntilAbandon != 0
	    || stutterer.isStuttering(static_cast<ModControllableAudio*>(kit))) {
		return false;
	}
	ArpeggiatorSettings* arp = part.clip != nullptr ? &part.clip->arpSettings : nullptr; // the Kit's, as stock reads it
	if (arp != nullptr && arp->mode != ArpMode::OFF
	    && (kit->arpeggiator.hasAnyInputNotesActive() || kit->arpeggiator.hasPendingNotes(arp))) {
		return false;
	}
	for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
		if (drum->type != DrumType::SOUND && static_cast<NonAudioDrum*>(drum)->hasActiveVoices()) {
			return false;
		}
	}
	return true;
}

void freePartOutput(Song* song, PartOutput& part) {
	InstrumentClip* clip = part.clip;
	if (clip == nullptr) {
		part = PartOutput{};
		return;
	}
	// Nothing may point at the Output once it's gone (spec §12.9). Perform keeps the undo log empty, and the view
	// moves the gold knobs off a swapped-out Output; if they are still on it, they let go here.
	auto* instrument = static_cast<Instrument*>(part.output);
	ModControllable* onKnobs = view.activeModControllableModelStack.modControllable;
	bool knobsOnIt = onKnobs != nullptr && onKnobs == instrument->toModControllable();
	for (Drum* drum : part.drums) {
		knobsOnIt = knobsOnIt || (drum != nullptr && onKnobs == drum->toModControllable());
	}
	if (knobsOnIt) {
		view.setActiveModControllableTimelineCounter(nullptr); // the view binds them again
	}
	if (view.instrumentPressedForMIDILearn == instrument) {
		view.instrumentPressedForMIDILearn = nullptr;
	}
	if (view.kitPressedForMIDILearn == instrument) {
		view.kitPressedForMIDILearn = nullptr;
	}
	for (Drum* drum : part.drums) {
		if (drum != nullptr && view.drumPressedForMIDILearn == drum) {
			view.drumPressedForMIDILearn = nullptr;
		}
	}
	{
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		clip->stopAllNotesPlaying(setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip));
	}
	midiFollow.removeClip(clip);
	if (song->getCurrentClip() == clip) {
		song->setCurrentClip(nullptr); // as Song::removeSessionClip() does
	}
	if (song->getSyncScalingClip() == clip) {
		song->setInputTickScaleClip(nullptr);
	}
	// The stock order, as SessionView::removeClip(): the clip detaches from its Output (yielding once per Kit row
	// while it unassigns its drums), and only then is the Output unlinked and deleted. Never hibernated: DELETE.
	const int32_t index = song->sessionClips.getIndexForClip(clip);
	if (index >= 0) {
		song->removeSessionClipLowLevel(clip, index);
	}
	song->deleteClipObject(clip, false, InstrumentRemoval::DELETE);
	AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
	part = PartOutput{};
}

void deleteUnusedAudioFiles() {
	AudioFileVector& files = audioFileManager.audioFiles;
	for (int32_t i = files.getNumElements() - 1; i >= 0; i--) {
		auto* audioFile = static_cast<AudioFile*>(files.getElement(i));
		bool tempRecording = audioFile->type == AudioFileType::SAMPLE
		                     && !static_cast<Sample*>(audioFile)->tempFilePathForRecording.isEmpty();
		if (audioFile->numReasonsToBeLoaded == 0 && !tempRecording) {
			audioFileManager.deleteUnusedAudioFileFromMemory(*audioFile, i);
		}
	}
}

void writeSongKey(Song* song, Key key) {
	uint16_t bits = 0;
	for (uint8_t semitone : kScaleSemitones[static_cast<int32_t>(key.scale)]) {
		bits |= static_cast<uint16_t>(1U << semitone);
	}
	song->key.rootNote = key.root;
	song->key.modeNotes = NoteSet(bits);
}

void deletePrivateSong(Song* song) {
	void* memory = dynamic_cast<void*>(song);
	song->~Song();
	delugeDealloc(memory);
}

} // namespace deluge::generative::firmware
