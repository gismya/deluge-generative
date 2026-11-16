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

// Complexity onto a tier band (spec §8.1): the one rule that picks a pattern's Complexity tier and an Answer's tier
// (spec §5.2). Entries carry a `tier` and a `weight`.

#pragma once

#include "generative/core/draw.h"
#include "generative/core/genre.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

/// The tier effective Complexity reaches in `band`: linear in integer math, with the remainder a keyed chance, drawn
/// with `chanceKey`, of the next tier up. The draw holds while its key does, so a slow turn moves one part at a time.
constexpr int32_t bandTier(TierBand band, int32_t complexity, const DrawKey& chanceKey) {
	const int32_t width = band.high > band.low ? band.high - band.low : 0;
	const int32_t position = complexity * width;
	const int32_t remainder = position % kMacroFull;
	const int32_t tier = band.low + position / kMacroFull;
	return remainder > 0 && drawBelow(chanceKey, kMacroFull) < remainder ? tier + 1 : tier;
}

/// The tier that plays for `tier`: the nearest one at or below it that `entries` have, or failing that their lowest.
template <typename Entry>
constexpr int32_t playingTier(std::span<const Entry> entries, int32_t tier) {
	int32_t playing = -1;
	int32_t lowest = INT32_MAX;
	for (const Entry& entry : entries) {
		if (entry.tier <= tier && entry.tier > playing) {
			playing = entry.tier;
		}
		lowest = entry.tier < lowest ? entry.tier : lowest;
	}
	return playing >= 0 ? playing : lowest;
}

/// A weighted keyed pick among the entries of `tier`, in their order: an index into `entries`, or -1 if none has it.
template <typename Entry>
constexpr int32_t drawInTier(const DrawKey& key, std::span<const Entry> entries, int32_t tier) {
	return drawWeightedIf(key, entries, [tier](const Entry& entry) { return entry.tier == tier; });
}

} // namespace deluge::generative
