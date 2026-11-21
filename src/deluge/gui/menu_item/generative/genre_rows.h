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
/// The genre at `slot` for a genre item's push, or nullptr with the stock error shown: can't happen, a slot with no
/// genre hides.
const deluge::generative::Genre* genreOrError(GenreSlot slot);
/// A genre item's label: the OLED name on a top-level row, the short name in a parent's submenu (spec §4.4).
std::string_view genreLabel(GenreSlot slot);
/// The genre GENRE ▸ opens on: the Current Recipe's, or the default genre before one exists (spec §14.1).
const deluge::generative::Genre& genreToOpenOn();

#if ENABLE_SYSEX_TEST
/// What GENRE ▸ and a parent's submenu share in test builds: picking a genre by name (pickGenreForTest()). Other
/// builds use Submenu itself, so the default build stays as it was.
class GenreSubmenu : public Submenu {
public:
	using Submenu::Submenu;
	/// Test port: focuses the item `genre` sits on in this menu, as turning to it would. Sets `row` to the label now
	/// focused and `from` to the one focused before, or returns false: `genre` isn't in this menu (nothing moved).
	bool focusGenre(const deluge::generative::Genre& genre, std::string_view& row, std::string_view& from);

private:
	/// The item `genre` sits on in this menu, or nullptr.
	[[nodiscard]] virtual MenuItem* itemFor(const deluge::generative::Genre& genre) = 0;
};
#else
using GenreSubmenu = Submenu;
#endif

/// A parent's row, `row`: the submenu of its subgenres, titled with the parent's name. It opens on the genre
/// GENRE ▸ opens on, when that is one of them. Its items are the menu's genre items for the open parent.
class GenreGroup final : public GenreSubmenu {
public:
	GenreGroup(int32_t row, std::span<MenuItem*> items)
	    : GenreSubmenu(l10n::String::STRING_FOR_GENRE, items), row_(row) {}
	[[nodiscard]] std::string_view getName() const override;
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	void beginSession(MenuItem* navigatedBackwardFrom) override;

private:
#if ENABLE_SYSEX_TEST
	/// `genre`'s place, if it is one of this parent's.
	[[nodiscard]] MenuItem* itemFor(const deluge::generative::Genre& genre) override;
#endif
	int32_t row_;
};

/// GENRE ▸ itself. Its items alternate per row: the genre item, then the parent's submenu, of which only one is
/// relevant (GenreMenuItems lays them out). It opens on the row of the genre to open on.
class GenreMenu final : public GenreSubmenu {
public:
	using GenreSubmenu::GenreSubmenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override;

#if ENABLE_SYSEX_TEST
private:
	/// `genre`'s row: its parent's submenu, for a subgenre.
	[[nodiscard]] MenuItem* itemFor(const deluge::generative::Genre& genre) override;
#endif
};

#if ENABLE_SYSEX_TEST
/// Test port ("pick genre <name>"): if `current`, the menu item the sound editor shows, is the GENRE ▸ or parent's
/// submenu whose session began last, focuses `genre`'s row in it as an encoder turn to it would, so a script picks a
/// genre by name rather than by counting rows. In GENRE ▸ a subgenre's row is its parent's. Sets `row` to the label
/// now focused and `from` to the one focused before (where the menu opened, right after a SELECT), or returns false:
/// no genre menu is open, or `genre` isn't in the open one.
bool pickGenreForTest(MenuItem* current, const deluge::generative::Genre& genre, std::string_view& row,
                      std::string_view& from);
#endif

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
