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

// The KEYBOARD page (spec §13.5), Perform's performance pads: the four macros as faders on columns 1–8, two columns
// each, the phrase position on column 14's top four rows, and FILL on columns 14–16's bottom four. A fader tap sets
// the macro's centre; a hold plays a temporary value that springs back on release, as stock's PerformanceView pads do.
// The page composes as portable cells: the Perform view turns them into colours, and the test port's gen_keyboard
// text is made from the same cells.

#pragma once

#include "generative/core/generator.h"
#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::perform {

/// Columns 1–8: Energy, Density, Complexity and Motion, two columns each, as Macro's order.
constexpr int32_t kMacroFaderWidth = 2;
constexpr int32_t kMacroFaderColumns = kNumMacros * kMacroFaderWidth;
/// A fader's levels, one per row: 0 the bottom row, the macro's 0, to 7 the top, its 50.
constexpr int32_t kMacroFaderLevels = 8;
/// Column 14 (counted from 0), rows 1–4 from the top: the phrase in quarters.
constexpr int32_t kPhraseColumn = 13;
constexpr int32_t kPhraseCells = 4;
/// Columns 14–16, rows 5–8 from the top (counted from 0): FILL.
constexpr int32_t kFillFirstColumn = 13;
constexpr int32_t kFillColumns = 3;
constexpr int32_t kFillFirstRow = 4;
constexpr int32_t kFillRows = 4;

enum class KeyboardPad : uint8_t { None, Fader, Fill };
/// What a main pad does on the KEYBOARD page, by column and row counted from the top from 0.
KeyboardPad keyboardPad(int32_t x, int32_t row);
/// The macro whose fader holds column `x` (0 to kMacroFaderColumns - 1).
Macro faderMacro(int32_t x);
/// The pad, 0 to kMacroFaderWidth * kMacroFaderLevels - 1, at column `x` and `level` of its fader.
int32_t macroFaderPad(int32_t x, int32_t level);
/// FILL pad (`x`, `row`)'s index, 0 to kFillColumns * kFillRows - 1.
int32_t fillPad(int32_t x, int32_t row);

/// What a main pad on the KEYBOARD page is, worked out once for its press, its release and whether it is held.
struct KeyboardPadAt {
	KeyboardPad kind = KeyboardPad::None;
	Macro macro = Macro::Energy; ///< Fader: whose fader
	int32_t level = 0;           ///< Fader: the row's level, 0 the bottom
	int32_t pad = 0;             ///< Fader: its pad on the fader (macroFaderPad). Fill: its FillHolds holder
};
/// Main pad (`x`, `row`), its row counted from the top from 0.
KeyboardPadAt resolveKeyboardPad(int32_t x, int32_t row);

/// The macro units a fader level sets: its share of 0–50 in whole points, to the nearest.
int32_t macroLevelUnits(int32_t level);
/// The level that shows `units`, to the nearest.
int32_t macroLevel(int32_t units);

/// A fader press let go sooner than this, in milliseconds, is a tap; held this long or longer, it is a hold. The user's
/// choice after trying stock's Hold Press Time (100 ms by default) on the Deluge and finding it too short. Fixed, so
/// stock's own pads keep their setting.
constexpr uint32_t kMacroTapMs = 300;

/// The macro faders' presses: a tap (let go within kMacroTapMs) sets a new centre, a hold plays its value only
/// while held (spec §13.5). The driver steers the core with the held value as a pad goes down, so a press answers at
/// once, and on the last pad's release keeps it as the centre or steers back to the centre.
class MacroFaderHolds {
public:
	/// A pad of `macro`'s fader (`pad`, 0 to kMacroFaderWidth * kMacroFaderLevels - 1) went down at `now`: the macro
	/// plays `units` while it is held. A second pad on the same fader moves the held value; the hold dates from the
	/// first.
	void press(Macro macro, int32_t pad, int32_t units, uint32_t now);

	enum class Release : uint8_t {
		None,       ///< not the fader's last pad held, or a pad this never saw go down
		Keep,       ///< a tap: the held value is the new centre
		SpringBack, ///< a hold: back to the centre
	};
	/// `pad` of `macro`'s fader went up at `now`. `tapTime` is kMacroTapMs in `now`'s units.
	Release release(Macro macro, int32_t pad, uint32_t now, uint32_t tapTime);

	/// Whether any pad of `macro`'s fader is held, and the value it plays.
	[[nodiscard]] bool held(Macro macro) const { return pads_[index(macro)] != 0; }
	[[nodiscard]] int32_t units(Macro macro) const { return units_[index(macro)]; }
	/// Whether `pad` of `macro`'s fader is held.
	[[nodiscard]] bool held(Macro macro, int32_t pad) const { return (pads_[index(macro)] >> pad & 1) != 0; }

private:
	static constexpr int32_t index(Macro macro) { return static_cast<int32_t>(macro); }
	uint16_t pads_[kNumMacros] = {}; ///< per fader, a bit per pad held
	int16_t units_[kNumMacros] = {};
	uint32_t pressedAt_[kNumMacros] = {};
	static_assert(kMacroFaderWidth * kMacroFaderLevels <= 16, "a fader's pads fit pads_");
};

