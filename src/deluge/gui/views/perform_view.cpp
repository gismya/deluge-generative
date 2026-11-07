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

#include "gui/views/perform_view.h"
#include "generative/core/genre.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "gui/colour/palette.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_modal.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "io/midi/test_port.h"
#include "model/mod_controllable/mod_controllable.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "util/d_stringbuf.h"
#include <algorithm>
#include <cstdio>

using deluge::generative::kMacroUnitsPerKnobStep;
using deluge::generative::kTicksPerStep;
using deluge::generative::Macro;
using deluge::generative::macroToPoints;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using deluge::gui::context_menu::leavePerform;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;
/// The End pad: AUDITION/SECTION column, row 8, the bottom row (spec §13.5). Held this long, it arms End.
constexpr int32_t kEndPadX = kDisplayWidth + 1;
constexpr int32_t kEndPadY = 0;
constexpr uint32_t kEndHoldSamples = kSampleRate * 6 / 10;
/// A coarse tempo step: 16 an octave, as stock's metronome values (2^(1/16)).
constexpr float kCoarseTempoStep = 1.0442738f;
/// The upper gold knob is encoder 1, the lower is 0.
constexpr int32_t kUpperKnob = 1;
constexpr int32_t kTicksPerBeat = 4 * kTicksPerStep;

/// Indexed by Macro.
constexpr deluge::l10n::String kMacroNames[] = {
    deluge::l10n::String::STRING_FOR_ENERGY,
    deluge::l10n::String::STRING_FOR_DENSITY,
    deluge::l10n::String::STRING_FOR_COMPLEXITY,
    deluge::l10n::String::STRING_FOR_MOTION,
};

uint8_t* songModKnobMode() {
	ModControllable* modControllable = view.activeModControllableModelStack.modControllable;
	return modControllable ? modControllable->getModKnobMode() : nullptr;
}

} // namespace

bool PerformView::opened() {
	focusRegained();
	return true;
}

void PerformView::focusRegained() {
	// PLAY must start session playback: Perform isn't a ClipMinder, and has no arrangement (spec §12.1).
	currentSong->lastClipInstanceEnteredStartPos = -1;
	currentSong->affectEntire = true;
	view.setActiveModControllableTimelineCounter(currentSong);

	indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::CLIP_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::KEYBOARD, false);
	indicator_leds::setLedState(IndicatorLED::BACK, true);
	updateKnobIndicators();
	releaseEndPad(); // a hold cut short by a menu or overlay never arms End

	shownBeat_ = -1;
	shownSection_ = -1;
	uiNeedsRendering(this);
	renderUIsForOled();
}

ActionResult PerformView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;

	if (b == BACK) {
		return ActionResult::DEALT_WITH; // a tap cancels the newest queued action, once there are any (spec §13.5)
	}
	if (b == TAP_TEMPO) {
		// Stock's TAP handling (SHIFT+TAP = metronome, MIDI learn); the tap itself sets the Recipe tempo (F4).
		return view.buttonAction(b, on, inCardRoutine);
	}
	// PLAY, RECORD, SHIFT and the encoder pushes go to the global handling, which guards RECORD and X+PLAY under
	// Perform (F9). Nothing else reaches stock views from here.
	return ActionResult::NOT_DEALT_WITH;
}

