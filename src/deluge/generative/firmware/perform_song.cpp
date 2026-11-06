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
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_holder.h"
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

/// The card failed or was pulled, as opposed to a file that isn't there (spec §10.7).
bool isCardError(Error error) {
	return error == Error::SD_CARD || error == Error::SD_CARD_NOT_PRESENT || error == Error::SD_CARD_NO_FILESYSTEM;
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
	*clipOut = clip;
	return Error::NONE;
}

/// Assembles a Kit with one sample per lane (spec §10.2, §10.4): a row for every lane of its kind in the fixed
/// order, so laneRow() holds. Lanes the Recipe has no sample for are empty, silent rows.
Error buildKit(Song* song, const Recipe& recipe, const PartSpec& part, PartOutput* out, Chunks& chunks) {
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
	for (int32_t row = 0; row < kitRows(fx); row++) {
		Lane lane = laneAtRow(fx, row);
		ParamManagerForTimeline paramManager;
		error = paramManager.setupWithPatching();
		if (error != Error::NONE) {
			break;
		}
		auto* drum = static_cast<SoundDrum*>(StorageManager::createNewDrum(DrumType::SOUND));
		if (drum == nullptr) {
			error = Error::INSUFFICIENT_RAM;
			break;
		}
		Sound::initParams(&paramManager);
		kit->addDrum(drum); // the Kit owns it from here
		drum->setupAsSample(&paramManager);
		drum->drumName = laneName(lane);
		applySyncDefaults(drum, song);
		song->backUpParamManager(drum, nullptr, &paramManager, true);
		out->drums[row] = drum;
		if (lane == Lane::ClosedHat || lane == Lane::OpenHat) {
			drum->polyphonic = PolyphonyMode::CHOKE; // the hats share the Kit's one choke group (spec §5.8)
		}

		// Not Source::setOscType(): it writes soundEditor state, and setupAsSample() already set the type.
		const char* path = recipe.sounds.lane[static_cast<int32_t>(lane)];
		MultiRange* range = drum->sources[0].getOrCreateFirstRange();
		if (path == nullptr || range == nullptr) {
			continue; // a lane the genre doesn't play, or with nothing to load, stays silent
		}
		AudioFileHolder* holder = range->getAudioFileHolder();
		error = holder->filePath.set(path);
		if (error != Error::NONE) {
			break;
		}
		// manuallySelected = false leaves the sample browser's last-path state alone.
		error = holder->loadFile(false, false, true, CLUSTER_ENQUEUE);
		if (chunks.boundary()) {
			error = Error::ABORTED_BY_USER;
			break;
		}
		if (isCardError(error)) {
			break;
		}
		if (error != Error::NONE || holder->audioFile == nullptr) {
			// A missing file isn't a card error: the lane stays silent (spec §10.2). The palette fallback chain and
			// its warning come with the sounds slice.
			error = Error::NONE;
			continue;
		}
		auto* sample = static_cast<Sample*>(holder->audioFile);
		drum->sources[0].repeatMode =
		    sample->getLengthInMSec() < kOneShotMaxLengthMs ? SampleRepeatMode::ONCE : SampleRepeatMode::CUT;
		if (lane == Lane::Kick) {
			drum->sideChainSendLevel = INT32_MAX; // as the sample browser does for kicks
		}
	}
	if (error != Error::NONE) {
		deleteUnlinked(song, kit);
		*out = PartOutput{};
		return error;
	}
	out->output = kit;
	return link(song, kit, &out->clip);
}

/// Loads a synth part's preset (spec §10.1), with its arp cleared.
Error loadSynth(Song* song, const Recipe& recipe, int32_t partIndex, const PartSpec& part, PartOutput* out) {
	const char* path = recipe.sounds.part[partIndex];
	if (path == nullptr) {
		return Error::FILE_NOT_FOUND;
	}
	FilePointer filePointer;
	if (!StorageManager::fileExists(path, &filePointer)) {
		return Error::FILE_NOT_FOUND;
	}
	String folder;
	String name;
	splitPresetPath(path, &folder, &name);
	Instrument* instrument = nullptr;
	Error error = StorageManager::loadInstrumentFromFile(song, nullptr, OutputType::SYNTH, true, &instrument,
	                                                     &filePointer, &name, &folder);
	if (error != Error::NONE) {
		return error;
	}
	if (instrument == nullptr) {
		return Error::UNSPECIFIED;
	}
	auto* synth = static_cast<SoundInstrument*>(instrument);
	synth->defaultArpSettings.preset = ArpPreset::OFF;
	synth->defaultArpSettings.mode = ArpMode::OFF;
	if (part.role == Role::Bass) {
		synth->polyphonic = PolyphonyMode::LEGATO; // the Bass role's voice mode (spec §5.8)
	}
	out->output = synth;
	return link(song, synth, &out->clip);
}

} // namespace

Error buildPerformSong(const Recipe& recipe, Song** songOut, Roster* roster, Chunks& chunks) {
	*roster = Roster{};
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
	// The song's swing interval is a sync level relative to its magnitude: 8 - magnitude is a 16th, and each step
	// down doubles the note value. The Recipe's is a note value, 0 for a 16th (spec §12.3).
	song->swingInterval = static_cast<uint8_t>(8 - kGeneratorTickMagnitude - recipe.swingInterval);
	writeSongKey(song, recipe.keyRoot, recipe.scale);
	song->inClipMinderViewOnLoad = false;
	song->lastClipInstanceEnteredStartPos = -1; // session playback, never arrangement (spec §12.1)

	const Genre& genre = *recipe.genre;
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()) && p < kMaxParts; p++) {
		const PartSpec& part = genre.parts[p];
		error = part.lanes.empty() ? loadSynth(song, recipe, p, part, &roster->parts[p])
		                           : buildKit(song, recipe, part, &roster->parts[p], chunks);
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

void writeSongKey(Song* song, uint8_t keyRoot, Scale scale) {
	uint16_t bits = 0;
	for (uint8_t semitone : kScaleSemitones[static_cast<int32_t>(scale)]) {
		bits |= static_cast<uint16_t>(1U << semitone);
	}
	song->key.rootNote = keyRoot;
	song->key.modeNotes = NoteSet(bits);
}

void deletePrivateSong(Song* song) {
	void* memory = dynamic_cast<void*>(song);
	song->~Song();
	delugeDealloc(memory);
}

} // namespace deluge::generative::firmware
