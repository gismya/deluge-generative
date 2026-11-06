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
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/timer_count.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/storage_manager.h"
#include <new>

namespace deluge::generative::firmware {

PerformAdapter performAdapter{};

namespace {

#if ENABLE_SYSEX_TEST
/// H1 (spec §20): the cost of the step decisions in the tick. Reported as a test event every 4 bars, then reset.
class DecisionTiming final : public perform::DecisionTimer {
public:
	void decisionsBegin() override { started_ = timerCount(); }
	void decisionsEnd(int32_t decided) override {
		uint32_t counts = timerCount() - started_;
		steps_ += decided;
		totalCounts_ += counts;
		uint32_t perStep = counts / decided;
		maxCounts_ = perStep > maxCounts_ ? perStep : maxCounts_;
		if (steps_ >= 4 * kStepsPerBar) {
			TEST_EVENT("gen_h1 steps=%lu mean_ns=%lu max_ns=%lu", static_cast<unsigned long>(steps_),
			           static_cast<unsigned long>(totalCounts_ / steps_ * kNanosecondsPerTimerCount),
			           static_cast<unsigned long>(maxCounts_ * kNanosecondsPerTimerCount));
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

Error PerformAdapter::enter() {
	if (active() || !currentRecipe.exists) {
		return Error::UNSPECIFIED;
	}

	// Build privately while the player's song plays on (spec §14.3). The build runs inline here; the runner slice
	// moves it onto a modal runner job with BACK to cancel.
	Song* song = nullptr;
	Roster roster;
	bool soundMissing = false;
	Error error = buildPerformSong(currentRecipe.recipe, &song, &roster, &soundMissing);
	if (error != Error::NONE) {
		return error;
	}

	// Two Generator blocks, from the internal heap, once per entry (spec §2.1).
	for (Generator*& block : blocks_) {
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Generator));
		if (memory == nullptr) {
			for (Generator*& allocated : blocks_) {
				if (allocated != nullptr) {
					delugeDealloc(allocated);
					allocated = nullptr;
				}
			}
			deletePrivateSong(song);
			return Error::INSUFFICIENT_RAM;
		}
		block = new (memory) Generator();
	}

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
	// The swap leaves its loading mode and animation up for the caller to clear, as Clear song does.
	currentUIMode = UI_MODE_NONE;
	display->removeWorkingAnimation();

	sink_ = OutputSink{};
	sink_.roster = roster;
	queue_.clear();
	generator_ = blocks_[0];
	if (soundMissing) {
		// One warning however many sounds fell back: a part on the default synth or a silent lane (spec §10.1).
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
		TEST_EVENT("gen_sound_missing");
	}
	return Error::NONE;
}

void PerformAdapter::leave() {
	if (!active()) {
		return;
	}
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback(); // the view's playbackEnded() stops the piece
	}
	if (driver_.running()) {
		stopPiece();
	}
	for (Generator*& block : blocks_) {
		block->~Generator();
		delugeDealloc(block);
		block = nullptr;
	}
	generator_ = nullptr;
	sink_ = OutputSink{};
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
	if (!active()) {
		return INT32_MAX;
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
	if (active()) {
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
