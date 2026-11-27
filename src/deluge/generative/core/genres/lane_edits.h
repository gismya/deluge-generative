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

// Copies of a genre's lane table with a few lanes edited, for a variant that swaps a part's lanes (spec §3.6): the
// genres' drum styles.

#pragma once

#include "generative/core/genre.h"

#include <array>
#include <cstddef>
#include <span>

namespace deluge::generative::genres {

/// A copy of `lanes` with `edit` applied to it.
template <size_t N, typename Edit>
constexpr std::array<LaneSpec, N> lanesWith(const LaneSpec (&lanes)[N], Edit edit) {
	std::array<LaneSpec, N> out{};
	for (size_t l = 0; l < N; l++) {
		out[l] = lanes[l];
	}
	edit(out);
	return out;
}

/// Lane `lane` of `lanes`. A table without it doesn't compile.
template <size_t N>
constexpr LaneSpec& laneOf(std::array<LaneSpec, N>& lanes, Lane lane) {
	for (LaneSpec& spec : lanes) {
		if (spec.lane == lane) {
			return spec;
		}
	}
	throw "the lane table has no such lane";
}

/// Whether `a` and `b` have as many entries, each as long and in the same tier as the other's: a pool a live set swaps
/// in for another keeps a frozen part's pick, which indexes it (spec §3.6).
constexpr bool sameShape(std::span<const Pattern> a, std::span<const Pattern> b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t e = 0; e < a.size(); e++) {
		if (a[e].length != b[e].length || a[e].tier != b[e].tier) {
			return false;
		}
	}
	return true;
}

} // namespace deluge::generative::genres