ActionResult PerformView::exitUI() {
	// Without Capture there's only LEAVE PERFORM? (spec §13.8). Playback goes on under it.
	if (currentUIMode == UI_MODE_NONE && leavePerform.setupAndCheckAvailability()) {
		releaseEndPad();
		openUI(&leavePerform);
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::leave() {
	// The screen reacts at once; the teardown runs behind the working animation, once the runner is free (spec
	// §12.10).
	indicator_leds::setLedState(IndicatorLED::BACK, false);
	generativeModalUI.open(false);
	performAdapter.leave([](deluge::generative::JobOutcome outcome) {
		generativeModalUI.jobEnded();
		if (outcome != deluge::generative::JobOutcome::Done) {
			// The teardown's task couldn't start: Perform carries on.
			indicator_leds::setLedState(IndicatorLED::BACK, true);
			display->displayError(Error::UNSPECIFIED);
			return;
		}
		TEST_EVENT("gen_perform_left");
	});
}

bool PerformView::onMacroPage() const {
	uint8_t* mode = songModKnobMode();
	return mode != nullptr && *mode == kMacroModButton;
}

void PerformView::modButtonAction(uint8_t whichButton, bool on) {
	if (whichButton == kMacroModButton) {
		if (on) {
			uint8_t* mode = songModKnobMode();
			if (mode == nullptr) {
				return;
			}
			secondMacroSet_ = (*mode == kMacroModButton) && !secondMacroSet_;
			*mode = kMacroModButton;
			view.setModLedStates();
			updateKnobIndicators();
		}
		return;
	}
	if (on && whichButton == 0 && songModKnobMode() != nullptr && *songModKnobMode() == 0) {
		return; // Perform owns the mod buttons: F1 again never turns the VU meter on (spec §13.5)
	}
	view.modButtonAction(whichButton, on);
	updateKnobIndicators();
}

Macro PerformView::knobMacro(int32_t whichModEncoder) const {
	if (whichModEncoder == kUpperKnob) {
		return secondMacroSet_ ? Macro::Complexity : Macro::Energy;
	}
	return secondMacroSet_ ? Macro::Motion : Macro::Density;
}

void PerformView::updateKnobIndicators() {
	if (!onMacroPage()) {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton]);
		view.setKnobIndicatorLevels();
		return;
	}
	// The knob rings show the centre (spec §13.5). The F8 LED blinks while Complexity / Motion is on.
	const auto& macros = currentRecipe.recipe.macros;
	for (int32_t knob = 0; knob < 2; knob++) {
		indicator_leds::setKnobIndicatorLevel(knob, macros[knobMacro(knob)] / kMacroUnitsPerKnobStep);
	}
	if (secondMacroSet_) {
		indicator_leds::blinkLed(indicator_leds::modLed[kMacroModButton]);
	}
	else {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton], true);
		indicator_leds::setLedState(indicator_leds::modLed[kMacroModButton], true);
	}
}

void PerformView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	if (!onMacroPage()) {
		view.modEncoderAction(whichModEncoder, offset);
		return;
	}
	Macro macro = knobMacro(whichModEncoder);
	int32_t value = currentRecipe.recipe.macros[macro] + offset * kMacroUnitsPerKnobStep;
	performAdapter.setMacro(macro, value);
	updateKnobIndicators();

	char popup[24];
	snprintf(popup, sizeof(popup), "%s %d", deluge::l10n::get(kMacroNames[static_cast<int32_t>(macro)]),
	         static_cast<int>(macroToPoints(currentRecipe.recipe.macros[macro])));
	display->displayPopup(popup);
	renderUIsForOled();
}

ActionResult PerformView::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x == kEndPadX && y == kEndPadY) {
		if (velocity) {
			// Held 0.6 s, it arms End; the pad brightens while held (spec §13.5).
			endHeld_ = true;
			endArmedThisHold_ = false;
			endPressedAt_ = AudioEngine::audioSampleTimer; // graphicsRoutine() arms it once held long enough
		}
		else if (endHeld_) {
			endHeld_ = false;
			if (!endArmedThisHold_) {
				// A tap cancels End while it waits.
				performAdapter.cancelEnd();
				TEST_EVENT("gen_end tap");
			}
		}
		redrawEndPad();
	}
	// The rest of the grid and sidebar are later slices (#109, #110).
	return ActionResult::DEALT_WITH;
}

