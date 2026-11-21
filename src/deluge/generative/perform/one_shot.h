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

// Whether a lane sample plays out or stops at its note-off (spec §10.4): the firmware sets each lane sample's repeat
// mode from this, and the harness's rough render follows the same rule, so the two can't drift apart.

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

/// Samples shorter than this play once (ONCE), as the sample browser sets them; longer ones load in CUT mode, so their
/// note-off stops them.
constexpr uint32_t kOneShotMaxLengthMs = 2000;

/// A sample's length in milliseconds, rounded as `Sample::getLengthInMSec()` rounds it.
constexpr uint32_t sampleLengthMs(uint64_t frames, uint32_t rate) {
	return frames == 0 ? 0 : static_cast<uint32_t>((frames - 1) * 1000 / rate + 1);
}

/// Whether a sample `lengthMs` long plays out (ONCE) rather than stopping at its note-off (CUT).
constexpr bool playsOut(uint32_t lengthMs) {
	return lengthMs < kOneShotMaxLengthMs;
}

} // namespace deluge::generative::perform
