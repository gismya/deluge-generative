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

// PerformView's focus and gold knobs (spec §13.5): holding and locking a part, F8's macros and Thin / Drive on the
// knobs, their rings, and letting go of held pads before an overlay.

#include "gui/views/perform_view_internal.h"

namespace {

using deluge::generative::perform::thinDriveOn;

/// Two presses on a part row this close together lock its focus (the control-layout prototype's).
constexpr uint32_t kDoubleTapSamples = kSampleRate * 35 / 100;

/// The gold knobs: the upper is encoder 1, the lower 0.
constexpr int32_t kGoldKnobs = 2;

constexpr int32_t kUpperKnob = 1;

/// Each macro's name in its popup, indexed by Macro. The MACROS page has their initials from
/// generative/perform/oled_page.h.
constexpr deluge::l10n::String kMacroNames[] = {
    deluge::l10n::String::STRING_FOR_ENERGY,
    deluge::l10n::String::STRING_FOR_DENSITY,
    deluge::l10n::String::STRING_FOR_COMPLEXITY,
    deluge::l10n::String::STRING_FOR_MOTION,
};

/// The F page of whatever the gold knobs are on: the song, or a focused part's Output.
uint8_t* knobMode() {
	ModControllable* modControllable = view.activeModControllableModelStack.modControllable;
	return modControllable ? modControllable->getModKnobMode() : nullptr;
}

} // namespace

bool PerformView::onMacroPage() const {
	// F8 holds the macros only at the Recipe level; a focused part's F8 is its own stock page (spec §13.5).
	uint8_t* mode = knobMode();
	return focusedPart() < 0 && mode != nullptr && *mode == kMacroModButton;
}

void PerformView::modButtonAction(uint8_t whichButton, bool on) {
	if (whichButton == kMacroModButton && focusedPart() < 0) {
		if (on) {
			uint8_t* mode = knobMode();
			if (mode == nullptr) {
				return;
			}
			secondMacroSet_ = (*mode == kMacroModButton) && !secondMacroSet_;
			*mode = kMacroModButton;
			view.setModLedStates();
			assertPageLeds();
			updateKnobIndicators();
		}
		return;
	}
	if (on && whichButton == 0 && knobMode() != nullptr && *knobMode() == 0) {
		return; // Perform owns the mod buttons: F1 again never turns the VU meter on (spec §13.5)
	}
	if (on && focusedPart() >= 0) {
		// A focused part's F7 pressed again switches between its stock stutter / CUSTOM 1 and Thin / Drive; any other
		// F page, or F7 pressed from another page, is stock (spec §13.5).
		const bool again = whichButton == kThinDriveModButton && knobMode() != nullptr && *knobMode() == whichButton;
		thinDrivePart_ = again && thinDrivePart_ != focusedPart() ? focusedPart() : -1;
		TEST_EVENT("gen_knobs page=%u thin_drive=%d", static_cast<unsigned>(whichButton + 1),
		           thinDrivePart_ >= 0 ? 1 : 0);
	}
	view.modButtonAction(whichButton, on);
	if (on && onThinDrivePage()) {
		// Stock's popup names stutter / CUSTOM 1, F7's own page: say what the knobs turn now.
		using deluge::generative::firmware::controlName;
		char text[32];
		snprintf(text, sizeof(text), "%s / %s", deluge::l10n::get(controlName(SoundControl::Thin)),
		         deluge::l10n::get(controlName(SoundControl::Drive)));
		display->displayPopup(text);
	}
	assertPageLeds();
	updateKnobIndicators();
}

void PerformView::songPadAction(int32_t y, bool on) {
	const int32_t part = rowFromY(y);
	if (part >= performAdapter.steering().rosterParts()) {
		return;
	}
	if (!on) {
		if (heldPart_ == part) {
			letGoOfHeldPart();
		}
		return;
	}
	// Held, the part is on the knobs, like holding a clip; a double tap locks it, or unlocks it (spec §13.5).
	const uint32_t now = AudioEngine::audioSampleTimer;
	if (lastTapPart_ == part && now - lastTapAt_ < kDoubleTapSamples) {
		lastTapPart_ = -1;
		toggleLock(part);
		return;
	}
	heldPart_ = part;
	lastTapPart_ = part;
	lastTapAt_ = now;
	lastFocusedPart_ = part;
	bindKnobs();
}

void PerformView::toggleLock(int32_t part) {
	lockedPart_ = lockedPart_ == part ? -1 : part;
	heldPart_ = -1;
	lastFocusedPart_ = part;
	bindKnobs();
}

