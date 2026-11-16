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

#include "generative/perform/keyboard_page.h"
#include "generative/perform/text.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

constexpr int32_t kTopLevel = kMacroFaderLevels - 1;
static_assert(kMacroFaderLevels == KeyboardPage::kRows);

using Look = KeyboardCell::Look;

/// The phrase-position cell (0 the top) that `stepInPhrase` falls in.
int32_t phraseCell(int32_t stepInPhrase, int32_t phraseSteps) {
	return stepInPhrase * kPhraseCells / phraseSteps;
}

/// The fader level that shows `units`, kept on the fader.
int32_t faderLevel(int32_t units) {
	return std::clamp<int32_t>(macroLevel(units), 0, kTopLevel);
}

} // namespace

KeyboardPad keyboardPad(int32_t x, int32_t row) {
	if (x >= 0 && x < kMacroFaderColumns) {
		return KeyboardPad::Fader;
	}
	if (x >= kFillFirstColumn && x < kFillFirstColumn + kFillColumns && row >= kFillFirstRow
	    && row < kFillFirstRow + kFillRows) {
		return KeyboardPad::Fill;
	}
	return KeyboardPad::None;
}

Macro faderMacro(int32_t x) {
	return static_cast<Macro>(x / kMacroFaderWidth);
}

int32_t macroFaderPad(int32_t x, int32_t level) {
	return level * kMacroFaderWidth + x % kMacroFaderWidth;
}

int32_t fillPad(int32_t x, int32_t row) {
	return (row - kFillFirstRow) * kFillColumns + (x - kFillFirstColumn);
}

KeyboardPadAt resolveKeyboardPad(int32_t x, int32_t row) {
	KeyboardPadAt at;
	at.kind = keyboardPad(x, row);
	switch (at.kind) {
	case KeyboardPad::Fader:
		at.macro = faderMacro(x);
		at.level = kTopLevel - row;
		at.pad = macroFaderPad(x, at.level);
		break;
	case KeyboardPad::Fill:
		at.pad = FillHolds::padHolder(fillPad(x, row));
		break;
	case KeyboardPad::None:
		break;
	}
	return at;
}

int32_t macroLevelUnits(int32_t level) {
	return macroPoints((level * kMacroPoints * 2 + kTopLevel) / (kTopLevel * 2));
}

int32_t macroLevel(int32_t units) {
	return (units * kTopLevel * 2 + kMacroFull) / (kMacroFull * 2);
}

void composeKeyboardPage(const KeyboardPageInput& in, KeyboardPage& out) {
	out = {};
	out.section = in.section;
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		// Each fader shows the value in effect, a held one while held, and where the Arc has the macro now. Both
		// stay on the fader's levels, so describeKeyboard() reads back from the cells what was put there.
		const bool held = in.holds != nullptr && in.holds->held(macro);
		const int32_t value = faderLevel(held ? in.holds->units(macro) : in.centres[macro]);
		const int32_t arc = in.piece != nullptr ? faderLevel(in.piece->effectiveMacro(macro)) : -1;
		for (int32_t level = 0; level < kMacroFaderLevels; level++) {
			const KeyboardCell cell = {
			    .look = level == arc ? Look::FaderArc : Look::Fader,
			    .lit = level <= value,
			    .held = held,
			    .macro = macro,
			};
			std::fill_n(&out.cell[kTopLevel - level][m * kMacroFaderWidth], kMacroFaderWidth, cell);
		}
	}
	if (in.sounding && in.phraseSteps > 0) {
		const int32_t now = phraseCell(std::max<int32_t>(in.clock, 0) / kTicksPerStep % in.phraseSteps, in.phraseSteps);
		for (int32_t c = 0; c < kPhraseCells; c++) {
			out.cell[c][kPhraseColumn] = {.look = c == now ? Look::PhraseNow : Look::Phrase, .lit = c < now};
		}
	}
	for (int32_t row = kFillFirstRow; row < kFillFirstRow + kFillRows; row++) {
		for (int32_t x = kFillFirstColumn; x < kFillFirstColumn + kFillColumns; x++) {
			out.cell[row][x] = {.look = Look::Fill, .lit = in.fill};
		}
	}
}

void describeKeyboard(const KeyboardPage& page, char (&out)[kKeyboardTextChars + 1]) {
	Text text(out);
	for (int32_t m = 0; m < kNumMacros; m++) {
		// The harness has no l10n: the macros' initials, as the MACROS page shows them.
		constexpr const char* kInitials[] = {"E=", "D=", "C=", "M="}; // Macro order
		static_assert(sizeof(kInitials) / sizeof(kInitials[0]) == kNumMacros);
		const int32_t x = m * kMacroFaderWidth;
		int32_t value = -1;
		int32_t arc = -1;
		for (int32_t level = 0; level < kMacroFaderLevels; level++) {
			const KeyboardCell& cell = page.cell[kTopLevel - level][x];
			value = cell.lit ? level : value;
			arc = cell.look == Look::FaderArc ? level : arc;
		}
		text.add(kInitials[m]).add(value + 1).add(page.cell[0][x].held ? "h/" : "/");
		if (arc >= 0) {
			text.add(arc + 1).add(" ");
		}
		else {
			text.add("- ");
		}
	}
	text.add("phrase=");
	int32_t now = -1;
	for (int32_t c = 0; c < kPhraseCells; c++) {
		now = page.cell[c][kPhraseColumn].look == Look::PhraseNow ? c : now;
	}
	if (now >= 0) {
		text.add(now + 1);
	}
	else {
		text.add("-");
	}
	text.add(" fill=").add(page.cell[kFillFirstRow][kFillFirstColumn].lit ? "1" : "0");
}

void MacroFaderHolds::press(Macro macro, int32_t pad, int32_t units, uint32_t now) {
	const int32_t i = index(macro);
	if (pads_[i] == 0) {
		pressedAt_[i] = now;
	}
	pads_[i] |= static_cast<uint16_t>(1U << pad);
	units_[i] = static_cast<int16_t>(units);
}

MacroFaderHolds::Release MacroFaderHolds::release(Macro macro, int32_t pad, uint32_t now, uint32_t tapTime) {
	const int32_t i = index(macro);
	if (!held(macro, pad)) {
		return Release::None;
	}
	pads_[i] &= static_cast<uint16_t>(~(1U << pad));
	if (pads_[i] != 0) {
		return Release::None;
	}
	return now - pressedAt_[i] < tapTime ? Release::Keep : Release::SpringBack;
}

int32_t MacroSteering::resend(Macro macro) const {
	const int32_t held = held_[static_cast<int32_t>(macro)];
	return held != kNotHeld && differs(macro, held) ? held : kNotHeld;
}

bool FillHolds::set(int32_t holder, bool down) {
	const bool was = held();
	const uint16_t bit = static_cast<uint16_t>(1U << holder);
	holders_ = down ? holders_ | bit : holders_ & static_cast<uint16_t>(~bit);
	return held() != was;
}

} // namespace deluge::generative::perform
