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

#include "generative/firmware/perform_midi.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/perform/midi_in.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/perform_view.h"
#include "gui/views/view.h"
#include "io/midi/midi_engine.h"
#include "io/midi/midi_follow.h"
#include "io/midi/test_port.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "modulation/params/param_collection.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>

namespace deluge::generative::firmware {

namespace params = deluge::modulation::params;

namespace {

/// The value each followed CC last sent, so a pass sends only what moved (H17).
perform::FeedbackFilter sent;

#if ENABLE_SYSEX_TEST
/// H17's meter: feedback CCs sent in a window of about a second, and the most one pass sent.
uint32_t windowStart = 0;
uint32_t windowCCs = 0;
uint32_t passCCs = 0;
uint32_t peakPassCCs = 0;
#endif

void send(int32_t cc, int32_t knobPos) {
	midiFollow.sendCCForMidiFollowFeedback(cc, knobPos);
#if ENABLE_SYSEX_TEST
	windowCCs++;
	passCCs++;
#endif
}

/// Sends `cc` at `knobPos` if that moved since `cc` last sent.
void sendIfMoved(int32_t cc, int32_t knobPos) {
	if (sent.changed(cc, knobPos + kKnobPosOffset)) {
		send(cc, knobPos);
	}
}

/// A macro's centre as the knob position its CC sends.
int32_t macroKnobPos(Macro macro) {
	return perform::macroCCValue(currentRecipe.recipe.macros[macro]) - kKnobPosOffset;
}

/// Reports each window's feedback load (test builds): CCs, DIN bytes per second at 3 a CC, the busiest pass.
void meterPass() {
#if ENABLE_SYSEX_TEST
	peakPassCCs = std::max(peakPassCCs, passCCs);
	passCCs = 0;
	const uint32_t elapsed = AudioEngine::audioSampleTimer - windowStart;
	if (elapsed < kSampleRate) {
		return;
	}
	// A window the transport left idle reports nothing: its rate would read low.
	if (windowCCs > 0 && elapsed < 2 * kSampleRate) {
		TEST_EVENT("gen_midi_feedback ccs=%lu ms=%lu bytes_per_s=%lu peak_pass=%lu",
		           static_cast<unsigned long>(windowCCs),
		           static_cast<unsigned long>(static_cast<uint64_t>(elapsed) * 1000 / kSampleRate),
		           static_cast<unsigned long>(static_cast<uint64_t>(windowCCs) * 3 * kSampleRate / elapsed),
		           static_cast<unsigned long>(peakPassCCs));
	}
	windowStart = AudioEngine::audioSampleTimer;
	windowCCs = 0;
	peakPassCCs = 0;
#endif
}

} // namespace

Clip* followedClip() {
	if (!view.isClipContext()) {
		return nullptr;
	}
	return static_cast<Clip*>(view.activeModControllableModelStack.getTimelineCounter());
}

ModelStackWithAutoParam* followedSongParam(ModelStackWithTimelineCounter* stack, int32_t globalParamId) {
	// No tempo CC: the Recipe tempo is Perform's (spec §13.7).
	if (globalParamId < 0 || globalParamId >= params::UNPATCHED_GLOBAL_MAX_NUM
	    || globalParamId == params::UNPATCHED_TEMPO) {
		return nullptr;
	}
	stack->setTimelineCounter(currentSong);
	ModelStackWithThreeMainThings* song =
	    stack->addOtherTwoThingsButNoNoteRow(&currentSong->globalEffectable, &currentSong->paramManager);
	return currentSong->getModelStackWithParam(song, globalParamId);
}

bool followCC(int32_t cc, int32_t value) {
	const std::optional<Macro> macro = perform::macroForCC(cc);
	if (!macro.has_value()) {
		return false;
	}
	// The controller has the value already: noted as sent, so it isn't sent back.
	sent.changed(cc, value);
	performView.steerMacroFromMidi(*macro, perform::macroUnitsFromCC(value));
	return true;
}

bool globalMidiCommand(GlobalMIDICommand command, bool on, int32_t note) {
	switch (command) {
	case GlobalMIDICommand::TRANSPOSE:
		// The played note's pitch class becomes the key at the next phrase line; the scale stays. Note-offs do
		// nothing.
		if (on && !gui::refusedWhileChangePending()) {
			const Key key = perform::transposedKey(performAdapter.keyAhead(), note);
			gui::popupIfRefused(performAdapter.queueKeyChange(key));
			TEST_EVENT("gen_midi_command transpose note=%ld", static_cast<long>(note));
		}
		return true;
	case GlobalMIDICommand::PLAYBACK_RESTART:
		if (on) {
			TEST_EVENT("gen_midi_command restart blocked");
		}
		return true;
	case GlobalMIDICommand::LOOP:
	case GlobalMIDICommand::LOOP_CONTINUOUS_LAYERING:
	case GlobalMIDICommand::UNDO:
	case GlobalMIDICommand::REDO:
	case GlobalMIDICommand::NEXT_SONG:
		// Perform has no undo, no loop recording, and its song is never swapped from outside.
		if (on) {
			TEST_EVENT("gen_midi_command %d ignored", static_cast<int>(command));
		}
		return true;
	default:
		return false;
	}
}

void sendFeedback(ModelStackWithAutoParam* stack, int32_t knobPos, bool isAutomation) {
	if (!midiFollow.isFeedbackEnabled()) {
		return;
	}
	Clip* clip = followedClip();
	if (stack != nullptr) {
		// A player edit, sent as stock sends one. A song param's CC is its global mapping.
		if (stack->autoParam == nullptr) {
			return;
		}
		const params::Kind kind = stack->paramCollection->getParamKind();
		int32_t cc = MIDI_CC_NONE;
		if (clip != nullptr) {
			cc = midiFollow.getCCFromParam(kind, stack->paramId);
		}
		else if (kind == params::Kind::UNPATCHED_GLOBAL) {
			cc = midiFollow.globalParamToCC[stack->paramId];
		}
		if (cc != MIDI_CC_NONE && !perform::macroForCC(cc).has_value()) {
			sent.changed(cc, knobPos + kKnobPosOffset);
			send(cc, knobPos);
		}
		return;
	}
	if (!isAutomation) {
		// A new context (focus, entry, a launch): every value goes out again.
		sent.forget();
	}
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* context =
	    setupModelStackWithSong(memory, currentSong)
	        ->addTimelineCounter(clip != nullptr ? static_cast<TimelineCounter*>(clip) : currentSong);
	for (int32_t cc = 0; cc <= kMaxMIDIValue; cc++) {
		if (perform::macroForCC(cc).has_value()) {
			continue;
		}
		// The live value: home plus every layer, as the param plays now.
		ModelStackWithAutoParam* param = midiFollow.getModelStackWithParam(context, clip, midiFollow.ccToSoundParam[cc],
		                                                                   midiFollow.ccToGlobalParam[cc], false);
		if (param == nullptr || param->autoParam == nullptr) {
			continue;
		}
		sendIfMoved(cc, param->paramCollection->paramValueToKnobPos(param->autoParam->getCurrentValue(), param));
	}
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		sendIfMoved(perform::macroCC(macro), macroKnobPos(macro));
	}
	meterPass();
}

void sendMacroFeedback(Macro macro) {
	if (midiFollow.isFeedbackEnabled()) {
		sendIfMoved(perform::macroCC(macro), macroKnobPos(macro));
	}
}

void keepFeedbackRunning() {
	if (midiEngine.midiFollowFeedbackAutomation != MIDIFollowFeedbackAutomationMode::DISABLED
	    && !uiTimerManager.isTimerSet(TimerName::SEND_MIDI_FEEDBACK_FOR_AUTOMATION)) {
		uiTimerManager.setTimer(TimerName::SEND_MIDI_FEEDBACK_FOR_AUTOMATION, 25);
	}
}

} // namespace deluge::generative::firmware
