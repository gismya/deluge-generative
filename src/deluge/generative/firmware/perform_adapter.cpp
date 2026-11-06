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

#include "generative/firmware/perform_adapter.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/runner.h"
#include "gui/ui/ui.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <new>

extern void setUIForLoadedSong(Song* song);

namespace deluge::generative::firmware {

PerformAdapter performAdapter{};

namespace {

#if ENABLE_SYSEX_TEST
/// Nanoseconds in a span of getSystemTicks() (33.33 MHz).
unsigned long ticksToNs(uint32_t ticks) {
	return static_cast<unsigned long>(uint64_t{ticks} * 1'000'000'000 / DELUGE_CLOCKS_PER);
}

/// H1 (spec §20): the cost of the step decisions in the tick. Reported as a test event every 4 bars, then reset.
class DecisionTiming final : public perform::DecisionTimer {
public:
	void decisionsBegin() override { started_ = getSystemTicks(); }
	void decisionsEnd(int32_t decided) override {
		uint32_t counts = getSystemTicks() - started_;
		steps_ += decided;
		totalCounts_ += counts;
		uint32_t perStep = counts / decided;
		maxCounts_ = perStep > maxCounts_ ? perStep : maxCounts_;
		if (steps_ >= 4 * kStepsPerBar) {
			TEST_EVENT("gen_h1 steps=%lu mean_ns=%lu max_ns=%lu", static_cast<unsigned long>(steps_),
			           ticksToNs(totalCounts_ / steps_), ticksToNs(maxCounts_));
			steps_ = totalCounts_ = maxCounts_ = 0;
		}
	}

private:
	uint32_t started_ = 0;
	uint32_t steps_ = 0;
	uint32_t totalCounts_ = 0;
	uint32_t maxCounts_ = 0;
};
DecisionTiming decisionTiming;

/// Free bytes on the heaps Perform uses, for the leave check (spec §13.8: memory after leaving matches memory before
/// entry). The stealable region holds the sample cache, so it's reported apart.
void reportFreeMemory(const char* when) {
	auto& allocator = GeneralMemoryAllocator::get();
	auto freeIn = [&](int32_t region) {
		uint32_t total = 0;
		auto& spaces = allocator.regions[region].emptySpaces;
		for (int32_t i = 0; i < spaces.getNumElements(); i++) {
			total += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(i))->length;
		}
		return static_cast<unsigned long>(total);
	};
	TEST_EVENT("gen_mem %s internal=%lu external=%lu stealable=%lu", when,
	           freeIn(MEMORY_REGION_INTERNAL) + freeIn(MEMORY_REGION_INTERNAL_SMALL),
	           freeIn(MEMORY_REGION_EXTERNAL) + freeIn(MEMORY_REGION_EXTERNAL_SMALL), freeIn(MEMORY_REGION_STEALABLE));
}

/// Milliseconds since a getSystemTicks() reading.
unsigned long msSince(uint32_t start) {
	return static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000));
}
#endif

} // namespace

// ---- OutputSink ----

namespace {
/// The ModelStack that plays `drum` on its row of `clip`, or nullptr if the drum has no row.
ModelStackWithThreeMainThings* drumModelStack(ModelStackWithTimelineCounter* clipStack, InstrumentClip* clip,
                                              Drum* drum) {
	int32_t rowIndex = -1;
	NoteRow* noteRow = drum ? clip->getNoteRowForDrum(drum, &rowIndex) : nullptr;
	if (noteRow == nullptr) {
		return nullptr;
	}
	return clipStack->addNoteRow(rowIndex, noteRow)
	    ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
}
} // namespace

