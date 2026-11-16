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

// The Recipe a genre tuning tool plays (spec §21.5): genre + Recipe overrides + seed. The harness rough render takes
// them as arguments, and a delugemu capture build compiles them in (GENERATIVE_CAPTURE), so the same arguments give
// the same Recipe in both. Facts are rolled from a fixed entropy instead of a timer, so a capture is a function of its
// arguments. Header-only: the default firmware build never uses it.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative {

/// A field left at its default keeps what the genre's new Recipe rolled.
struct RecipeOverrides {
	/// A genre's stable name. nullptr, or a name no genre has, plays the fallback genre.
	const char* genre = nullptr;
	/// Fresh entropy the facts (tempo, key, scale) are rolled from.
	uint32_t entropy = 0;
	uint32_t seed = 1;
	/// OLED points (0–50) per Macro, in Macro order. -1 keeps the genre's default.
	int8_t macroPoints[kNumMacros] = {-1, -1, -1, -1};
	/// Hundredths of a BPM. 0 keeps the rolled tempo.
	int32_t tempo = 0;
	/// Pitch class, 0 = C. -1 keeps the rolled key.
	int8_t keyRoot = -1;
	/// A Scale value. -1 keeps the rolled scale.
	int8_t scale = -1;
	/// -49..49. kKeepSwing keeps the genre's swing.
	int8_t swing = kKeepSwing;

	static constexpr int8_t kKeepSwing = INT8_MIN;
};

struct RolledRecipe {
	Recipe recipe;
	uint32_t seed = 0;
};

/// The genre's new Recipe rolled from `overrides.entropy`, with each override applied.
inline RolledRecipe rollWithOverrides(const Genre& fallback, const RecipeOverrides& overrides) {
	const Genre* genre = overrides.genre != nullptr ? findGenre(overrides.genre) : nullptr;
	RolledRecipe rolled{newRecipe(genre != nullptr ? *genre : fallback, overrides.entropy), overrides.seed};
	Recipe& recipe = rolled.recipe;
	for (int32_t m = 0; m < kNumMacros; m++) {
		if (overrides.macroPoints[m] >= 0) {
			recipe.macros.set(static_cast<Macro>(m), macroPoints(overrides.macroPoints[m]));
		}
	}
	if (overrides.tempo > 0) {
		recipe.tempo = overrides.tempo;
	}
	if (overrides.keyRoot >= 0) {
		recipe.keyRoot = static_cast<uint8_t>(overrides.keyRoot % 12);
	}
	if (overrides.scale >= 0) {
		recipe.scale = static_cast<Scale>(overrides.scale);
	}
	if (overrides.swing != RecipeOverrides::kKeepSwing) {
		recipe.swing = overrides.swing;
	}
	return rolled;
}

} // namespace deluge::generative
