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

#include "generative/perform/control_faders.h"

namespace deluge::generative::perform {

namespace {

/// From spec row 3 down.
constexpr SoundControl kFaderControls[] = {SoundControl::Brightness, SoundControl::Thin,  SoundControl::Decay,
                                           SoundControl::Space,      SoundControl::Drive, SoundControl::Wobble};
static_assert(sizeof(kFaderControls) / sizeof(kFaderControls[0]) == kNumSoundControls);

constexpr int32_t kKnobSpan = 128; ///< -64..64
constexpr int32_t kLastPad = kFaderPads - 1;

} // namespace

SoundControl faderControl(int32_t row) {
	return kFaderControls[row - kFirstFaderRow];
}

int32_t faderPad(int32_t knobPos) {
	return ((knobPos + kKnobSpan / 2) * kLastPad * 2 + kKnobSpan) / (kKnobSpan * 2);
}

int32_t faderKnobPos(int32_t pad) {
	return (pad * kKnobSpan * 2 + kLastPad) / (kLastPad * 2) - kKnobSpan / 2;
}

FaderLook faderLook(int32_t pad, int32_t homePad, int32_t livePad) {
	if (pad == livePad && livePad != homePad) {
		return FaderLook::Live;
	}
	return pad <= homePad ? FaderLook::Home : FaderLook::Off;
}

} // namespace deluge::generative::perform