void OutputSink::play(const Event& event) {
	switch (event.kind) {
	case EventKind::NoteOn:
		note(event, true);
		break;
	case EventKind::NoteOff:
		note(event, false);
		break;
	case EventKind::Section:
		section = event.section;
		occurrence = event.occurrence;
		sectionKnown = true;
		TEST_EVENT("gen_section %s %u", sectionName(event.section), static_cast<unsigned>(event.occurrence));
		break;
	case EventKind::KeyScale:
		// A resolved key or scale change: the Current Recipe and Perform's song key follow it (spec §12.8).
		currentRecipe.recipe.keyRoot = event.keyRoot;
		currentRecipe.recipe.scale = event.scale;
		writeSongKey(currentSong, event.keyRoot, event.scale);
		TEST_EVENT("gen_key root=%u scale=%u", static_cast<unsigned>(event.keyRoot),
		           static_cast<unsigned>(event.scale));
		break;
	case EventKind::Control: // sound controls come with their own slice
		break;
	}
}

void OutputSink::note(const Event& event, bool on) {
	if (event.part >= kMaxParts) {
		return;
	}
	PartOutput& part = roster.parts[event.part];
	if (part.output == nullptr || part.clip == nullptr) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(part.clip);
	int16_t mpeValues[kNumExpressionDimensions] = {};

	if (event.lane != Lane::None) {
		// Every Kit has a row per lane in the fixed order, so the lane's kind gives its drum (spec §5.4, §12.8).
		Drum* drum = part.drums[laneRow(event.lane)];
		ModelStackWithThreeMainThings* withDrum = drumModelStack(modelStack, part.clip, drum);
		if (withDrum == nullptr) {
			return;
		}
		auto* kit = static_cast<Kit*>(part.output);
		if (on) {
			kit->noteOnPreKitArp(withDrum, drum, event.velocity, mpeValues);
		}
		else {
			kit->noteOffPreKitArp(withDrum, drum);
		}
		return;
	}

	auto* synth = static_cast<SoundInstrument*>(part.output);
	ModelStackWithThreeMainThings* withSynth =
	    modelStack->addNoteRow(event.pitch, nullptr)
	        ->addOtherTwoThings(synth->toModControllable(), &part.clip->paramManager);
	static_cast<MelodicInstrument*>(synth)->sendNote(withSynth, on, event.pitch, on ? mpeValues : nullptr,
	                                                 MIDI_CHANNEL_NONE, event.velocity);
}

void OutputSink::allNotesOff() {
	for (PartOutput& part : roster.parts) {
		if (part.output == nullptr || part.clip == nullptr) {
			continue;
		}
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(part.clip);
		if (part.output->type == OutputType::KIT) {
			// No Output-level allNotesOff: Kits need it per drum (spec §12.5).
			for (Drum* drum : part.drums) {
				ModelStackWithThreeMainThings* withDrum = drumModelStack(modelStack, part.clip, drum);
				if (withDrum != nullptr && drum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(drum);
					soundDrum->allNotesOff(withDrum, soundDrum->getArp());
				}
			}
		}
		else {
			auto* synth = static_cast<SoundInstrument*>(part.output);
			synth->allNotesOff(
			    modelStack->addOtherTwoThingsButNoNoteRow(synth->toModControllable(), &part.clip->paramManager),
			    synth->getArp());
		}
	}
}

// ---- PerformAdapter ----

// ---- Entry and leaving, as runner jobs (spec §12.10) ----

/// Perform entry (spec §14.3): modal; BACK cancels it until the swap.
class EntryJob final : public Job {
public:
	EntryJob() : Job(Kind::Modal) {}
	EntryEnded ended = nullptr;

