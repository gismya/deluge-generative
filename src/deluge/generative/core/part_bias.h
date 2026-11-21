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
// 50/50 blend of the linear and the headroom-proportional curves by ear (#414; the knee hybrid before it).
// partBiased() is the one function to change, and biasFor() works from it whatever its shape, as long as it stays
// monotonic in the bias.

#pragma once

#include "generative/core/recipe.h"

#include <algorithm>
#include <cstdint>

namespace deluge::generative {

/// `shared`, the value every part plays a macro at (after the Arc, effectiveOn()), moved by a part's `bias`, both in
/// macro units. The 50/50 blend: the mean of the linear curve, `shared + bias` clamped to 0..kMacroFull, and the
/// headroom-proportional one, which moves `shared` by bias/kMacroFull of the room towards the end the bias heads for
/// (50 up, 0 down). So through the middle a part moves about three quarters of its bias, and it reaches each end
/// together with the shared value. The identity at bias 0, monotonic in both arguments, within 0..kMacroFull, and
/// one unit of `shared` moves it by one unit at most. Integer only (ADR 0004): one division, rounding down.
constexpr int32_t partBiased(int32_t shared, int32_t bias) {
	const int64_t o = std::clamp<int32_t>(bias, -kMacroFull, kMacroFull);
	if (o == 0) {
		return shared;
	}
	const int64_t full = kMacroFull;
	const int64_t v = shared;
	const int64_t linear = std::clamp<int64_t>(v + o, 0, full);
	// The curved value times kMacroFull: up, v + o·(full − v)/full; down, v + o·v/full. Never negative, since |o| ≤
	// full.
	const int64_t curved = o > 0 ? full * v + o * (full - v) : full * v + o * v;
	return static_cast<int32_t>((full * linear + curved) / (2 * full));
}

/// The bias that puts a part at `target` from `centre` (a KEYBOARD fader row, spec §13.5): the smallest one whose
/// partBiased() reaches it (about 4/3 of `target - centre` through the middle). 0 when they are equal.
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