void PerformView::checkEndHold() {
	if (!endHeld_ || endArmedThisHold_) {
		return;
	}
	if (getCurrentUI() != this) {
		releaseEndPad(); // an overlay opened over the hold: its release would never reach us
		return;
	}
	if (AudioEngine::audioSampleTimer - endPressedAt_ >= kEndHoldSamples) {
		endArmedThisHold_ = true;
		performAdapter.armEnd();
		TEST_EVENT("gen_end armed");
	}
}

void PerformView::releaseEndPad() {
	endHeld_ = false;
	endArmedThisHold_ = false;
	redrawEndPad();
}

void PerformView::redrawEndPad() {
	uiNeedsRendering(this, 0, 1 << kEndPadY);
}

void PerformView::tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed) {
	using namespace deluge::hid::button;
	auto& recipe = currentRecipe.recipe;

	if (Buttons::isButtonPressed(TAP_TEMPO)) {
		// The swing interval, as stock: the first turn shows it.
		if (display->hasPopupOfType(PopupType::SWING)) {
			int32_t interval = std::clamp<int32_t>(recipe.swingInterval + (offset > 0 ? 1 : -1), 0,
			                                       deluge::generative::kMaxSwingInterval);
			performAdapter.setSwingInterval(interval);
		}
		showSwing();
		return;
	}
	offset = std::clamp<int8_t>(offset, -1, 1);
	if (Buttons::isButtonPressed(X_ENC)) {
		playbackHandler.commandNudgeClock(offset); // stock (spec §13.7)
		return;
	}
	if (shiftButtonPressed) {
		if (display->hasPopupOfType(PopupType::SWING)) {
			performAdapter.setSwing(std::clamp<int32_t>(recipe.swing + offset, -deluge::generative::kMaxSwing,
			                                            deluge::generative::kMaxSwing));
		}
		showSwing();
		return;
	}
	if (Buttons::isButtonPressed(LEARN)) {
		return; // F4: the clock-out scale can't change inside Perform (spec §13.7)
	}
	if (playbackHandler.isExternalClockActive()) {
		showTempo(); // marked EXT; the master owns tempo
		return;
	}
	if (display->hasPopupOfType(PopupType::TEMPO)) {
		// Coarse or fine as stock's truth table: the FineTempoKnob setting flips what a push does.
		bool fineSetting =
		    runtimeFeatureSettings.get(RuntimeFeatureSettingType::FineTempoKnob) == RuntimeFeatureStateToggle::On;
		int32_t tempo = recipe.tempo;
		if (fineSetting == encoderButtonPressed) {
			// Coarse: 16 steps an octave, as stock's metronome values.
			tempo = static_cast<int32_t>(
			    static_cast<float>(tempo) * (offset > 0 ? kCoarseTempoStep : 1.0f / kCoarseTempoStep) + 0.5f);
		}
		else {
			tempo = ((tempo + 50) / 100 + offset) * 100; // whole BPM
		}
		setRecipeTempo(tempo);
		return;
	}
	showTempo();
}

void PerformView::setRecipeTempo(int32_t tempo) {
	performAdapter.setTempo(std::clamp(tempo, deluge::generative::kMinTempo, deluge::generative::kMaxTempo));
	showTempo();
}

void PerformView::showTempo() {
	// The stock tempo popup, with the Recipe tempo the song glides to; under an external clock the tracked tempo,
	// marked EXT (spec §13.7).
	DEF_STACK_STRING_BUF(text, 30);
	text.append("Tempo: ");
	if (playbackHandler.isExternalClockActive()) {
		playbackHandler.getTempoStringForOLED(playbackHandler.calculateBPMForDisplay(), text);
		text.append(" EXT");
	}
	else {
		playbackHandler.getTempoStringForOLED(static_cast<float>(currentRecipe.recipe.tempo) / 100.f, text);
	}
	display->popupTextTemporary(text.c_str(), PopupType::TEMPO);
	TEST_EVENT("gen_tempo_popup %s", text.c_str());
}

