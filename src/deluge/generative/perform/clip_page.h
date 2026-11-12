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

// The CLIP page (spec §6.6, §13.5) as portable cells: row 1 shows the last part focused's takes 1–16, the one playing
// bright, earlier ones dim, the next new take grey and a queued one blinking; row 2 has FREEZE, NEW SOUND and SOUND
// EDITOR; rows 3–8 are its six sound controls as faders, over the knob range of the param each sets (its control's
// lead param, spec §11.6), the pads up to home lit and the live value's pad white. The Perform view turns the cells
// into colours and applies the blink phase; the test port's gen_clip and gen_faders texts are made from the same
// cells. Also the Y turn through the takes, the PART page's take label and the seed code Y push shows (spec §6.5).

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative::perform {

/// The CLIP page's rows above the faders, counted from the top from 0 (spec §13.5): row 1 the takes, row 2 FREEZE,
/// NEW SOUND and SOUND EDITOR from the left.
constexpr int32_t kTakesRow = 0;
constexpr int32_t kActionsRow = 1;
constexpr int32_t kFreezePad = 0;
constexpr int32_t kNewSoundPad = 1;
constexpr int32_t kSoundEditorPad = 2;

constexpr int32_t kFaderPads = 16;
/// The first fader's row, counted from the top from 0 (spec row 3).
constexpr int32_t kFirstFaderRow = 2;

/// The control on a CLIP page row (counted from the top from 0), for rows kFirstFaderRow onwards: Brightness, Thin,
/// Decay, Space, Drive, Wobble.
SoundControl faderControl(int32_t row);

/// A gold knob's positions, as stock's knobs read a param (`paramValueToKnobPos`).
constexpr int32_t kKnobMin = -64;
constexpr int32_t kKnobMax = 64;
constexpr int32_t kKnobSpan = kKnobMax - kKnobMin;

/// The knob position a tap on fader pad `pad` sets: pad 0 is -64, the bottom, and pad 15 is 64, the top.
int32_t faderKnobPos(int32_t pad);

/// A part's takes as the CLIP page reads them.
struct TakeRow {
	int32_t playing = 0;  ///< The take playing, 0–15.
	int32_t waiting = -1; ///< The take queued for the next phrase line, or -1.
	int32_t newest = 0;   ///< The highest take the part has played in this Recipe.
};

/// A control's home and live value on a part, as knob positions (kKnobMin..kKnobMax), for the faders and the PART
/// page. The live value is the param now: home plus every layer (spec §13.5).
struct ControlLevel {
	bool has = false; ///< The part's preset uses the control.
	int8_t home = 0;
	int8_t live = 0;
	bool operator==(const ControlLevel&) const = default;
};

/// A part's new sound on its way (spec §10.7).
enum class SoundSwap : uint8_t {
	None,
	Loading, ///< its preload runs
	Waiting, ///< loaded, waiting for its phrase line
};

/// What the CLIP page reads, from task context between ticks (spec §2.8). The firmware's params and sound swaps come
/// as plain values.
struct ClipPageInput {
	/// The last part held or locked, or -1 before any. The page is dark unless it is in the roster.
	int32_t part = -1;
	/// Parts in the roster.
	int32_t parts = 0;
	TakeRow takes;
	bool frozen = false;
	SoundSwap sound = SoundSwap::None;
	/// The part's six controls as last read, indexed by SoundControl.
	ControlLevel levels[kNumSoundControls];
};

struct ClipCell {
	enum class Look : uint8_t {
		Off,
		TakePlaying,  ///< bright
		TakeEarlier,  ///< a take made already: dim
		TakeNew,      ///< the next new take: grey
		TakeQueued,   ///< waiting for its line: blinking
		Freeze,       ///< `lit` while the part is frozen
		NewSound,     ///< NEW SOUND at rest
		SoundLoading, ///< NEW SOUND while the sound preloads: blinking
		SoundWaiting, ///< NEW SOUND while the sound waits for its line: blinking
		SoundEditor,
		Fader,     ///< `lit` on home's pad and below it
		FaderLive, ///< the live value's pad, wherever it is: white. `lit` as Fader
	};
	Look look = Look::Off;
	bool lit = false;
	bool operator==(const ClipCell&) const = default;
};

struct ClipPage {
	static constexpr int32_t kRows = 8;
	static constexpr int32_t kColumns = 16;
	/// ClipPageInput::part, for the test port's texts.
	int8_t part = -1;
	/// The part is in the roster: the page shows it. Otherwise every cell is off.
	bool shown = false;
	/// Rows from the top, 0 = row 1.
	ClipCell cell[kRows][kColumns];
};

void composeClipPage(const ClipPageInput& in, ClipPage& out);

/// The longest gen_clip and gen_faders text.
constexpr int32_t kClipTextChars = 63;
/// The test port's gen_clip text, rows 1 and 2: each take pad (P playing, e earlier, n the next new take, q queued,
/// . off), the freeze, and the part's sound (p preloading, w waiting for its line).
/// "part=1 takes=Peqn............ freeze=0 sound=-".
void describeClip(const ClipPage& page, char (&out)[kClipTextChars + 1]);
/// The test port's gen_faders text: each fader's home and live pads (1–16), from row 3 down. "12=" when the live
/// value is on home's pad, "12/11" when it isn't, "-" where the part has no such control; "at_home" after the part
/// when every fader's live value is on its home's pad. "part=1 at_home 9= 9= 9= 9= 9= -".
void describeFaders(const ClipPage& page, char (&out)[kClipTextChars + 1]);

/// Whether tapping take pad `take` queues it: any lit pad, up to the next new take.
bool takeTappable(const TakeRow& row, int32_t take);

/// The take a Y turn queues: one back (`direction` < 0) or forward from the waiting take, or else the one playing.
/// Forward past the newest is the next new take. -1 before the first take or past the last.
int32_t steppedTake(const TakeRow& row, int32_t direction);

/// The PART page's take label's longest: "T16/16*".
constexpr int32_t kTakeLabelChars = 7;
/// The PART page's take (spec §13.5): "T2/3", the take playing of the takes made (a waiting one counted), with "*"
/// when the part is frozen.
void takeLabel(const TakeRow& row, bool frozen, char (&out)[kTakeLabelChars + 1]);

/// A seed code's length: 7 base-32 characters cover 32 bits (spec §6.5).
constexpr int32_t kSeedCodeChars = 7;
/// Writes `seed` as its seed code, most significant character first, in Crockford's base 32 (0–9 and A–Z without I,
/// L, O and U), which QwertyUI accepts and the OLED can't confuse.
void seedCode(uint32_t seed, char (&out)[kSeedCodeChars + 1]);

} // namespace deluge::generative::perform
