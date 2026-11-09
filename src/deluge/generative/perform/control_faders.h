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

// The CLIP page's sound-control faders (spec §13.5): rows 3–8 hold Brightness, Thin, Decay, Space, Drive and Wobble
// across the 16 main pads, over the knob range of the param each fader sets (its control's lead param, spec §11.6).
// Tapping a pad sets home; the pads up to home are lit and the live value's pad is white, on home's pad too.

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative::perform {

constexpr int32_t kFaderPads = 16;
/// The first fader's row, counted from the top from 0 (spec row 3).
constexpr int32_t kFirstFaderRow = 2;

/// The control on a CLIP page row (counted from the top from 0), for rows kFirstFaderRow onwards.
SoundControl faderControl(int32_t row);

/// The pad (0–15) that shows a knob position (-64..64, as stock's gold knobs read a param), to the nearest pad.
int32_t faderPad(int32_t knobPos);
/// The knob position a tap on `pad` sets: pad 0 is -64, the bottom, and pad 15 is 64, the top.
int32_t faderKnobPos(int32_t pad);

enum class FaderLook : uint8_t {
	Off,  ///< above home
	Home, ///< home's pad or below it
	Live, ///< the live value's pad, wherever it is: white
};
/// How `pad` looks with home on `homePad` and the live value on `livePad`.
FaderLook faderLook(int32_t pad, int32_t homePad, int32_t livePad);

} // namespace deluge::generative::perform