	JobOutcome run(Chunks& chunks) override {
		committed_ = false;
		error_ = Error::NONE;
		Song* song = nullptr;
		Roster roster;
		error_ = buildPerformSong(currentRecipe.recipe, &song, &roster, chunks);
		if (error_ == Error::ABORTED_BY_USER) {
			return JobOutcome::Cancelled;
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		error_ = performAdapter.allocateBlocks();
		if (error_ == Error::NONE && chunks.boundary()) {
			error_ = Error::ABORTED_BY_USER;
		}
		if (error_ != Error::NONE) {
			performAdapter.freeBlocks();
			deletePrivateSong(song);
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}
		committed_ = true; // no yield from here to the swap's end
		performAdapter.swapIn(song, roster);
		return JobOutcome::Done;
	}

	[[nodiscard]] bool cancellable() const override { return !committed_; }

	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		// H9 (spec §20): from the OK press to Perform's song swapped in.
		TEST_EVENT("gen_entry %s ms=%lu",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           msSince(submitted));
#endif
		if (ended != nullptr) {
			ended(outcome, error_);
		}
	}

	uint32_t submitted = 0;

private:
	bool committed_ = false;
	Error error_ = Error::NONE;
};

/// Tearing Perform's song down on leave (spec §12.10, §13.8): modal and never cancelled. With nothing kept it lands
/// on a new blank song in session view, the same as SHIFT+LOAD → NEW, which this follows (`ClearSong`) with the
/// transport stopped.
class LeaveJob final : public Job {
public:
	LeaveJob() : Job(Kind::Modal) {}
	LeaveEnded ended = nullptr;

	JobOutcome run(Chunks& /*chunks*/) override {
		// No yield from the delete to the swap: nothing may run without a current song but the card loader's waits,
		// as in Clear song.
		actionLogger.deleteAllLogs();
		nullifyUIs();
#if ENABLE_SYSEX_TEST
		uint32_t started = getSystemTicks();
#endif
		deleteOldSongBeforeLoadingNew(); // Perform's song: every Output of both rosters
		performAdapter.freeBlocks();
#if ENABLE_SYSEX_TEST
		deleteCounts = getSystemTicks() - started;
#endif

		void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
		if (songMemory == nullptr) {
			// As Clear song: the old song is gone and the UIs are nullified, so there's nothing to resume into.
			FREEZE_WITH_ERROR("E458");
			while (true) {}
		}
		preLoadedSong = new (songMemory) Song();
		preLoadedSong->paramManager.setupUnpatched();
		GlobalEffectable::initParams(&preLoadedSong->paramManager);
		preLoadedSong->setupDefault();
		preLoadedSong->inClipMinderViewOnLoad = false; // session view (spec §13.8), not stock NEW's clip view
		preLoadedSong->ensureAtLeastOneSessionClip();  // loads a synth preset from the card
		playbackHandler.doSongSwap(false);
		audioFileManager.deleteAnyTempRecordedSamplesFromMemory();
		currentSong->loadAllSamples();
		return JobOutcome::Done;
	}

	[[nodiscard]] bool cancellable() const override { return false; }

	void finished(JobOutcome outcome) override {
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		display->removeWorkingAnimation();
#if ENABLE_SYSEX_TEST
		TEST_EVENT("gen_left ms=%lu delete_us=%lu", msSince(submitted), ticksToNs(deleteCounts) / 1000);
		reportFreeMemory("after_leave");
#endif
		if (ended != nullptr) {
			ended();
		}
	}

	uint32_t submitted = 0;
#if ENABLE_SYSEX_TEST
	uint32_t deleteCounts = 0;
#endif
};

namespace {
EntryJob entryJob;
LeaveJob leaveJob;
} // namespace

bool PerformAdapter::beginEntry(EntryEnded ended) {
	if (active() || runner.busy() || !currentRecipe.exists) {
		return false;
	}
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_entry");
#endif
	entryJob.ended = ended;
	entryJob.submitted = getSystemTicks();
	runner.submit(entryJob);
	return true;
}

Error PerformAdapter::allocateBlocks() {
	// Two Generator blocks, from the internal heap, once per entry (spec §2.1).
	for (Generator*& block : blocks_) {
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Generator));
		if (memory == nullptr) {
			freeBlocks();
			return Error::INSUFFICIENT_RAM;
		}
		block = new (memory) Generator();
	}
	return Error::NONE;
}

