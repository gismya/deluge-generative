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

#include "generative/perform/perform_driver.h"
#include "io/midi/midi_follow.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_holder.h"
#include "storage/multi_range/multi_range.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <algorithm>
#include <limits>

namespace deluge::generative::perform {

PerformDriver performDriver{};

namespace {

// POC sound picks. Fixed here until palettes exist. One factory sample per lane, in Lane order.
constexpr char const* kLaneSamplePaths[kNumLanes] = {
    "SAMPLES/DRUMS/Kick/909 Kick.wav",
    "SAMPLES/DRUMS/Clap/909 Clap.WAV",
    "SAMPLES/DRUMS/HatC/909 Closed hihat.wav",
    "SAMPLES/DRUMS/HatO/909 Open hihat.wav",
};
constexpr char const* kLaneNames[kNumLanes] = {"Kick", "Clap", "Closed hat", "Open hat"};
constexpr char const* kKitName = "GEN DRUMS";
constexpr char const* kBassPresetFolder = "SYNTHS";
constexpr char const* kBassPresetName = "047 Basic Dirty Bass";
constexpr char const* kBassPresetPath = "SYNTHS/047 Basic Dirty Bass.XML";
constexpr float kTempoBPM = 130.f;
/// Samples shorter than this play once, as the sample browser does.
constexpr uint32_t kOneShotMaxLengthMs = 2002;

/// The Generator counts 96 ticks per quarter note. The song may run at another tick magnitude.
constexpr int64_t kGeneratorTicksPerQuarter = 4 * kTicksPerStep;

int64_t toGeneratorTicks(int64_t firmwareTicks, int64_t firmwareTicksPerQuarter) {
	return (firmwareTicks * kGeneratorTicksPerQuarter) / firmwareTicksPerQuarter;
}

int64_t toFirmwareTicksRoundingUp(int64_t generatorTicks, int64_t firmwareTicksPerQuarter) {
	int64_t product = generatorTicks * firmwareTicksPerQuarter;
	int64_t result = product / kGeneratorTicksPerQuarter;
	if (product > 0 && product % kGeneratorTicksPerQuarter) {
		result++;
	}
	return result;
}

/// Deletes an Instrument that was never linked into the song's Output list.
void deleteUnlinkedInstrument(Song* song, Instrument* instrument) {
	instrument->prepareForHibernationOrDeletion();
	song->deleteOutput(instrument);
}

/// Assembles the Drums kit from one card sample per lane (#28). The Kit stays out of the song's Output list.
/// Its ParamManager and each drum's are backed up in `song`, ready for InstrumentClip::setAudioInstrument.
Kit* buildDrumsKit(Song* song, Drum* drumsOut[kNumLanes], Error* error) {
	ParamManagerForTimeline kitParamManager;
	Kit* kit = static_cast<Kit*>(StorageManager::createNewInstrument(OutputType::KIT, &kitParamManager));
	if (kit == nullptr) {
		*error = Error::INSUFFICIENT_RAM;
		return nullptr;
	}
	kit->name.set(kKitName);
	kit->dirPath.set("KITS");
	song->backUpParamManager(kit, nullptr, &kitParamManager, true);

	for (int32_t lane = 0; lane < kNumLanes; lane++) {
		ParamManagerForTimeline paramManager;
		*error = paramManager.setupWithPatching();
		if (*error != Error::NONE) {
			goto fail;
		}

		SoundDrum* drum = static_cast<SoundDrum*>(StorageManager::createNewDrum(DrumType::SOUND));
		if (drum == nullptr) {
			*error = Error::INSUFFICIENT_RAM;
			goto fail;
		}
		Sound::initParams(&paramManager);
		kit->addDrum(drum); // From here the Kit owns it and frees it on failure.
		drum->setupAsSample(&paramManager);
		drum->drumName = kLaneNames[lane]; // Unique within the kit.
		song->backUpParamManager(drum, nullptr, &paramManager, true);

		// Don't use Source::setOscType(): it writes soundEditor state, and setupAsSample() already set the type.
		Source* source = &drum->sources[0];
		MultiRange* range = source->getOrCreateFirstRange();
		if (range == nullptr) {
			*error = Error::INSUFFICIENT_RAM;
			goto fail;
		}
		AudioFileHolder* holder = range->getAudioFileHolder();
		*error = holder->filePath.set(kLaneSamplePaths[lane]);
		if (*error != Error::NONE) {
			goto fail;
		}
		// manuallySelected = false, so the sample browser's last-path state is left alone.
		*error = holder->loadFile(false, false, true, CLUSTER_ENQUEUE);
		if (*error == Error::NONE && holder->audioFile == nullptr) {
			*error = Error::FILE_NOT_FOUND;
		}
		if (*error != Error::NONE) {
			goto fail;
		}

		auto* sample = static_cast<Sample*>(holder->audioFile);
		source->repeatMode =
		    (sample->getLengthInMSec() < kOneShotMaxLengthMs) ? SampleRepeatMode::ONCE : SampleRepeatMode::CUT;
		if (lane == static_cast<int32_t>(Lane::Kick)) {
			drum->sideChainSendLevel = 2147483647; // As the sample browser does for kicks.
		}
		if (lane == static_cast<int32_t>(Lane::ClosedHat) || lane == static_cast<int32_t>(Lane::OpenHat)) {
			drum->polyphonic = PolyphonyMode::CHOKE; // The hats share the kit's one choke group (#12).
		}
		drumsOut[lane] = drum;

		AudioEngine::routineWithClusterLoading(); // Give audio a turn between lanes.
	}
	return kit;

fail:
	deleteUnlinkedInstrument(song, kit);
	return nullptr;
}

/// Loads the Bass synth from a factory preset. The synth stays out of the song's Output list.
SoundInstrument* loadBassSynth(Song* song, Error* error) {
	FilePointer filePointer;
	if (!StorageManager::fileExists(kBassPresetPath, &filePointer)) {
		*error = Error::FILE_NOT_FOUND;
		return nullptr;
	}
	String name;
	String folder;
	name.set(kBassPresetName);
	folder.set(kBassPresetFolder);

	Instrument* instrument = nullptr;
	*error = StorageManager::loadInstrumentFromFile(song, nullptr, OutputType::SYNTH, false, &instrument, &filePointer,
	                                                &name, &folder);
	if (*error != Error::NONE || instrument == nullptr) {
		if (*error == Error::NONE) {
			*error = Error::UNSPECIFIED;
		}
		return nullptr;
	}
	instrument->loadAllAudioFiles(true);

	auto* synth = static_cast<SoundInstrument*>(instrument);
	// Factory presets can carry an arp and portamento. The Generator owns the notes, so both go off (#34).
	synth->defaultArpSettings.preset = ArpPreset::OFF;
	synth->defaultArpSettings.mode = ArpMode::OFF;
	return synth;
}

/// Gives `instrument` an empty, always-active, looping 1-bar session clip and links it into the song.
/// On failure the instrument is deleted and nothing is left in the song.
InstrumentClip* linkWithClip(Song* song, Instrument* instrument, Error* error) {
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	if (memory == nullptr) {
		*error = Error::INSUFFICIENT_RAM;
		deleteUnlinkedInstrument(song, instrument);
		return nullptr;
	}
	auto* clip = new (memory) InstrumentClip(song);
	clip->loopLength = song->getBarLength(); // Before setAudioInstrument(): the ParamManager is trimmed to it.
	clip->activeIfNoSolo = true;
	int32_t numClips = song->sessionClips.getNumElements();
	if (numClips > 0) {
		clip->section = song->sessionClips.getClipAtIndex(numClips - 1)->section;
	}

	*error = clip->setAudioInstrument(instrument, song, true, nullptr);
	if (*error != Error::NONE) {
		clip->~InstrumentClip();
		delugeDealloc(memory);
		deleteUnlinkedInstrument(song, instrument);
		return nullptr;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);
	if (instrument->type == OutputType::KIT) {
		clip->setupAsNewKitClipIfNecessary(modelStack); // One NoteRow per drum, in drum (= Lane) order.
	}
	else {
		// Portamento off for a part that doesn't slide (#34).
		clip->paramManager.getUnpatchedParamSet()
		    ->params[deluge::modulation::params::UNPATCHED_PORTAMENTO]
		    .setCurrentValueBasicForSetup(std::numeric_limits<int32_t>::min());
	}
	clip->arpSettings.preset = ArpPreset::OFF;
	clip->arpSettings.mode = ArpMode::OFF;

	*error = song->sessionClips.insertClipAtIndex(clip, numClips);
	if (*error != Error::NONE) {
		song->deleteClipObject(clip, false, InstrumentRemoval::NONE);
		deleteUnlinkedInstrument(song, instrument);
		return nullptr;
	}
	song->addOutput(instrument, false);
	instrument->setActiveClip(modelStack);
	if (playbackHandler.isEitherClockActive() && currentPlaybackMode == &session) {
		session.reSyncClip(modelStack, true);
	}
	return clip;
}

/// Removes a clip that linkWithClip() added, and its Output with it.
void unlinkClip(Song* song, InstrumentClip* clip) {
	int32_t index = song->sessionClips.getIndexForClip(clip);
	if (index < 0) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	clip->stopAllNotesPlaying(setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip));
	midiFollow.removeClip(clip);
	// removeSessionClip() deletes the clip's Output too, since it has no other clips. An Output marked as edited
	// would be hibernated in the song instead; these are throwaway, so make sure it's deleted.
	static_cast<Instrument*>(clip->output)->editedByUser = false;
	song->removeSessionClip(clip, index);
}

} // namespace

