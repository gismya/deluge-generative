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

// Keyed draws (spec §6.1–§6.4). Every random choice the Generator makes is a pure hash of
// (seed, part, lane, take, purpose, position), so draws never affect each other: skipping work, computing a muted
// part or turning a macro never shifts any other draw. The core never calls the firmware's random sources.

#pragma once

#include <cstdint>
#include <span>

namespace deluge::generative {

/// What a draw is for. Each purpose keys on the Arc counters it chooses, so repetition is designed in.
/// Append new purposes at the end: the value is part of the key, so reordering changes every event stream.
enum class Purpose : uint8_t {
	DensityJitter, ///< position: pick, step within the pattern
	LineRoll,      ///< position: pick, step within the line, field
	LineMutation,  ///< position: pick, passes since the line was rolled, step, field
	GrooveLength,  ///< position: Groove occurrence
	BreakLength,   ///< position: Break occurrence
	BuildLength,   ///< position: Build occurrence
	GrooveToBuild, ///< position: Groove occurrence
};

/// Everything a draw is keyed on. Position words are whichever Arc counters the purpose names.
struct DrawKey {
	uint32_t seed = 0;
	uint8_t part = 0;
	uint8_t lane = 0xFF; ///< Lane value, or 0xFF for a part-wide draw.
	uint8_t take = 0;
	Purpose purpose = Purpose::DensityJitter;
	uint32_t position[3] = {};
};

namespace detail {
/// A 32-bit integer finaliser with good avalanche ("lowbias32", Chris Wellons). Unsigned, so wrapping is defined.
constexpr uint32_t mix(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}
} // namespace detail

/// A uniform 32-bit value for `key`.
constexpr uint32_t draw(const DrawKey& key) {
	uint32_t h = detail::mix(key.seed ^ 0x9e3779b9U);
	h = detail::mix(h
	                ^ (static_cast<uint32_t>(key.part) | static_cast<uint32_t>(key.lane) << 8
	                   | static_cast<uint32_t>(key.take) << 16 | static_cast<uint32_t>(key.purpose) << 24));
	for (uint32_t word : key.position) {
		h = detail::mix(h ^ word);
	}
	return h;
}

/// A uniform value in [0, n), for 0 < n. Multiply-high, so no modulo bias worth hearing.
constexpr int32_t drawBelow(const DrawKey& key, int32_t n) {
	return static_cast<int32_t>((static_cast<uint64_t>(draw(key)) * static_cast<uint32_t>(n)) >> 32);
}

/// The index of an entry drawn by its `weight`, or -1 for an empty list. A list whose weights are all 0 gives its first
/// entry.
template <typename Entry>
constexpr int32_t drawWeighted(const DrawKey& key, std::span<const Entry> entries) {
	if (entries.empty()) {
		return -1;
	}
	int32_t total = 0;
	for (const Entry& entry : entries) {
		total += entry.weight;
	}
	int32_t pick = drawBelow(key, total > 0 ? total : 1);
	for (int32_t i = 0; i < static_cast<int32_t>(entries.size()); i++) {
		if (pick < entries[i].weight) {
			return i;
		}
		pick -= entries[i].weight;
	}
	return 0;
}

/// True with a chance of `perMille` / 1000.
constexpr bool drawChance(const DrawKey& key, int32_t perMille) {
	return drawBelow(key, 1000) < perMille;
}

} // namespace deluge::generative
