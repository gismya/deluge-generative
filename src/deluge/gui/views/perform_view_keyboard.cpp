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

// PerformView's KEYBOARD page (spec §13.5): the macro faders, FILL, the key pads.

#include "gui/views/perform_view_internal.h"

using deluge::generative::Macro;
using deluge::generative::SectionKind;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using namespace perform_view_shared;

namespace {

/// Each macro's colour on the KEYBOARD page's faders (the control-layout prototype's), indexed by Macro: Energy,
/// Density, Complexity, Motion.
constexpr RGB kMacroColours[] = {RGB(255, 70, 30), RGB(255, 200, 0), RGB(0, 180, 255), RGB(180, 90, 255)};

/// The KEYBOARD page's FILL pads while FILL is up (the control-layout prototype's); white while it is held.
constexpr RGB kFillColour = RGB::monochrome(70);

/// A KEYBOARD fader press let go sooner than this is a tap, a new centre; held longer, it springs back (spec §13.5).
constexpr uint32_t kMacroTapSamples = kSampleRate * deluge::generative::perform::kMacroTapMs / 1000;

/// What the Recipe keeps for a fader let go: the macro's centre, or `part`'s bias on it (test events).
[[maybe_unused]] int32_t keptByFader(Macro macro, int32_t part) {
	return part >= 0 ? currentRecipe.recipe.bias.of(part, macro) : currentRecipe.recipe.macros[macro];
}

} // namespace

void PerformView::keyboardPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::generative::perform;
	const KeyboardPadAt at = resolveKeyboardPad(x, rowFromY(y));
	switch (at.kind) {
	case KeyboardPad::Fader: {
		if (!on) {
			releaseMacroPad(at.macro, at.pad);
			break;
		}
		// With a part focused the row sets its Part bias, so the part plays at the row (spec §3.3, §13.5).
		const int32_t units = macroLevelUnits(at.level);
		macroHolds_.press(at.macro, at.pad, units, AudioEngine::audioSampleTimer, focusedPart());
		const int32_t part = macroHolds_.part(at.macro);
		performAdapter.steering().holdFader(part, at.macro, units);
		TEST_EVENT("gen_macro_pad macro=%d hold units=%ld part=%ld", static_cast<int>(at.macro),
		           static_cast<long>(units), static_cast<long>(part));
		if (part >= 0) {
			const int32_t bias = performAdapter.steering().heldBias(at.macro);
			showBiasPopup(at.macro, deluge::generative::partBiased(currentRecipe.recipe.macros[at.macro], bias), bias);
		}
		else {
			showMacroPopup(at.macro, units);
		}
		break;
	}
	case KeyboardPad::Fill:
		holdFill(at.pad, on);
		break;
	case KeyboardPad::None:
		break;
	}
	refreshKeyboardPage();
	refreshScreen();
}

void PerformView::releaseMacroPad(Macro macro, int32_t pad) {
	using deluge::generative::perform::MacroFaderHolds;
	const int32_t units = macroHolds_.units(macro);
	const int32_t part = macroHolds_.part(macro); // the part the press steered, whatever is focused now
	// A tap keeps the value as the new centre, or bias; a hold springs back to it (spec §13.5).
	switch (macroHolds_.release(macro, pad, AudioEngine::audioSampleTimer, kMacroTapSamples)) {
	case MacroFaderHolds::Release::Keep:
		performAdapter.steering().keepFader(part, macro, units);
		TEST_EVENT("gen_macro_pad macro=%d keep units=%ld part=%ld", static_cast<int>(macro),
		           static_cast<long>(keptByFader(macro, part)), static_cast<long>(part));
		break;
	case MacroFaderHolds::Release::SpringBack:
		performAdapter.steering().springFaderBack(part, macro);
		TEST_EVENT("gen_macro_pad macro=%d spring units=%ld part=%ld", static_cast<int>(macro),
		           static_cast<long>(keptByFader(macro, part)), static_cast<long>(part));
		break;
	case MacroFaderHolds::Release::None:
		return;
	}
	updateKnobIndicators(); // the rings show the centre
}

bool PerformView::keyboardPadHeld(int32_t x, int32_t y) const {
	using namespace deluge::generative::perform;
	const KeyboardPadAt at = resolveKeyboardPad(x, rowFromY(y));
	switch (at.kind) {
	case KeyboardPad::Fader:
		return macroHolds_.held(at.macro, at.pad);
	case KeyboardPad::Fill:
		return fillHolds_.held(at.pad);
	case KeyboardPad::None:
		break;
	}
	return false;
}

void PerformView::releaseLostHolds() {
	using deluge::generative::perform::FillHolds;
	if (fillHolds_.held(FillHolds::kButton) && !Buttons::isButtonPressed(deluge::hid::button::SYNC_SCALING)) {
		holdFill(FillHolds::kButton, false);
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			if (keyboardPadHeld(x, y) && !matrixDriver.isPadPressed(x, y)) {
				keyboardPadAction(x, y, false);
			}
		}
	}
}

namespace {
/// A KEYBOARD page cell's colour, the phrase column in `section`'s.
RGB keyboardColour(const deluge::generative::perform::KeyboardCell& cell, SectionKind section) {
	using Look = deluge::generative::perform::KeyboardCell::Look;
	switch (cell.look) {
	case Look::FaderArc:
		return RGB(255, 255, 255);
	case Look::Fader: {
		const RGB colour = kMacroColours[static_cast<int32_t>(cell.macro)];
		return !cell.lit ? colour.dim(4) : cell.held ? colour : colour.dim(1); // brighter while held
	}
	case Look::PhraseNow:
		return sectionColour(section, true);
	case Look::Phrase:
		return sectionColour(section, true).dim(cell.lit ? 1 : 4);
	case Look::Fill:
		return cell.lit ? RGB(255, 255, 255) : kFillColour;
	case Look::Off:
		break;
	}
	return deluge::gui::colours::black;
}
} // namespace

void PerformView::refreshKeyboardPage() {
	using namespace deluge::generative::perform;
	static_assert(KeyboardPage::kRows == kDisplayHeight && KeyboardPage::kColumns == kDisplayWidth);
	if (page_ != Page::Keyboard) {
		return;
	}
	const auto& output = performAdapter.output();
	KeyboardPageInput in;
	in.centres = currentRecipe.recipe.macros;
	in.holds = &macroHolds_;
	in.focused = focusedPart();
	in.bias = currentRecipe.recipe.bias;
	in.piece = performAdapter.playingGenerator();
	in.sounding = in.piece != nullptr && output.sectionKnown;
	in.section = output.section;
	in.clock = performAdapter.clock();
	in.phraseSteps = performAdapter.steering().phraseSteps();
	in.fill = currentSong->isFillModeActive(); // MIDI FILL too
	KeyboardPage composed;
	composeKeyboardPage(in, composed);
#if ENABLE_SYSEX_TEST
	{
		char text[kKeyboardTextChars + 1];
		static_assert(sizeof(text) == sizeof(shownKeyboard_));
		describeKeyboard(composed, text);
		if (strcmp(text, shownKeyboard_) != 0) {
			strcpy(shownKeyboard_, text);
			TEST_EVENT("gen_keyboard %s", text);
		}
	}
#endif
	RGB pads[kDisplayHeight][kDisplayWidth];
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			pads[y][x] = keyboardColour(composed.cell[rowFromY(y)][x], composed.section);
		}
	}
	showMainPads(pads);
}