Recipe defaultRecipe() {
	Recipe recipe;
	recipe.key = 9; // A
	recipe.scale = Scale::Minor;
	recipe.energy = 25;
	recipe.density = 25;
	recipe.complexity = 25;
	recipe.motion = 25;
	return recipe;
}

// ---- OutputSink ----

Drum* OutputSink::drumForLane(uint8_t lane, int32_t* noteRowIndex) {
	if (lane >= kNumLanes || kitClip == nullptr || drums[lane] == nullptr) {
		return nullptr;
	}
	// The kit clip has one NoteRow per lane in Lane order, but look the row up in case it moved.
	NoteRow* noteRow = kitClip->getNoteRowForDrum(drums[lane], noteRowIndex);
	return noteRow ? drums[lane] : nullptr;
}

void OutputSink::noteOn(int32_t /*tick*/, PartId part, uint8_t lane, int16_t note, uint8_t velocity) {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, currentSong);
	int16_t mpeValues[kNumExpressionDimensions] = {};

	if (part == PartId::Drums) {
		int32_t noteRowIndex = -1;
		Drum* drum = drumForLane(lane, &noteRowIndex);
		if (drum == nullptr) {
			return;
		}
		NoteRow* noteRow = kitClip->noteRows.getElement(noteRowIndex);
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addTimelineCounter(kitClip)
		        ->addNoteRow(noteRowIndex, noteRow)
		        ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
		kit->noteOnPreKitArp(modelStackWithThreeMainThings, drum, velocity, mpeValues);
	}
	else if (part == PartId::Bass && bass != nullptr && bassClip != nullptr) {
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addTimelineCounter(bassClip)
		        ->addNoteRow(note, nullptr)
		        ->addOtherTwoThings(bass->toModControllable(), &bassClip->paramManager);
		static_cast<MelodicInstrument*>(bass)->sendNote(modelStackWithThreeMainThings, true, note, mpeValues,
		                                                MIDI_CHANNEL_NONE, velocity);
	}
}