/// What the driver has steered the core's macros with, so a KEYBOARD fader's value reaches the core across a piece or
/// instance start (spec §13.5). A start begins from the Recipe's centres and drops the steering queue; a transition's
/// instance carries the last one's macros, and the queue goes on into it.
class MacroSteering {
public:
	static constexpr int32_t kNotHeld = -1;

	/// A piece starts from `macros`, the steering queue empty.
	void startFrom(const Macros& macros) { sent_ = macros; }
	/// The driver pushed a SetMacro (a knob turn, a fader, anything): the core plays it once it lands.
	void sent(Macro macro, int32_t units) { sent_.set(macro, units); }
	/// Whether the core needs `units` pushed to end up with it: what it was last sent differs.
	[[nodiscard]] bool differs(Macro macro, int32_t units) const { return sent_[macro] != units; }

	/// A fader held at `units`, or let go.
	void hold(Macro macro, int32_t units) { held_[static_cast<int32_t>(macro)] = static_cast<int16_t>(units); }
	void letGo(Macro macro) { held_[static_cast<int32_t>(macro)] = kNotHeld; }
	/// Whether a new centre (a gold-knob turn) steers the core now: not while a fader holds the macro, whose held
	/// value plays until it is let go and springs back to the centre then (spec §13.5).
	[[nodiscard]] bool centreSteers(Macro macro) const { return held_[static_cast<int32_t>(macro)] == kNotHeld; }
	/// After an instance starts: the held value to send `macro` again, as FILL is, or kNotHeld when it isn't held or
	/// the core has it already.
	[[nodiscard]] int32_t resend(Macro macro) const;

private:
	Macros sent_;
	int16_t held_[kNumMacros] = {kNotHeld, kNotHeld, kNotHeld, kNotHeld};
};

/// What holds FILL: the SYNC-SCALING button and the KEYBOARD page's FILL pads (spec §13.5). FILL is on while any of
/// them is held.
class FillHolds {
public:
	static constexpr int32_t kButton = 0;
	/// FILL pad `pad`'s holder.
	static constexpr int32_t padHolder(int32_t pad) { return 1 + pad; }

	/// `holder` went down or up. @return Whether FILL turned on or off with it: the first holder down or the last up.
	bool set(int32_t holder, bool down);
	[[nodiscard]] bool held() const { return holders_ != 0; }
	[[nodiscard]] bool held(int32_t holder) const { return (holders_ >> holder & 1) != 0; }

private:
	uint16_t holders_ = 0;
	static_assert(1 + kFillColumns * kFillRows <= 16, "every holder fits holders_");
};

/// What the KEYBOARD page reads, from task context between ticks (spec §2.8).
struct KeyboardPageInput {
	/// The Recipe's macro centres.
	Macros centres;
	/// The faders held: a held fader shows its held value, brighter.
	const MacroFaderHolds* holds = nullptr;
	/// The piece playing, or nullptr while stopped: each fader shows where the Arc has its macro now.
	const Generator* piece = nullptr;
	/// A Section has been heard, and `section` is it: the phrase column shows where its phrase is.
	bool sounding = false;
	SectionKind section = SectionKind::Intro;
	/// Generator ticks since step 0's grid line, as heard.
	int32_t clock = 0;
	int32_t phraseSteps = 8 * kStepsPerBar;
	/// FILL is on: a FILL holder is down (FillHolds), or MIDI holds the song's fill mode.
	bool fill = false;
};

struct KeyboardCell {
	enum class Look : uint8_t {
		Off,
		Fader,     ///< a fader pad of `macro`: `lit` on the value's level and below it
		FaderArc,  ///< where the Arc has `macro` now: white, on the value's level too. `lit` as Fader
		Phrase,    ///< a phrase quarter: `lit` once it has passed
		PhraseNow, ///< the phrase quarter playing
		Fill,      ///< `lit` while FILL is on
	};
	Look look = Look::Off;
	bool lit = false;
	/// Fader cells: the fader is held, and shows its held value.
	bool held = false;
	Macro macro = Macro::Energy;
	bool operator==(const KeyboardCell&) const = default;
};

struct KeyboardPage {
	static constexpr int32_t kRows = 8;
	static constexpr int32_t kColumns = 16;
	/// The Section playing, whose colour the phrase column has.
	SectionKind section = SectionKind::Intro;
	/// Rows from the top, 0 = row 1.
	KeyboardCell cell[kRows][kColumns];
};

void composeKeyboardPage(const KeyboardPageInput& in, KeyboardPage& out);

/// The longest gen_keyboard text is 45 characters.
constexpr int32_t kKeyboardTextChars = 63;
/// The test port's gen_keyboard text: each fader's value level and the Arc's (1–8 from the bottom; h while held, -
/// when stopped), the phrase cell (1–4 from the top, - when stopped) and FILL.
/// "E=5h/3 D=4/4 C=4/4 M=4/5 phrase=2 fill=0".
void describeKeyboard(const KeyboardPage& page, char (&out)[kKeyboardTextChars + 1]);

} // namespace deluge::generative::perform
