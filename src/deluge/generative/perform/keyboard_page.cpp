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

namespace deluge::generative::perform {

namespace {

constexpr int32_t kTopLevel = kMacroFaderLevels - 1;

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

MacroPadLook macroPadLook(int32_t level, int32_t valueLevel, int32_t arcLevel) {
	if (level == arcLevel) {
		return MacroPadLook::Arc;
	}
	return level <= valueLevel ? MacroPadLook::Lit : MacroPadLook::Off;
}

int32_t phraseCell(int32_t stepInPhrase, int32_t phraseSteps) {
	return stepInPhrase * kPhraseCells / phraseSteps;
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
