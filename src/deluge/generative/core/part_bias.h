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

// Part bias's curve (spec §3.3): how a part's signed bias moves the value it plays a macro at. The owner picked the
// knee hybrid by ear (#414) and may swap it after listening: partBiased() is the one function to change, and
// biasFor() works from it whatever its shape, as long as it stays monotonic in the bias.

#pragma once

#include "generative/core/recipe.h"

#include <algorithm>
#include <cstdint>

namespace deluge::generative {

/// The narrowest knee: a small bias eases into its end over the last 10 points, instead of hitting it (tuning).
constexpr int32_t kPartBiasKneeMin = macroPoints(10);

/// `shared`, the value every part plays a macro at (after the Arc, effectiveOn()), moved by a part's `bias`, both in
/// macro units. The knee hybrid: exactly `shared + bias` while the end the bias heads for (50 up, 0 down) is at least
/// K = min(50, max(10, 2·|bias|)) points away. Inside that knee the bias shrinks in step with the room left (bias·d/K
/// for d from the end), so the part reaches the end together with the shared value and keeps following the Arc at
/// half its speed or more. From |bias| 25 the knee spans the whole range (headroom-proportional). The identity at
/// bias 0, monotonic in both arguments, within 0..kMacroFull. Integer only (ADR 0004).
constexpr int32_t partBiased(int32_t shared, int32_t bias) {
	const int32_t o = std::clamp<int32_t>(bias, -kMacroFull, kMacroFull);
	if (o == 0) {
		return shared;
	}
	const int32_t magnitude = o < 0 ? -o : o;
	const int32_t knee = std::min<int32_t>(kMacroFull, std::max<int32_t>(kPartBiasKneeMin, 2 * magnitude));
	const int32_t room = o > 0 ? kMacroFull - shared : shared; // the distance to the end the bias heads for
	if (room >= knee) {
		return shared + o;
	}
	// Up rounds towards the shared value, down away from it (as the prototype); neither passes its end, since
	// |bias| ≤ K.
	if (o > 0) {
		return shared + static_cast<int32_t>(int64_t{o} * room / knee);
	}
	return shared - static_cast<int32_t>((int64_t{magnitude} * room + knee - 1) / knee);
}

/// The bias that puts a part at `target` from `centre` (a KEYBOARD fader row, spec §13.5): the smallest one whose
/// partBiased() reaches it, `target - centre` in the curve's linear middle. 0 when they are equal.
constexpr int32_t biasFor(int32_t centre, int32_t target) {
	if (target == centre) {
		return 0;
	}
	const bool up = target > centre;
	// Binary search over the bias's size: partBiased() is monotonic in the bias, and ±kMacroFull reaches either end.
	int32_t low = 0;
	int32_t high = kMacroFull;
	while (low < high) {
		const int32_t mid = (low + high) / 2;
		const bool reaches = up ? partBiased(centre, mid) >= target : partBiased(centre, -mid) <= target;
		if (reaches) {
			high = mid;
		}
		else {
			low = mid + 1;
		}
	}
	return up ? low : -low;
}

} // namespace deluge::generative
