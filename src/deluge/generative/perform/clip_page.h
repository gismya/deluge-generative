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

// The CLIP page's takes row and Y turn (spec §6.6, §13.5): row 1 shows a part's takes 1–16, the one playing bright,
// earlier ones dim, the next new take grey and a queued one blinking. Y turned steps back or forward through them;
// forward past the newest makes a new take. And the seed code Y push shows (spec §6.5).

#pragma once

#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::perform {

/// A part's takes as the CLIP page reads them.
struct TakeRow {
	int32_t playing = 0;  ///< The take playing, 0–15.
	int32_t waiting = -1; ///< The take queued for the next phrase line, or -1.
	int32_t newest = 0;   ///< The highest take the part has played in this Recipe.
};

enum class TakeLook : uint8_t {
	Off,     ///< past the next new take
	Playing, ///< bright
	Earlier, ///< a take made already: dim
	New,     ///< the next new take: grey
	Queued,  ///< waiting for its line: blinking
};
/// How take pad `take` (0–15) looks. A waiting take counts as made.
TakeLook takeLook(const TakeRow& row, int32_t take);

/// Whether tapping take pad `take` queues it: any lit pad, up to the next new take.
bool takeTappable(const TakeRow& row, int32_t take);

/// The take a Y turn queues: one back (`direction` < 0) or forward from the waiting take, or else the one playing.
/// Forward past the newest is the next new take. -1 before the first take or past the last.
int32_t steppedTake(const TakeRow& row, int32_t direction);

/// A seed code's length: 7 base-32 characters cover 32 bits (spec §6.5).
constexpr int32_t kSeedCodeChars = 7;
/// Writes `seed` as its seed code, most significant character first, in Crockford's base 32 (0–9 and A–Z without I,
/// L, O and U), which QwertyUI accepts and the OLED can't confuse.
void seedCode(uint32_t seed, char (&out)[kSeedCodeChars + 1]);

} // namespace deluge::generative::perform