void PerformAdapter::freeBlocks() {
	for (Generator*& block : blocks_) {
		if (block != nullptr) {
			block->~Generator();
			delugeDealloc(block);
			block = nullptr;
		}
	}
}

void PerformAdapter::swapIn(Song* song, const Roster& roster) {
	// Swap it in stopped (spec §14.3), as Clear song does. Perform keeps the undo log empty from entry (spec §12.9).
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback();
	}
	// RECORD armed in a stock view would stay on, and Perform can't turn it off (F9): disarm it.
	playbackHandler.recording = RecordingMode::OFF;
	playbackHandler.setLedStates();
	actionLogger.deleteAllLogs();
	nullifyUIs();
	deleteOldSongBeforeLoadingNew();
	preLoadedSong = song;
	playbackHandler.doSongSwap(false);
	audioFileManager.deleteAnyTempRecordedSamplesFromMemory(); // the old song's, as Clear song does
	// The swap leaves its loading mode and animation up for the caller to clear, as Clear song does.
	currentUIMode = UI_MODE_NONE;
	display->removeWorkingAnimation();

	sink_ = OutputSink{};
	sink_.roster = roster;
	queue_.clear();
	song_ = song;
	generator_ = blocks_[0];
}

void PerformAdapter::leave(LeaveEnded ended) {
	if (!active()) {
		return;
	}
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback(); // the view's playbackEnded() stops the piece
	}
	stopPiece();
	// From here the tick plays nothing; the blocks and the roster go with the teardown.
	generator_ = nullptr;
	song_ = nullptr;
	sink_ = OutputSink{};
	leaveJob.ended = ended;
	leaveJob.submitted = getSystemTicks();
	runner.cancelAllThen(leaveJob);
}

void PerformAdapter::startPiece(int32_t swungTick) {
	// Every start is fresh: the Arc resets and a new piece begins (spec §13.2).
	generator_->start(currentRecipe.recipe, currentRecipe.seed, currentRecipe.takes);
	queue_.clear();
	sink_.sectionKnown = false;
	driver_.begin(*generator_);
#if ENABLE_SYSEX_TEST
	driver_.setDecisionTimer(&decisionTiming);
#endif
	lastSwungTick_ = swungTick; // the first delta is 0: no catch-up burst when the hook first runs late
}

void PerformAdapter::stopPiece() {
	if (driver_.running()) {
		driver_.stop(sink_);
	}
	sink_.allNotesOff();
}

int32_t PerformAdapter::tick(int32_t swungTick) {
	if (!active() || currentSong != song_) {
		return INT32_MAX; // the hook runs for every song; only Perform's plays the Generator
	}
	if (driver_.running() && swungTick < lastSwungTick_) {
		stopPiece(); // playback restarted without a stop: start over
	}
	if (!driver_.running()) {
		startPiece(swungTick); // step 0's grid line is where the hook first runs after PLAY
	}
	int32_t delta = swungTick - lastSwungTick_;
	lastSwungTick_ = swungTick;
	return driver_.advance(delta, currentSong->getInputTickMagnitude(), queue_, nullptr, sink_);
}

void PerformAdapter::playbackEnded() {
	if (active() && currentSong == song_) {
		stopPiece();
	}
}

void PerformAdapter::setMacro(Macro macro, int32_t units) {
	currentRecipe.recipe.macros.set(macro, units);
	if (active()) {
		queue_.push({CommandKind::SetMacro, static_cast<uint8_t>(macro), currentRecipe.recipe.macros[macro]});
	}
}

bool PerformAdapter::queueKeyChange(uint8_t keyRoot, Scale scale) {
	if (!active()) {
		return false;
	}
	return queue_.push({CommandKind::SetKey, keyRoot, static_cast<int32_t>(scale)});
}

bool PerformAdapter::keyChangePending() const {
	return active() && generator_->keyChangePending();
}

} // namespace deluge::generative::firmware
