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

#include "generative/core/recipe.h"
#include "generative/core/draw.h"
#include "generative/core/genre.h"
#include <bit>

namespace deluge::generative {

Recipe newRecipe(const Genre& genre, uint32_t entropy, std::optional<Key> leanFrom) {
	Recipe recipe;
	recipe.genre = &genre;
	recipe.macros = genre.defaults;
	recipe.swing = genre.facts.swing;

	// Facts are rolled from fresh entropy, never the seed (spec §3.2). Each fact takes its own mix of it.
	uint32_t roll = entropy;
	auto nextRoll = [&roll]() {
		roll = detail::mix(roll + 0x9e3779b9U);
		return roll;
	};
	auto below = [](uint32_t draw, int32_t n) {
		return static_cast<int32_t>((static_cast<uint64_t>(draw) * static_cast<uint32_t>(n)) >> 32);
	};
	int32_t tempoSpan = genre.facts.tempoHighBpm - genre.facts.tempoLowBpm + 1;
	recipe.tempo = (genre.facts.tempoLowBpm + below(nextRoll(), tempoSpan > 0 ? tempoSpan : 1)) * 100;
	const uint32_t rootDraw = nextRoll();
	const uint32_t scaleDraw = nextRoll();
	// The scale first, so a lean knows which keys are related (spec §9.7).
	if (!genre.facts.scales.empty()) {
		recipe.scale = genre.facts.scales[below(scaleDraw, static_cast<int32_t>(genre.facts.scales.size()))];
	}
	const uint16_t related = leanFrom.has_value() ? relatedRoots(*leanFrom, recipe.scale) : 0;
	const int32_t relatedCount = std::popcount(related);
	if (relatedCount == 0) {
		recipe.keyRoot = static_cast<uint8_t>(below(rootDraw, 12));
		return recipe;
	}
	// In units of 1/(12 × 1000 × relatedCount) of the roll: each related root gets the lean's share split between
	// them, and every root an even share of the rest.
	const int32_t everyRoot = (1000 - kKeyLeanPerMille) * relatedCount;
	const int32_t total = 12 * everyRoot + relatedCount * 12 * kKeyLeanPerMille;
	int32_t pick = below(rootDraw, total);
	for (uint8_t root = 0; root < 12; root++) {
		const int32_t weight = everyRoot + ((related >> root) & 1U ? 12 * kKeyLeanPerMille : 0);
		if (pick < weight) {
			recipe.keyRoot = root;
			break;
		}
		pick -= weight;
	}
	return recipe;
}

bool SoundPath::set(const char* folder, const char* name) {
	int32_t length = 0;
	auto append = [this, &length](const char* from) {
		for (; *from != 0; from++) {
			if (length >= kMaxSoundPath - 1) {
				return false;
			}
			text[length++] = *from;
		}
		return true;
	};
	bool fits = append(folder) && (name == nullptr || (append("/") && append(name)));
	text[fits ? length : 0] = 0;
	return fits;
}

} // namespace deluge::generative
