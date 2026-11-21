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
	if (x >= kLaneMuteColumn && x < kLaneMuteColumn + kLaneMuteColumns && row >= 0 && row < KeyboardPage::kRows
	    && (x - kLaneMuteColumn) * KeyboardPage::kRows + row < kNumDrumLanes) {
		return KeyboardPad::LaneMute;
	}
	if (x >= kPunchFirstColumn && x < kPunchFirstColumn + kPunchColumns && row >= 0
	    && row * kPunchColumns + (x - kPunchFirstColumn) < kNumPunchPads) {
		return KeyboardPad::Punch;
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
	case KeyboardPad::LaneMute:
		at.pad = (x - kLaneMuteColumn) * KeyboardPage::kRows + row;
		break;
	case KeyboardPad::Punch:
		at.punch = kPunchPads[row * kPunchColumns + (x - kPunchFirstColumn)];
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
	out.focused = static_cast<int8_t>(in.focused);
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		// Each fader shows the value in effect, a held one while held, and where the Arc has the macro now. Both
		// stay on the fader's levels, so describeKeyboard() reads back from the cells what was put there.
		// With a part focused, the part's biased value and the centre it is biased from (spec §13.5).
		// A hold shows only on the level it steers: a part's on that part, the Recipe level's with nothing focused.
		const bool held = in.holds != nullptr && in.holds->held(macro) && in.holds->part(macro) == in.focused;
		const bool part = in.focused >= 0;
		const int32_t shown = part ? partBiased(in.centres[macro], in.bias.of(in.focused, macro)) : in.centres[macro];
		const int32_t value = faderLevel(held ? in.holds->units(macro) : shown);
		const int32_t arc = part                  ? faderLevel(in.centres[macro])
		                    : in.piece != nullptr ? faderLevel(in.piece->effectiveMacro(macro))
		                                          : -1;
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
	// A pad per lane the Drums part has; the rest of the two columns stay dark.
	for (int32_t l = 0; l < std::min(in.drumLaneCount, kNumDrumLanes); l++) {
		const int32_t key = PunchHolds::punchKey(PunchIn::LaneMute, in.drumLanes[l]);
		out.cell[l % KeyboardPage::kRows][kLaneMuteColumn + l / KeyboardPage::kRows] = {
		    .look = Look::LaneMute,
		    .lit = in.punches != nullptr && in.punches->on(key),
		    .held = in.punches != nullptr && in.punches->held(key),
		};
	}
	for (int32_t i = 0; i < kNumPunchPads; i++) {
		const int32_t key = PunchHolds::punchKey(kPunchPads[i], Lane::None);
		out.cell[i / kPunchColumns][kPunchFirstColumn + i % kPunchColumns] = {
		    .look = Look::Punch,
		    .lit = in.punches != nullptr && in.punches->on(key),
		    .held = in.punches != nullptr && in.punches->held(key),
		    .punch = kPunchPads[i],
		};
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
	if (page.focused >= 0) {
		text.add(" part=").add(static_cast<int32_t>(page.focused));
	}
}

void describePunches(const KeyboardPage& page, char (&out)[kPunchTextChars + 1]) {
	Text text(out);
	text.add("lanes=");
	for (int32_t l = 0; l < kNumDrumLanes; l++) {
		const KeyboardCell& cell = page.cell[l % KeyboardPage::kRows][kLaneMuteColumn + l / KeyboardPage::kRows];
		if (cell.look == Look::LaneMute) {
			text.add(cell.held ? "M" : cell.lit ? "m" : ".");
		}
	}
	text.add(" punch=");
	for (int32_t i = 0; i < kNumPunchPads; i++) {
		constexpr const char* kOn[] = {"H", "S", "F", "D", "P"}; // kPunchPads' order
		constexpr const char* kHeld[] = {"h", "s", "f", "d", "p"};
		static_assert(sizeof(kOn) / sizeof(kOn[0]) == kNumPunchPads && sizeof(kHeld) == sizeof(kOn));
		const KeyboardCell& cell = page.cell[i / kPunchColumns][kPunchFirstColumn + i % kPunchColumns];
		text.add(cell.held ? kHeld[i] : cell.lit ? kOn[i] : ".");
	}
}

bool PunchHolds::press(int32_t key, uint32_t now) {
	const uint32_t bit = 1U << key;
	if ((held_ & bit) != 0) {
		return false;
	}
	held_ |= bit;
	pressedAt_[key] = now;
	wasOn_ = (on_ & bit) != 0 ? wasOn_ | bit : wasOn_ & ~bit;
	if ((on_ & bit) != 0) {
		return false;
	}
	on_ |= bit;
	return true;
}

bool PunchHolds::release(int32_t key, uint32_t now, uint32_t tapTime) {
	const uint32_t bit = 1U << key;
	if ((held_ & bit) == 0) {
		return false;
	}
	held_ &= ~bit;
	// A tap on one that was off latches it; any other release lets it go.
	if ((wasOn_ & bit) == 0 && now - pressedAt_[key] < tapTime) {
		return false;
	}
	on_ &= ~bit;
	return true;
}

void PunchHolds::refused(int32_t key, bool on) {
	const uint32_t bit = 1U << key;
	held_ &= ~bit;
	on_ = on ? on_ | bit : on_ & ~bit;
}

void MacroFaderHolds::press(Macro macro, int32_t pad, int32_t units, uint32_t now, int32_t part) {
	const int32_t i = index(macro);
	if (pads_[i] == 0) {
		pressedAt_[i] = now;
		part_[i] = static_cast<int8_t>(part);
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
