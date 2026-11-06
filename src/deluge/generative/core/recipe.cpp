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

namespace deluge::generative {

Recipe newRecipe(const Genre& genre, uint32_t entropy) {
	Recipe recipe;
	recipe.genre = &genre;
	recipe.macros = genre.defaults;
	recipe.swing = genre.facts.swing;

	// Facts are rolled from fresh entropy, never the seed (spec §3.2). Each fact takes its own mix of it.
	uint32_t roll = entropy;
	auto next = [&roll](int32_t n) {
		roll = detail::mix(roll + 0x9e3779b9U);
		return static_cast<int32_t>((static_cast<uint64_t>(roll) * static_cast<uint32_t>(n)) >> 32);
	};
	int32_t tempoSpan = genre.facts.tempoHighBpm - genre.facts.tempoLowBpm + 1;
	recipe.tempo = (genre.facts.tempoLowBpm + next(tempoSpan > 0 ? tempoSpan : 1)) * 100;
	recipe.keyRoot = static_cast<uint8_t>(next(12));
	if (!genre.facts.scales.empty()) {
		recipe.scale = genre.facts.scales[next(static_cast<int32_t>(genre.facts.scales.size()))];
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
