/*
 * Copyright © 2026 Synthstrom Audible Limited
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

/// @file
/// @brief Maps a sysex firmware load's progress onto the 8x18 main pad grid.

#pragma once

#include <algorithm>
#include <cstdint>

namespace deluge::io::midi {

constexpr uint32_t kLoadProgressPads = 18 * 8;

/// Index of the pad that lights for the packet at byte offset @p pos of a @p bufSize load buffer, counted row by
/// row from the top left. Always less than kLoadProgressPads, so it can index PadLEDs::image directly.
constexpr uint32_t loadProgressPad(uint32_t pos, uint32_t bufSize) {
	if (bufSize == 0) {
		return 0;
	}
	uint64_t pad = (uint64_t{kLoadProgressPads} * pos) / bufSize;
	return static_cast<uint32_t>(std::min<uint64_t>(pad, kLoadProgressPads - 1));
}

} // namespace deluge::io::midi
