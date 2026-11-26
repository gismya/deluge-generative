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

#include "generative/perform/regenerate_page.h"
#include "generative/core/variants.h"
#include "generative/perform/genre_menu.h"
#include "generative/perform/text.h"
#include <bit>

namespace deluge::generative::perform {

AllowedShown allowedShown(const VariantSet& set, uint8_t allowed) {
	const uint8_t kept = allowedIn(set, allowed);
	const auto of = static_cast<int8_t>(set.variants.size());
	const auto count = static_cast<int8_t>(std::popcount(kept));
	if (count == of) {
		return {.kind = AllowedShown::Kind::Random, .count = count, .of = of};
	}
	if (count == 1) {
		return {.kind = AllowedShown::Kind::One,
		        .variant = static_cast<int8_t>(std::countr_zero(kept)),
		        .count = count,
		        .of = of};
	}
	return {.kind = AllowedShown::Kind::Subset, .count = count, .of = of};
}

uint8_t toggledAllowed(const VariantSet& set, uint8_t allowed, int32_t v) {
	const uint8_t kept = allowedIn(set, allowed);
	if (v < 0 || v >= static_cast<int32_t>(set.variants.size())) {
		return kept;
	}
	const auto turned = static_cast<uint8_t>(kept ^ (1U << v));
	return turned != 0 ? turned : kept;
}

VariantPicks allRandom(const Genre& genre) {
	VariantPicks picks;
	for (size_t s = 0; s < genre.variantSets.size(); s++) {
		picks.allowed[s] = allVariants(genre.variantSets[s]);
	}
	return picks;
}

void picksText(const Genre& genre, const VariantPicks& picks, std::span<char> out) {
	if (out.empty()) {
		return;
	}
	Text text(out);
	if (genre.variantSets.empty()) {
		text.add("-");
		return;
	}
	for (size_t s = 0; s < genre.variantSets.size(); s++) {
		text.add(s > 0 ? " " : "").add(genre.variantSets[s].name).add("=");
		text.add(pickedVariant(genre, picks, static_cast<int32_t>(s)).name).add(":").add(picks.allowed[s]);
	}
}

bool narrowSet(Recipe& recipe, int32_t set, uint8_t allowed, uint32_t entropy) {
	if (recipe.genre == nullptr || set < 0 || set >= static_cast<int32_t>(recipe.genre->variantSets.size())) {
		return false;
	}
	const uint8_t before = recipe.variants.pick[set];
	recipe.variants.allowed[set] = allowedIn(recipe.genre->variantSets[set], allowed);
	drawUnsetVariants(recipe, entropy); // draws only the picks outside their masks: this one, if it now is
	return recipe.variants.pick[set] != before;
}

void masksText(const Genre& genre, const VariantPicks& picks, std::span<char> out) {
	if (out.empty()) {
		return;
	}
	Text text(out);
	if (genre.variantSets.empty()) {
		text.add("-");
		return;
	}
	for (size_t s = 0; s < genre.variantSets.size(); s++) {
		text.add(s > 0 ? " " : "").add(genre.variantSets[s].name).add(":").add(picks.allowed[s]);
	}
}

void RegeneratePage::open(const Genre& incoming, bool load) {
	genre_ = &incoming;
	load_ = load;
	style_ = confirmStyle(incoming);
	narrowed_ = allRandom(incoming);
}

bool RegeneratePage::showsVariants() const {
	return genre_ != nullptr && !load_ && !genre_->variantSets.empty();
}

RegeneratePage::Choice RegeneratePage::go() const {
	return {.style = style_, .narrowed = load_ ? VariantPicks{} : narrowed_};
}

} // namespace deluge::generative::perform