void OutputSink::noteOff(int32_t /*tick*/, PartId part, uint8_t lane, int16_t note) {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, currentSong);

	if (part == PartId::Drums) {
		int32_t noteRowIndex = -1;
		Drum* drum = drumForLane(lane, &noteRowIndex);
		if (drum == nullptr) {
			return;
		}
		NoteRow* noteRow = kitClip->noteRows.getElement(noteRowIndex);
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addTimelineCounter(kitClip)
		        ->addNoteRow(noteRowIndex, noteRow)
		        ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
		kit->noteOffPreKitArp(modelStackWithThreeMainThings, drum);
	}
	else if (part == PartId::Bass && bass != nullptr && bassClip != nullptr) {
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addTimelineCounter(bassClip)
		        ->addNoteRow(note, nullptr)
		        ->addOtherTwoThings(bass->toModControllable(), &bassClip->paramManager);
		static_cast<MelodicInstrument*>(bass)->sendNote(modelStackWithThreeMainThings, false, note, nullptr);
	}
}

void OutputSink::allNotesOff() {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, currentSong);

	if (kit != nullptr && kitClip != nullptr) {
		for (int32_t lane = 0; lane < kNumLanes; lane++) {
			int32_t noteRowIndex = -1;
			Drum* drum = drumForLane(static_cast<uint8_t>(lane), &noteRowIndex);
			if (drum == nullptr || drum->type != DrumType::SOUND) {
				continue;
			}
			NoteRow* noteRow = kitClip->noteRows.getElement(noteRowIndex);
			ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
			    modelStack->addTimelineCounter(kitClip)
			        ->addNoteRow(noteRowIndex, noteRow)
			        ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
			kit->noteOffPreKitArp(modelStackWithThreeMainThings, drum);
			auto* soundDrum = static_cast<SoundDrum*>(drum);
			soundDrum->allNotesOff(modelStackWithThreeMainThings, soundDrum->getArp());
		}
	}
	if (bass != nullptr && bassClip != nullptr) {
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addTimelineCounter(bassClip)->addOtherTwoThingsButNoNoteRow(bass->toModControllable(),
		                                                                            &bassClip->paramManager);
		bass->allNotesOff(modelStackWithThreeMainThings, bass->getArp());
	}
}