void PerformView::bindKnobs() {
	using deluge::generative::perform::knobPage;
	using deluge::generative::perform::thinDriveAfterBind;
	const int32_t part = focusedPart();
	if (thinDriveAfterBind(part, thinDrivePart_) != thinDrivePart_) {
		// Focus moved away, or a stop dropped it: Thin / Drive ends, and a part focused again starts on its stock
		// pages (spec §13.5).
		thinDrivePart_ = -1;
		TEST_EVENT("gen_knobs thin_drive=0 refocus");
#if ENABLE_SYSEX_TEST
		// The faders are sent again after it, so a script reads them in a known order: at a stop, the 100 ms
		// refresh can send the homes before this event (drift keeps them moving up to the stop).
		shownFaders_[0] = '\0';
#endif
	}
	InstrumentClip* clip = partClip(part);
	if (clip != nullptr) {
		affectWholeKit(*clip);
		view.setActiveModControllableTimelineCounter(clip);
		// The F page is the Output's, so a sound swap's new Output comes up on its own; Thin / Drive stays on the
		// part, so it carries over (#204).
		uint8_t* mode = knobMode();
		if (mode != nullptr) {
			const uint8_t page = knobPage(part, thinDrivePart_, *mode);
			if (*mode != page) {
				*mode = page;
				view.setModLedStates();
			}
		}
	}
	else {
		view.setActiveModControllableTimelineCounter(currentSong);
	}
	assertPageLeds();
	updateKnobIndicators();
	TEST_EVENT("gen_focus part=%ld locked=%ld", static_cast<long>(clip != nullptr ? part : -1),
	           static_cast<long>(lockedPart_));
}

void PerformView::letGoForOverlay() {
	// An overlay opening over a held row lets go of the hold (a lock stays), and a held End pad never arms (spec
	// §13.5): their releases would go to the overlay.
	releaseEndPad();
	letGoOfHeldPart();
}

void PerformView::letGoOfHeldPart() {
	// Focus lasts only while the row is held (spec §13.5); a lock stays.
	if (heldPart_ >= 0) {
		heldPart_ = -1;
		bindKnobs();
	}
}

void PerformView::dropFocus() {
	heldPart_ = -1;
	lockedPart_ = -1;
	bindKnobs();
}

SoundControl PerformView::thinDriveControl(int32_t whichModEncoder) {
	return whichModEncoder == kUpperKnob ? SoundControl::Thin : SoundControl::Drive;
}

int32_t PerformView::thinDriveRing(int32_t whichModEncoder) const {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(focusedPart(), thinDriveControl(whichModEncoder), memory);
	return stack != nullptr ? liveKnobPos(*stack) - deluge::generative::perform::kKnobMin : 0;
}

int32_t PerformView::macroRing(int32_t whichModEncoder) const {
	return currentRecipe.recipe.macros[knobMacro(whichModEncoder)] / kMacroUnitsPerKnobStep;
}

bool PerformView::takeKnobRings() {
	// Under overlays too: F16 sends their gold knobs here, so they turn Thin and Drive, or the macros, as well.
	const bool thinDrive = onThinDrivePage();
	if (!thinDrive && !onMacroPage()) {
		return false;
	}
	for (int32_t knob = 0; knob < kGoldKnobs; knob++) {
		indicator_leds::setKnobIndicatorLevel(knob, thinDrive ? thinDriveRing(knob) : macroRing(knob));
	}
	return true;
}

Macro PerformView::knobMacro(int32_t whichModEncoder) const {
	if (whichModEncoder == kUpperKnob) {
		return secondMacroSet_ ? Macro::Complexity : Macro::Energy;
	}
	return secondMacroSet_ ? Macro::Motion : Macro::Density;
}

void PerformView::updateKnobIndicators() {
	// Thin and Drive's params, or the macros' centres (spec §13.5); otherwise stock's.
	if (!takeKnobRings()) {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton]);
		view.setKnobIndicatorLevels();
		return;
	}
	if (!onMacroPage()) {
		return;
	}
	// The F8 LED blinks while Complexity / Motion is on.
	if (secondMacroSet_) {
		indicator_leds::blinkLed(indicator_leds::modLed[kMacroModButton]);
	}
	else {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton], true);
		indicator_leds::setLedState(indicator_leds::modLed[kMacroModButton], true);
	}
}

bool PerformView::onThinDrivePage() const {
	const uint8_t* mode = knobMode();
	return thinDriveOn(focusedPart(), thinDrivePart_) && mode != nullptr && *mode == kThinDriveModButton;
}

