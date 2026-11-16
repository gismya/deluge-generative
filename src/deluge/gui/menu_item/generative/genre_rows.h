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
#if ENABLE_SYSEX_TEST
	/// Test port: focuses `genre`'s place, as turning to it would, and sets `from` to the label focused before. False
	/// if it isn't one of this parent's.
	bool focusGenre(const deluge::generative::Genre& genre, std::string_view& from);
#endif

private:
	int32_t row_;
};

/// GENRE ▸ itself. Its items alternate per row: the genre item, then the parent's submenu, of which only one is
/// relevant. It opens on the row of the genre to open on.
class GenreMenu final : public Submenu {
public:
	using Submenu::Submenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override;
#if ENABLE_SYSEX_TEST
	/// Test port: focuses `genre`'s row (its parent's, for a subgenre), as turning to it would, and sets `from` to the
	/// label focused before.
	bool focusGenre(const deluge::generative::Genre& genre, std::string_view& from);
#endif
};

#if ENABLE_SYSEX_TEST
/// Test port ("pick genre <name>"): if `current`, the menu item the sound editor shows, is GENRE ▸ or a parent's
/// submenu, focuses `genre`'s row in it as an encoder turn to it would, so a script picks a genre by name rather than
/// by counting rows. In GENRE ▸ a subgenre's row is its parent's. Returns the label now focused and sets `from` to the
/// one focused before (where the menu opened, right after a SELECT), or returns nullptr: no genre menu is open, or
/// `genre` isn't in the open one.
const char* pickGenreForTest(MenuItem* current, const deluge::generative::Genre& genre, std::string_view& from);
#endif

} // namespace deluge::gui::menu_item::generative