// ---- PerformDriver ----

Error PerformDriver::enter(Song* song) {
	if (armed_) {
		return Error::NONE;
	}
	actionLogger.deleteAllLogs(); // Undo history can't describe the Outputs we add and later remove.

	Error error = Error::NONE;
	Drum* drums[kNumLanes] = {};

	// Card reads first, while nothing is in the song. These yield, so ticks run meanwhile; the hook isn't armed.
	Kit* kit = buildDrumsKit(song, drums, &error);
	if (kit == nullptr) {
		return error;
	}
	SoundInstrument* bass = loadBassSynth(song, &error);
	if (bass == nullptr) {
		deleteUnlinkedInstrument(song, kit);
		return error;
	}

	// Now link everything in. No card access from here on.
	InstrumentClip* kitClip = linkWithClip(song, kit, &error);
	if (kitClip == nullptr) {
		deleteUnlinkedInstrument(song, bass);
		return error;
	}
	InstrumentClip* bassClip = linkWithClip(song, bass, &error);
	if (bassClip == nullptr) {
		unlinkClip(song, kitClip);
		return error;
	}

	sink_.kit = kit;
	sink_.kitClip = kitClip;
	std::copy(std::begin(drums), std::end(drums), std::begin(sink_.drums));
	sink_.bass = bass;
	sink_.bassClip = bassClip;

	song->setBPM(kTempoBPM, false);

	recipe_ = defaultRecipe();
	seed_ = 1;
	running_ = false;
	armed_ = true;
	if (playbackHandler.isEitherClockActive()) {
		playbackHandler.expectEvent(); // Run the hook on the next tick so it can line up with the next bar.
	}
	return Error::NONE;
}

void PerformDriver::exit(Song* song) {
	if (!armed_) {
		return;
	}
	stopGenerator();
	armed_ = false;
	removeRoster(song);
	actionLogger.deleteAllLogs();
}

void PerformDriver::removeRoster(Song* song) {
	if (sink_.bassClip != nullptr) {
		unlinkClip(song, sink_.bassClip);
	}
	if (sink_.kitClip != nullptr) {
		unlinkClip(song, sink_.kitClip);
	}
	sink_ = OutputSink{};
}

void PerformDriver::stopGenerator() {
	if (running_) {
		generator_.stop(sink_);
		running_ = false;
	}
	sink_.allNotesOff();
}

void PerformDriver::transportStopped() {
	if (armed_) {
		stopGenerator();
	}
}

void PerformDriver::setRecipe(const Recipe& recipe) {
	recipe_ = recipe;
	if (running_) {
		generator_.setRecipe(recipe_);
	}
}

int32_t PerformDriver::currentStep() const {
	return running_ ? generator_.currentStep() : -1;
}

int32_t PerformDriver::doTickForward(int64_t swungPos) {
	if (!armed_) {
		return std::numeric_limits<int32_t>::max();
	}
	const int64_t ticksPerQuarter = currentSong->getQuarterNoteLength();

	// Playback restarted without a stop (e.g. a restart from the PLAY button): start over.
	if (running_ && swungPos < lastSwungPos_) {
		stopGenerator();
	}
	lastSwungPos_ = swungPos;

	if (!running_) {
		// Tick 0 is the first bar line at or after now: playback start, or the next bar when entering mid-song.
		const int64_t barLength = currentSong->getBarLength();
		originTick_ = ((swungPos + barLength - 1) / barLength) * barLength;
		generator_.start(recipe_, seed_);
		running_ = true;
	}

	const int64_t generatorNow = toGeneratorTicks(swungPos - originTick_, ticksPerQuarter);
	int64_t generatorNext;
	if (generatorNow < -kLookaheadTicks) {
		// Not yet at step 0's decision point. The Generator's ticks must not go below that.
		generatorNext = -kLookaheadTicks;
	}
	else {
		generatorNext = generatorNow + generator_.tick(static_cast<int32_t>(generatorNow), sink_);
	}

	int64_t firmwareNext = originTick_ + toFirmwareTicksRoundingUp(generatorNext, ticksPerQuarter);
	int64_t wait = std::max<int64_t>(1, firmwareNext - swungPos);
	return static_cast<int32_t>(std::min<int64_t>(wait, std::numeric_limits<int32_t>::max()));
}

} // namespace deluge::generative::perform