void PerformView::thinDriveTurn(int32_t whichModEncoder, int32_t offset) {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(focusedPart(), thinDriveControl(whichModEncoder), memory);
	if (stack == nullptr) {
		return; // the part's preset doesn't use the control: nothing on this knob
	}
	// As a stock knob turns a param (View::modEncoderAction_existentParam), so F3 sees a player edit.
	using deluge::generative::perform::kKnobMax;
	using deluge::generative::perform::kKnobMin;
	const int32_t live = liveKnobPos(*stack);
	const int32_t newKnobPos = std::clamp<int32_t>(live + offset, kKnobMin, kKnobMax);
	if (newKnobPos == live) {
		view.displayModEncoderValuePopup(stack->paramCollection->getParamKind(), stack->paramId, live);
		return;
	}
	setLeadParam(*stack, newKnobPos, false);
}

void PerformView::setLeadParam(ModelStackWithAutoParam& stack, int32_t knobPos, bool asHome) {
	view.displayModEncoderValuePopup(stack.paramCollection->getParamKind(), stack.paramId, knobPos);
	const int32_t value = stack.paramCollection->knobPosToParamValue(knobPos, &stack);
	if (asHome) {
		soundControls.setHome(stack, value);
	}
	else {
		stack.autoParam->setCurrentValueInResponseToUserInput(value, &stack);
	}
	// MIDI-follow feedback of the live value, as stock's knob turn sends it (spec §13.7); only for the part on the gold
	// knobs, which sendFeedback() checks. Follow maps no patch cable, and stock may have deleted one set to 0.
	if (stack.paramCollection->getParamKind() != deluge::modulation::params::Kind::PATCH_CABLE) {
		view.sendMidiFollowFeedback(&stack, liveKnobPos(stack));
	}
	updateKnobIndicators();
	refreshFaders();
	refreshScreen();
}

void PerformView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	if (onThinDrivePage()) {
		thinDriveTurn(whichModEncoder, offset);
		return;
	}
	if (!onMacroPage()) {
		// A focused part's own pages, or the song FX: stock (spec §13.5).
		view.modEncoderAction(whichModEncoder, offset);
		return;
	}
	Macro macro = knobMacro(whichModEncoder);
	int32_t value = currentRecipe.recipe.macros[macro] + offset * kMacroUnitsPerKnobStep;
	performAdapter.steering().setMacro(macro, value);
	TEST_EVENT("gen_macro_knob macro=%d", static_cast<int>(macro));
	updateKnobIndicators();
	showMacroPopup(macro, currentRecipe.recipe.macros[macro]);
}

void PerformView::steerMacroFromMidi(Macro macro, int32_t units) {
	performAdapter.steering().setMacro(macro, units);
	TEST_EVENT("gen_macro_midi macro=%d units=%ld", static_cast<int>(macro),
	           static_cast<long>(currentRecipe.recipe.macros[macro]));
	updateKnobIndicators();
	if (midiEngine.midiFollowDisplayParam) {
		showMacroPopup(macro, currentRecipe.recipe.macros[macro]);
	}
}

void PerformView::showMacroPopup(Macro macro, int32_t units) {
	char popup[24];
	snprintf(popup, sizeof(popup), "%s %d", deluge::l10n::get(kMacroNames[static_cast<int32_t>(macro)]),
	         static_cast<int>(macroToPoints(units)));
	display->displayPopup(popup);
	renderUIsForOled();
}

#if ENABLE_SYSEX_TEST
namespace indicator_leds {
extern uint8_t knobIndicatorLevels[]; // what the rings show (indicator_leds.cpp)
}

void PerformView::checkKnobRings() {
	// Whether the rings show what the knobs turn, read back from the LEDs' state (overlays included): on Thin / Drive
	// "ok", or "stale" if a stock refresh put the stutter / CUSTOM 1 levels back; on the macros "macros ok" or
	// "macros stale" (stock's song-level levels).
	char text[sizeof(shownRings_)] = "";
	const bool thinDrive = onThinDrivePage();
	if (thinDrive || onMacroPage()) {
		bool ok = true;
		for (int32_t knob = 0; knob < kGoldKnobs; knob++) {
			ok = ok && indicator_leds::knobIndicatorLevels[knob] == (thinDrive ? thinDriveRing(knob) : macroRing(knob));
		}
		snprintf(text, sizeof(text), "%s%s upper=%ld lower=%ld", thinDrive ? "" : "macros ", ok ? "ok" : "stale",
		         static_cast<long>(indicator_leds::knobIndicatorLevels[kUpperKnob]),
		         static_cast<long>(indicator_leds::knobIndicatorLevels[1 - kUpperKnob]));
	}
	if (strcmp(text, shownRings_) != 0) {
		strcpy(shownRings_, text);
		if (text[0] != '\0') {
			TEST_EVENT("gen_rings %s", text);
		}
	}
}
#endif
