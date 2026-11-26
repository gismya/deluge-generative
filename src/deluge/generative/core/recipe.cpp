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
#include "generative/core/variants.h"
#include <bit>

namespace deluge::generative {

namespace {

/// The key's root: an even roll, or one leaning towards the roots related to `leanFrom` (spec §9.7).
uint8_t rollRoot(uint32_t rootDraw, Scale scale, std::optional<Key> leanFrom) {
	const uint16_t related = leanFrom.has_value() ? relatedRoots(*leanFrom, scale) : 0;
	const int32_t relatedCount = std::popcount(related);
	if (relatedCount == 0) {
		return static_cast<uint8_t>(scaleBelow(rootDraw, 12));
	}
	// In units of 1/(12 × 1000 × relatedCount) of the roll: each related root gets the lean's share split between
	// them, and every root an even share of the rest.
	const int32_t everyRoot = (1000 - kKeyLeanPerMille) * relatedCount;
	const int32_t root = rollWeighted(
	    12, [&](int32_t r) { return everyRoot + ((related >> r) & 1U ? 12 * kKeyLeanPerMille : 0); },
	    [&](int32_t total) { return scaleBelow(rootDraw, total); });
	return static_cast<uint8_t>(root >= 0 ? root : 0);
}

} // namespace

Recipe newRecipe(const Genre& genre, uint32_t entropy, std::optional<Key> leanFrom, const VariantPicks* narrowed) {
	Recipe recipe;
	recipe.genre = &genre;
	recipe.macros = genre.defaults;

	// Facts are rolled from fresh entropy, never the seed (spec §3.2). Each fact takes its own mix of it.
	EntropyStream rolls(entropy);
	int32_t tempoSpan = genre.facts.tempoHighBpm - genre.facts.tempoLowBpm + 1;
	recipe.tempo = (genre.facts.tempoLowBpm + rolls.below(tempoSpan > 0 ? tempoSpan : 1)) * 100;
	const uint32_t rootDraw = rolls.next();
	const uint32_t scaleDraw = rolls.next();
	// Swing after tempo, root and scale, so the facts any entropy gave before stay the same (ADR 0008).
	const int32_t swingSpan = genre.facts.swingHigh - genre.facts.swingLow + 1;
	recipe.swing = static_cast<int8_t>(genre.facts.swingLow + rolls.below(swingSpan > 0 ? swingSpan : 1));
	// The scale first, so a lean knows which keys are related (spec §9.7).
	if (!genre.facts.scales.empty()) {
		recipe.scale = genre.facts.scales[scaleBelow(scaleDraw, static_cast<int32_t>(genre.facts.scales.size()))];
	}
	recipe.keyRoot = rollRoot(rootDraw, recipe.scale, leanFrom);
	// The variant sets last, one value a set: a later draw appends after them (spec §3.6, ADR 0008).
	drawVariants(genre, narrowed != nullptr ? *narrowed : VariantPicks{}, rolls, recipe.variants);
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
