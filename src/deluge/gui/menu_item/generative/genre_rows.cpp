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

#include "gui/menu_item/generative/genre_rows.h"
#include "generative/firmware/current_recipe.h"
#include "generative/perform/genre_menu.h"

namespace deluge::gui::menu_item::generative {

using deluge::generative::allGenres;
using deluge::generative::Genre;
using deluge::generative::firmware::currentRecipe;
namespace perform = deluge::generative::perform;

namespace {
/// The parent whose submenu is open: its genre items read their genres from it.
const Genre* openParent = nullptr;
#if ENABLE_SYSEX_TEST
/// The genre menu (GENRE ▸ or a parent's submenu) whose session began last.
GenreSubmenu* openGenreMenu = nullptr;
#endif
} // namespace

const Genre* genreAt(GenreSlot slot) {
	if (slot.inGroup) {
		return openParent != nullptr ? perform::subgenreAt(allGenres(), *openParent, slot.index) : nullptr;
	}
	const perform::GenreRow row = perform::genreRow(allGenres(), slot.index);
	return row.group ? nullptr : row.genre;
}

std::string_view genreLabel(GenreSlot slot) {
	const Genre* genre = genreAt(slot);
	if (genre == nullptr) {
		return "";
	}
	return slot.inGroup ? genre->shortName : genre->oledName;
}

const Genre& genreToOpenOn() {
	const Genre* current = currentRecipe.exists ? currentRecipe.recipe.genre : nullptr;
	return current != nullptr ? *current : deluge::generative::defaultGenre();
}

std::string_view GenreGroup::getName() const {
	const perform::GenreRow row = perform::genreRow(allGenres(), row_);
	return row.group ? row.genre->oledName : "";
}

bool GenreGroup::isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const {
	return perform::genreRow(allGenres(), row_).group;
}

void GenreGroup::beginSession(MenuItem* navigatedBackwardFrom) {
#if ENABLE_SYSEX_TEST
	openGenreMenu = this;
#endif
	openParent = perform::genreRow(allGenres(), row_).genre;
	if (navigatedBackwardFrom == nullptr) {
		// Opened afresh: on the genre to open on, if it is one of this parent's.
		const Genre& genre = genreToOpenOn();
		const perform::GenreSpot spot = perform::genreSpot(allGenres(), genre);
		if (genre.parent == openParent && spot.inGroup >= 0 && spot.inGroup < static_cast<int32_t>(items.size())) {
			navigatedBackwardFrom = items[spot.inGroup];
		}
		else if (!items.empty()) {
			navigatedBackwardFrom = items[0];
		}
	}
	Submenu::beginSession(navigatedBackwardFrom);
}

void GenreMenu::beginSession(MenuItem* navigatedBackwardFrom) {
#if ENABLE_SYSEX_TEST
	openGenreMenu = this;
#endif
	if (navigatedBackwardFrom == nullptr) {
		// Opened afresh: on the row of the genre to open on, its genre item or its parent's submenu.
		const int32_t row = perform::genreSpot(allGenres(), genreToOpenOn()).row;
		for (int32_t i = 2 * row; i < 2 * row + 2 && i < static_cast<int32_t>(items.size()); i++) {
			if (isItemRelevant(items[i])) {
				navigatedBackwardFrom = items[i];
			}
		}
	}
	Submenu::beginSession(navigatedBackwardFrom);
}

#if ENABLE_SYSEX_TEST
bool GenreSubmenu::focusGenre(const Genre& genre, std::string_view& row, std::string_view& from) {
	MenuItem* item = itemFor(genre);
	if (item == nullptr) {
		return false;
	}
	from = current_item_ != items.end() ? (*current_item_)->getName() : "";
	focusChild(item);
	row = current_item_ != items.end() ? (*current_item_)->getName() : "";
	return true;
}

MenuItem* GenreGroup::itemFor(const Genre& genre) {
	const int32_t place = perform::genreSpot(allGenres(), genre).inGroup;
	if (genre.parent != openParent || place < 0 || place >= static_cast<int32_t>(items.size())
	    || !isItemRelevant(items[place])) {
		return nullptr;
	}
	return items[place];
}

MenuItem* GenreMenu::itemFor(const Genre& genre) {
	const int32_t row = perform::genreSpot(allGenres(), genre).row;
	for (int32_t i = 2 * row; i < 2 * row + 2 && i < static_cast<int32_t>(items.size()); i++) {
		if (isItemRelevant(items[i])) {
			return items[i];
		}
	}
	return nullptr;
}

bool pickGenreForTest(MenuItem* current, const Genre& genre, std::string_view& row, std::string_view& from) {
	if (current == nullptr || current != openGenreMenu || !openGenreMenu->focusGenre(genre, row, from)) {
		return false;
	}
	openGenreMenu->updateDisplay();
	return true;
}
#endif

} // namespace deluge::gui::menu_item::generative
