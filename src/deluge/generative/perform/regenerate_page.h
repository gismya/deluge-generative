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

// The Regenerate and genre-change page (spec §13.6, ADR 0008 "The menus"), and the VARIANTS rows it shares with the
// Compose screen (§14.4): what the page holds while open, what GO commits, and what a set's row shows. The row
// layout the OLED names must fit is perform/menu_rows.h. The firmware's stock submenus (gui/menu_item/generative/
// regenerate_page.*) draw it; this decides it, so the harness checks it.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

// ---- A set's allowed variants ----

/// Whether `allowed` lets a draw land on variant `v`.
constexpr bool allows(uint8_t allowed, int32_t v) {
	return v >= 0 && v < kMaxVariants && (allowed >> v & 1U) != 0;
}

/// What a set's VARIANTS row shows (spec §13.6): RANDOM when every variant is allowed, the variant's OLED name when
/// one is, and `n OF m` for a subset.
struct AllowedShown {
	enum class Kind : uint8_t { Random, One, Subset };
	Kind kind = Kind::Random;
	int8_t variant = -1; ///< Kind::One: the variant allowed
	int8_t count = 0;    ///< variants allowed
	int8_t of = 0;       ///< variants in the set
};
AllowedShown allowedShown(const VariantSet& set, uint8_t allowed);

/// `allowed` with variant `v`'s tick turned over, as the set's multi-select does. The last variant allowed keeps its
/// tick: a set always allows one. Bits past the set's variants are dropped.
uint8_t toggledAllowed(const VariantSet& set, uint8_t allowed, int32_t v);

/// Every set of `genre` on RANDOM: each allows all its variants. Picks 0.
VariantPicks allRandom(const Genre& genre);

/// Set `set` of `recipe` now allows `allowed` (the Compose screen's VARIANTS rows, spec §14.4). The variant playing
/// stays while it's allowed; one no longer allowed is drawn again within the ticks from `entropy`, as a Recipe file's
/// reader draws a pick outside its mask (drawUnsetVariants()), so the Recipe never plays or saves a pick its mask
/// leaves out. @return whether the pick changed.
bool narrowSet(Recipe& recipe, int32_t set, uint8_t allowed, uint32_t entropy);

/// A Recipe's picks as test events show them: `<set>=<variant>:<allowed mask>` per set of `genre` by stable names,
/// space-separated (`skank=plain:2`), or `-` for a genre with no sets. Cut short to fit `out`.
void picksText(const Genre& genre, const VariantPicks& picks, std::span<char> out);
/// Each set's allowed mask as test events show them: `<set>:<mask>` per set of `genre`, space-separated
/// (`skank:2`), or `-` for a genre with no sets. Cut short to fit `out`.
void masksText(const Genre& genre, const VariantPicks& picks, std::span<char> out);
/// Room for picksText() or masksText(), the NUL included (Dub's five sets: 80 chars). regenerate_page_spec checks
/// that every built-in genre's longest picks text fits, with room left on the test port's 160-char event line.
constexpr size_t kPicksTextChars = 128;

// ---- The page ----

/// The page's state while it's open. Picking REGENERATE or a genre opens it on the incoming genre (the genre playing
/// for a Regenerate), and a mid-set LOAD on its file's genre with only TRANSITION, because the file brings its own
/// variants. Every open starts afresh: the incoming genre's transition entry and every set on RANDOM. BACK drops it.
class RegeneratePage {
public:
	/// Opens the page for `incoming`, a LOAD's genre when `load`.
	void open(const Genre& incoming, bool load);

	[[nodiscard]] const Genre* genre() const { return genre_; }
	[[nodiscard]] bool load() const { return load_; }
	/// VARIANTS ▸ shows only for a genre with sets, and never for a LOAD.
	[[nodiscard]] bool showsVariants() const;

	[[nodiscard]] TransitionStyle style() const { return style_; }
	void setStyle(TransitionStyle style) { style_ = style; }

	/// The page's allowed masks, which the VARIANTS rows edit.
	[[nodiscard]] VariantPicks& narrowed() { return narrowed_; }
	[[nodiscard]] const VariantPicks& narrowed() const { return narrowed_; }

	/// What GO commits: the transition's style, and the masks the incoming Recipe draws within (for a LOAD, all
	/// RANDOM: the file's masks are what it plays).
	struct Choice {
		TransitionStyle style = TransitionStyle::BuildIntoDrop;
		VariantPicks narrowed;
	};
	[[nodiscard]] Choice go() const;

private:
	const Genre* genre_ = nullptr;
	bool load_ = false;
	TransitionStyle style_ = TransitionStyle::BuildIntoDrop;
	VariantPicks narrowed_;
};

} // namespace deluge::generative::perform
