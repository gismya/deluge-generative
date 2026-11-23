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

#include "generative/core/variants.h"

namespace deluge::generative {

namespace {

/// A draw by weight within `allowed`, from `value`.
uint8_t drawWithin(const VariantSet& set, uint8_t allowed, uint32_t value) {
	const int32_t picked = rollWeighted(
	    static_cast<int32_t>(set.variants.size()),
	    [&](int32_t v) { return (allowed >> v) & 1U ? static_cast<int32_t>(set.variants[v].weight) : int32_t{0}; },
	    [&](int32_t total) { return scaleBelow(value, total); });
	return static_cast<uint8_t>(picked >= 0 ? picked : 0);
}

} // namespace

void drawVariants(const Genre& genre, const VariantPicks& narrowed, EntropyStream& rolls, VariantPicks& out) {
	out = VariantPicks{};
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		const VariantSet& set = genre.variantSets[s];
		out.allowed[s] = allowedIn(set, narrowed.allowed[s]);
		out.pick[s] = drawWithin(set, out.allowed[s], rolls.next());
	}
}

void drawUnsetVariants(Recipe& recipe, uint32_t entropy) {
	if (recipe.genre == nullptr) {
		return;
	}
	EntropyStream rolls(entropy);
	const Genre& genre = *recipe.genre;
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		const VariantSet& set = genre.variantSets[s];
		const uint8_t allowed = allowedIn(set, recipe.variants.allowed[s]);
		recipe.variants.allowed[s] = allowed;
		const uint32_t value = rolls.next(); // one a set, used or not, so a set's draw doesn't hang on the others
		const uint8_t pick = recipe.variants.pick[s];
		if (pick >= set.variants.size() || ((allowed >> pick) & 1U) == 0) {
			recipe.variants.pick[s] = drawWithin(set, allowed, value);
		}
	}
}

bool patchesGenre(const Genre& genre, const VariantPicks& picks) {
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		if (pickedVariant(genre, picks, static_cast<int32_t>(s)).apply != nullptr) {
			return true;
		}
	}
	return false;
}

const Genre& resolveGenre(const Genre& genre, const VariantPicks& picks, ResolvedGenre* into) {
	if (into == nullptr || !patchesGenre(genre, picks)) {
		return genre;
	}
	into->genre = genre;
	const int32_t parts = partCount(genre);
	for (int32_t p = 0; p < parts; p++) {
		into->parts[p] = genre.parts[p];
	}
	into->genre.parts = std::span<const PartSpec>(into->parts, parts);
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		const Variant& variant = pickedVariant(genre, picks, static_cast<int32_t>(s));
		if (variant.apply != nullptr) {
			variant.apply(*into);
		}
	}
	return into->genre;
}

int32_t findVariantSet(const Genre& genre, std::string_view name) {
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		if (name == genre.variantSets[s].name) {
			return static_cast<int32_t>(s);
		}
	}
	return -1;
}

int32_t findVariant(const VariantSet& set, std::string_view name) {
	for (size_t v = 0; v < set.variants.size(); v++) {
		if (name == set.variants[v].name) {
			return static_cast<int32_t>(v);
		}
	}
	return -1;
}

std::optional<VariantFix> parseVariantFix(const Genre& genre, std::string_view text) {
	const size_t equals = text.find('=');
	if (equals == std::string_view::npos) {
		return std::nullopt;
	}
	const int32_t set = findVariantSet(genre, text.substr(0, equals));
	if (set < 0) {
		return std::nullopt;
	}
	const int32_t variant = findVariant(genre.variantSets[set], text.substr(equals + 1));
	if (variant < 0) {
		return std::nullopt;
	}
	return VariantFix{set, variant};
}

} // namespace deluge::generative
