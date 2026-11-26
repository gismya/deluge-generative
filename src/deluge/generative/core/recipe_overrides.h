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
// them as arguments, and a delugemu capture sets them at run time with a test build's "next recipe" lines (NextRecipe,
// seam S7), so the same arguments give the same Recipe in both. Facts are rolled from a fixed entropy instead of a
// timer, so a capture is a function of its arguments. Header-only: the default firmware build never uses it.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/variants.h"
#include <algorithm>
#include <cstdint>

namespace deluge::generative {

/// A field left at its default keeps what the genre's new Recipe rolled.
struct RecipeOverrides {
	/// A genre's stable name. nullptr, or a name no genre has, plays the fallback genre.
	const char* genre = nullptr;
	/// Fresh entropy the facts (tempo, key, scale, swing) are rolled from.
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
	/// -49..49. kKeepSwing keeps the rolled swing.
	int8_t swing = kKeepSwing;
	/// Per variant set of the genre, by index: a variant that set is fixed to (its pick, and its allowed variants that
	/// one alone), or -1 to keep the drawn pick (spec §3.6). The goldens fix every set to 0 (golden_case.h).
	int8_t variant[kMaxVariantSets] = {-1, -1, -1, -1, -1, -1, -1, -1};

	void clearVariants() { std::ranges::fill(variant, int8_t{-1}); }

	static constexpr int8_t kKeepSwing = INT8_MIN;
};

static_assert(kMaxVariantSets == 8, "RecipeOverrides::variant's initializer has one -1 a set");

/// Fixes each of `recipe`'s variant sets that `variant` names (0 or more) to that variant (fixVariant()). A value past
/// the set's variants is skipped.
inline void applyVariantOverrides(Recipe& recipe, const int8_t (&variant)[kMaxVariantSets]) {
	const Genre& genre = *recipe.genre;
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		if (variant[s] >= 0 && static_cast<size_t>(variant[s]) < genre.variantSets[s].variants.size()) {
			fixVariant(recipe.variants, {static_cast<int32_t>(s), variant[s]});
		}
	}
}

struct RolledRecipe {
	Recipe recipe;
	uint32_t seed = 0;
};

/// Each override's macro, fact and variant over `recipe`; the genre, entropy and seed are the roll's.
inline void applyOverrides(Recipe& recipe, const RecipeOverrides& overrides) {
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
	applyVariantOverrides(recipe, overrides.variant);
}

/// The genre's new Recipe rolled from `overrides.entropy`, with each override applied.
inline RolledRecipe rollWithOverrides(const Genre& fallback, const RecipeOverrides& overrides) {
	const Genre* genre = overrides.genre != nullptr ? findGenre(overrides.genre) : nullptr;
	RolledRecipe rolled{newRecipe(genre != nullptr ? *genre : fallback, overrides.entropy), overrides.seed};
	applyOverrides(rolled.recipe, overrides);
	return rolled;
}

/// The Recipe and seed a test build's "next recipe" lines set for the next roll only (seam S7, spec §21.3): overrides
/// plus sound picks. Left out, the genre is the one the roll is for, entropy and seed are fresh, and a pick is rolled
/// from the palette.
struct NextRecipe {
	RecipeOverrides overrides;
	/// The genre `overrides.variant` was resolved against (nullptr when no variant line was set): a Regenerate that
	/// keeps another genre skips them rather than indexing its sets with the wrong genre's indices.
	const Genre* variantGenre = nullptr;
	bool entropySet = false;
	bool seedSet = false;
	/// An empty pick is rolled from the palette as usual.
	SoundPicks sounds;

	/// Drops any resolved variant picks (e.g. when `overrides.genre` changes to another genre).
	void forgetVariants() {
		overrides.clearVariants();
		variantGenre = nullptr;
	}
	/// As constructed, without a SoundPicks temporary (several KB) on the stack.
	void clear() {
		overrides = RecipeOverrides{};
		variantGenre = nullptr;
		entropySet = seedSet = false;
		for (SoundPath& path : sounds.part) {
			path.clear();
		}
		for (SoundPath& path : sounds.lane) {
			path.clear();
		}
	}
	/// Each pick set here over the rolled ones.
	void applySounds(SoundPicks& picks) const {
		for (int32_t p = 0; p < kMaxParts; p++) {
			if (!sounds.part[p].empty()) {
				picks.part[p] = sounds.part[p];
			}
		}
		for (int32_t l = 0; l < kNumLanes; l++) {
			if (!sounds.lane[l].empty()) {
				picks.lane[l] = sounds.lane[l];
			}
		}
	}
};

} // namespace deluge::generative
