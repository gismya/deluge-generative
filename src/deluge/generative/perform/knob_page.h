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

// Which F page the gold knobs show for a focused part (spec §13.5). The page lives on each Output (stock's
// modKnobMode), but Thin / Drive is on a part: a sound swap gives the part a new Output with its own page, and
// Thin / Drive must carry over to it (#204).

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

/// F7, as a mod button index: a focused part's stutter / CUSTOM 1, and Thin / Drive when pressed again.
inline constexpr uint8_t kThinDriveModButton = 6;

/// Whether Thin / Drive is on the part the gold knobs are bound to: `focused` (-1: the song), with Thin / Drive on
/// `thinDrivePart` (-1: none).
[[nodiscard]] constexpr bool thinDriveOn(int32_t focused, int32_t thinDrivePart) {
	return focused >= 0 && focused == thinDrivePart;
}

/// The part Thin / Drive is on once the gold knobs are bound to `focused` (-1: the song), given the part it was on
/// (-1: none). It stays on its part, whatever that part's Output is now, and ends when focus is anywhere else.
[[nodiscard]] constexpr int32_t thinDriveAfterBind(int32_t focused, int32_t thinDrivePart) {
	return thinDriveOn(focused, thinDrivePart) ? thinDrivePart : -1;
}

/// The F page for the gold knobs bound to `focused`, whose Output's own page is `ownPage`, with Thin / Drive on
/// `thinDrivePart` (thinDriveAfterBind()): F7 while Thin / Drive is on that part, else the Output's own page.
[[nodiscard]] constexpr uint8_t knobPage(int32_t focused, int32_t thinDrivePart, uint8_t ownPage) {
	return thinDriveOn(focused, thinDrivePart) ? kThinDriveModButton : ownPage;
}

} // namespace deluge::generative::perform
