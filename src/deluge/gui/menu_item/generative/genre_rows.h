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

// GENRE ▸'s rows (spec §14.1, §13.6), shared by PERFORM FROM and the Perform menu. perform/genre_menu.h lays them
// out: a genre with no parent is a row, and a parent is one row (TECHNO ▸) that opens a submenu of its subgenres by
// their short names. Each menu supplies its own genre item, which says what a pick does; the rows, the parents'
// submenus and where each opens are here.

#pragma once

#include "generative/core/genre.h"
#include "gui/menu_item/submenu.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace deluge::gui::menu_item::generative {

/// Where a genre item sits: a top-level row of GENRE ▸, or a place in the open parent's submenu.
struct GenreSlot {
	int32_t index = 0;
	bool inGroup = false;
};

/// The genre at `slot`, or nullptr: a parent's row, or a slot past the last.
const deluge::generative::Genre* genreAt(GenreSlot slot);
/// A genre item's label: the OLED name on a top-level row, the short name in a parent's submenu (spec §4.4).
std::string_view genreLabel(GenreSlot slot);
/// The genre GENRE ▸ opens on: the Current Recipe's, or the default genre before one exists (spec §14.1).
const deluge::generative::Genre& genreToOpenOn();

/// A parent's row, `row`: the submenu of its subgenres, titled with the parent's name. It opens on the genre
/// GENRE ▸ opens on, when that is one of them. Its items are the menu's genre items for the open parent.
class GenreGroup final : public Submenu {
public:
	GenreGroup(int32_t row, std::span<MenuItem*> items) : Submenu(l10n::String::STRING_FOR_GENRE, items), row_(row) {}
	[[nodiscard]] std::string_view getName() const override;
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	void beginSession(MenuItem* navigatedBackwardFrom) override;

private:
	int32_t row_;
};

/// GENRE ▸ itself. Its items alternate per row: the genre item, then the parent's submenu, of which only one is
/// relevant (GenreMenuItems lays them out). It opens on the row of the genre to open on.
class GenreMenu final : public Submenu {
public:
	using Submenu::Submenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override;
};

/// A menu's GENRE ▸ and every item under it, with `Item` (constructed from a GenreSlot) as its genre item: per row a
/// genre item then a parent's submenu, in the order GenreMenu::beginSession() reads (`2 * row`), and the genre items
/// the open parent's submenu shows. As many of each as there can be genres.
template <typename Item>
class GenreMenuItems {
	static constexpr int32_t kSlots = deluge::generative::kMaxGenres;

public:
	explicit GenreMenuItems(l10n::String name)
	    : GenreMenuItems(name, std::make_integer_sequence<int32_t, kSlots>{},
	                     std::make_integer_sequence<int32_t, 2 * kSlots>{}) {}
	GenreMenuItems(const GenreMenuItems&) = delete;
	GenreMenuItems& operator=(const GenreMenuItems&) = delete;

	/// GENRE ▸ itself.
	[[nodiscard]] GenreMenu& menu() { return menu_; }
	/// The parents' submenus, one per row.
	[[nodiscard]] std::span<GenreGroup> groups() { return groups_; }

private:
	template <int32_t... Slot, int32_t... Row>
	GenreMenuItems(l10n::String name, std::integer_sequence<int32_t, Slot...> /*slots*/,
	               std::integer_sequence<int32_t, Row...> /*rows*/)
	    : subgenreItems_{Item{GenreSlot{Slot, true}}...}, subgenreList_{&subgenreItems_[Slot]...},
	      rowItems_{Item{GenreSlot{Slot}}...}, groups_{GenreGroup{Slot, subgenreList_}...}, rows_{rowItem(Row)...},
	      menu_{name, rows_} {}
	/// GENRE ▸'s `index`th item: row `index / 2`'s genre item, then its parent's submenu.
	MenuItem* rowItem(int32_t index) {
		return index % 2 == 0 ? static_cast<MenuItem*>(&rowItems_[index / 2]) : &groups_[index / 2];
	}

	Item subgenreItems_[kSlots];
	MenuItem* subgenreList_[kSlots];
	Item rowItems_[kSlots];
	GenreGroup groups_[kSlots];
	MenuItem* rows_[2 * kSlots];
	GenreMenu menu_;
};

} // namespace deluge::gui::menu_item::generative