void PerformView::showSwing() {
	// Stock's swing popup, with the Recipe's amount and interval.
	const auto& recipe = currentRecipe.recipe;
	playbackHandler.displaySwingPopup(recipe.swing, deluge::generative::firmware::swingSyncLevel(recipe.swingInterval),
	                                  deluge::generative::kGeneratorTickMagnitude);
}

void PerformView::graphicsRoutine() {
	checkEndHold();
	// Not UI::graphicsRoutine(): it calls the root UI's, which is this one.
	const auto& output = performAdapter.output();
	int32_t beat = performAdapter.playing() ? performAdapter.clock() / kTicksPerBeat : -1;
	int32_t section = output.sectionKnown ? static_cast<int32_t>(output.section) : -1;
	bool endQueued = performAdapter.endQueued();
	if (endQueued != shownEndQueued_) {
		shownEndQueued_ = endQueued;
		redrawEndPad();
	}
	if (beat != shownBeat_ || section != shownSection_ || output.occurrence != shownOccurrence_) {
		shownBeat_ = beat;
		shownSection_ = section;
		shownOccurrence_ = output.occurrence;
		renderUIsForOled();
	}
}

void PerformView::notifyPlaybackBegun() {
	renderUIsForOled();
}

void PerformView::playbackEnded() {
	performAdapter.playbackEnded();
	renderUIsForOled();
}

bool PerformView::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                 uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) {
	if (image == nullptr) {
		return true;
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		if (whichRows & (1 << y)) {
			std::fill_n(image[y], kDisplayWidth, deluge::gui::colours::black);
			if (occupancyMask != nullptr) {
				std::fill_n(occupancyMask[y], kDisplayWidth, 0);
			}
		}
	}
	return true;
}

bool PerformView::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (image == nullptr) {
		return true;
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		if (whichRows & (1 << y)) {
			image[y][kDisplayWidth] = deluge::gui::colours::black;
			image[y][kDisplayWidth + 1] = deluge::gui::colours::black;
		}
	}
	if (whichRows & (1 << kEndPadY)) {
		// End: dim red, bright while held or queued. Its queued blink comes with the sidebar (#109).
		const RGB red = deluge::gui::colours::red;
		image[kEndPadY][kEndPadX] = endHeld_ || shownEndQueued_ ? red : deluge::gui::colours::red_dull;
	}
	return true;
}

void PerformView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	const auto* genre = currentRecipe.recipe.genre;
	canvas.drawScreenTitle(genre ? genre->oledName : deluge::l10n::get(deluge::l10n::String::STRING_FOR_PERFORM));

	char line[32];
	int32_t y = OLED_MAIN_TOPMOST_PIXEL + 14;
	const auto& output = performAdapter.output();
	if (!performAdapter.playing()) {
		snprintf(line, sizeof(line), "%s", deluge::l10n::get(deluge::l10n::String::STRING_FOR_STOPPED));
	}
	else if (output.sectionKnown) {
		int32_t beat = performAdapter.clock() / kTicksPerBeat;
		snprintf(line, sizeof(line), "%s %d  %d.%d", deluge::generative::sectionName(output.section),
		         static_cast<int>(output.occurrence + 1), static_cast<int>(beat / 4 + 1),
		         static_cast<int>(beat % 4 + 1));
	}
	else {
		snprintf(line, sizeof(line), "%s", deluge::l10n::get(deluge::l10n::String::STRING_FOR_STARTING));
	}
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
	y += kTextSpacingY + 2;

	const auto& macros = currentRecipe.recipe.macros;
	snprintf(line, sizeof(line), "E%d D%d C%d M%d", static_cast<int>(macroToPoints(macros[Macro::Energy])),
	         static_cast<int>(macroToPoints(macros[Macro::Density])),
	         static_cast<int>(macroToPoints(macros[Macro::Complexity])),
	         static_cast<int>(macroToPoints(macros[Macro::Motion])));
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
}
