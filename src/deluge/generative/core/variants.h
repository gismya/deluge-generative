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

// Variant sets (spec §3.6, ADR 0008): a genre lists rules a piece draws one variant of, the Recipe stores the picks,
// and an instance plays a copy of the genre with the picked variants' patches applied, so the step reads genre data as
// it always has.

#pragma once

#include "generative/core/draw.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace deluge::generative {

/// A genre and its parts, copied so a variant's patch can write them (spec §3.6). An instance plays one when the
/// Recipe's picks patch anything; it lives outside the Generator block (spec §20), owned beside it. `genre.parts`
/// points at `parts`, so it is never copied.
struct ResolvedGenre {
	ResolvedGenre() = default;
	ResolvedGenre(const ResolvedGenre&) = delete;
	ResolvedGenre& operator=(const ResolvedGenre&) = delete;

	Genre genre;
	PartSpec parts[kMaxParts];
};

/// The variants a draw may land on in `set` when every one is allowed.
constexpr uint8_t allVariants(const VariantSet& set) {
	return static_cast<uint8_t>((1U << set.variants.size()) - 1U);
}

/// `allowed` kept to `set`'s variants, or all of them when that leaves none.
constexpr uint8_t allowedIn(const VariantSet& set, uint8_t allowed) {
	const uint8_t kept = allowed & allVariants(set);
	return kept != 0 ? kept : allVariants(set);
}

/// The variant `picks` plays in set `s` of `genre`: its pick, or the first when the pick isn't one of the set's.
constexpr const Variant& pickedVariant(const Genre& genre, const VariantPicks& picks, int32_t s) {
	const std::span<const Variant> variants = genre.variantSets[s].variants;
	const uint8_t pick = picks.pick[s];
	return variants[pick < variants.size() ? pick : 0];
}

/// Draws each of `genre`'s sets into `out` by weight within `narrowed`'s allowed variants (allowedIn()), one value
/// of `rolls` a set, and stores the allowed masks with them. Sets past the genre's are 0.
void drawVariants(const Genre& genre, const VariantPicks& narrowed, EntropyStream& rolls, VariantPicks& out);

/// Draws each set whose pick is kUnsetVariant, or outside its allowed variants, within them (a Recipe file's set it
/// left out or a pick it didn't know, spec §16.1), from `entropy`. The others keep their picks.
void drawUnsetVariants(Recipe& recipe, uint32_t entropy);

/// Whether `genre` has a live set `set` with a variant `variant`: what a live switch may name (spec §3.6, §13.6).
bool switchable(const Genre& genre, int32_t set, int32_t variant);

/// A live switch lands: set `set` plays `variant`, which joins the set's allowed variants, so a Drop change, SAVE and
/// the file keep it (spec §3.6). The one place that rule lives.
void switchVariant(VariantPicks& picks, int32_t set, int32_t variant);

/// The variant set `s` plays after the Drop that opens Groove `groove` (spec §3.6), for a ChangeAt::Drop `set` playing
/// `pick` within `allowed`: a keyed draw (seed, kArcPart, VariantChange, the Groove, the set) against its
/// changeChance decides whether it changes, and a second picks the new one by weight within `allowed`, leaving out
/// `pick`. A set that allows nothing but `pick` keeps it.
uint8_t dropChange(const VariantSet& set, uint8_t allowed, uint8_t pick, uint32_t seed, uint16_t groove, int32_t s);

/// Whether `picks` patch anything: a picked variant of `genre` has an apply().
bool patchesGenre(const Genre& genre, const VariantPicks& picks);

/// The genre an instance plays for `genre` at `picks` (spec §3.6): `genre` itself when no picked variant patches it,
/// else `into`, made a copy of it with each picked variant's patch applied in set order. `into` may be null only when
/// nothing patches.
const Genre& resolveGenre(const Genre& genre, const VariantPicks& picks, ResolvedGenre* into);

/// Set `s` of `genre` by its stable name, or -1.
int32_t findVariantSet(const Genre& genre, std::string_view name);
/// Variant `v` of `set` by its stable name, or -1.
int32_t findVariant(const VariantSet& set, std::string_view name);

/// One set fixed to one variant, by their indices in the genre's lists.
struct VariantFix {
	int32_t set = 0;
	int32_t variant = 0;
};

/// `text`, `set=name` by stable names (the tuning tools' --variant, spec §21.5), as indices in `genre`'s lists, or
/// nullopt if it has no such set or variant.
std::optional<VariantFix> parseVariantFix(const Genre& genre, std::string_view text);

/// Fixes `fix.set` to `fix.variant`: the pick, and the allowed variants that one alone. The one place that rule lives.
constexpr void fixVariant(VariantPicks& picks, VariantFix fix) {
	picks.pick[fix.set] = static_cast<uint8_t>(fix.variant);
	picks.allowed[fix.set] = static_cast<uint8_t>(1U << fix.variant);
}

namespace detail {

constexpr int32_t textLength(const char* text) {
	int32_t length = 0;
	while (text != nullptr && text[length] != 0) {
		length++;
	}
	return length;
}

constexpr bool sameText(const char* a, const char* b) {
	return std::string_view(a) == std::string_view(b);
}

} // namespace detail

/// The genre's variant sets hold the rules ADR 0008 sets (spec §4.4): at most kMaxVariantSets, each with 2 to
/// kMaxVariants variants of weight above 0, stable names that are there (a variant's at most kMaxVariantNameChars)
/// and unique in their list, OLED names that fit a menu row, and a change chance of 1 to 1000 per mille for a set that
/// changes at a Drop (0 for one that never does).
constexpr bool variantSetsValid(const Genre& genre) {
	if (genre.variantSets.size() > kMaxVariantSets) {
		return false;
	}
	for (size_t s = 0; s < genre.variantSets.size(); s++) {
		const VariantSet& set = genre.variantSets[s];
		if (set.variants.size() < 2 || set.variants.size() > kMaxVariants || detail::textLength(set.name) == 0
		    || detail::textLength(set.oledName) == 0 || detail::textLength(set.oledName) > kMaxVariantSetOledName
		    || (set.changeAt == ChangeAt::Drop ? set.changeChance < 1 || set.changeChance > 1000
		                                       : set.changeChance != 0)) {
			return false;
		}
		for (size_t other = 0; other < s; other++) {
			if (detail::sameText(set.name, genre.variantSets[other].name)) {
				return false;
			}
		}
		for (size_t v = 0; v < set.variants.size(); v++) {
			const Variant& variant = set.variants[v];
			if (variant.weight == 0 || detail::textLength(variant.name) == 0
			    || detail::textLength(variant.name) > kMaxVariantNameChars || detail::textLength(variant.oledName) == 0
			    || detail::textLength(variant.oledName) > kMaxVariantOledName) {
				return false;
			}
			for (size_t other = 0; other < v; other++) {
				if (detail::sameText(variant.name, set.variants[other].name)) {
					return false;
				}
			}
		}
	}
	return true;
}

} // namespace deluge::generative
