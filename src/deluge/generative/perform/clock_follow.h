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

// How Perform follows the master's clock when it changes rate or position (spec §12.3, §12.4). The firmware's tick
// decides with these from the plain facts it reads, and the specs ask the same functions.

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

/// What the tick does when it finds the song's input tick magnitude changed since its last call.
enum class MagnitudeChange : uint8_t {
	None,    ///< the magnitude is the same
	Rescale, ///< an external Start's magnitude matching is settling: count on at the new rate, and that's all
	Jump,    ///< a clock scale change mid-piece: a jump where the master doesn't move (PerformDriver::jump())
};

/// The magnitude was `before` at the last call and is `now`. Matching can end on the same input tick that changes
/// the magnitude, so a change counts as matching if it ran at the last call (`matchingBefore`) or runs now
/// (`matchingNow`). Any other change mid-piece is a jump: counting on would move the piece by its whole elapsed
/// length (spec §12.3).
[[nodiscard]] constexpr MagnitudeChange magnitudeChange(int32_t before, int32_t now, bool matchingBefore,
                                                        bool matchingNow) {
	if (now == before) {
		return MagnitudeChange::None;
	}
	return matchingBefore || matchingNow ? MagnitudeChange::Rescale : MagnitudeChange::Jump;
}

/// Whether a song position pointer is a jump: one that comes while the piece runs makes the next tick jump to it
/// (PerformDriver::jumpTo(), at that tick's magnitude, after any magnitude change). While stopped there's nothing
/// to move: the next start places step 0 from the transport's position (spec §12.4).
[[nodiscard]] constexpr bool pointerJumps(bool running) {
	return running;
}

} // namespace deluge::generative::perform
