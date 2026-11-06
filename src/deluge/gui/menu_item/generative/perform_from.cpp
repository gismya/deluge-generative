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

#include "gui/menu_item/generative/perform_from.h"
#include "definitions.h"
#include "generative/core/genre.h"
#include "gui/context_menu/generative.h"
#include "gui/ui/ui.h"

namespace deluge::gui::menu_item::generative {

std::string_view GenreItem::getName() const {
	return genre_->oledName;
}

MenuItem* GenreItem::selectButtonPress() {
	context_menu::discardSong.setGenre(genre_);
	if (context_menu::discardSong.setupAndCheckAvailability()) {
		openUI(&context_menu::discardSong);
	}
	return NO_NAVIGATION;
}

namespace {
// One row per built-in genre, in allGenres() order.
PLACE_SDRAM_BSS GenreItem technoItem{deluge::generative::genres::techno};
PLACE_SDRAM_BSS Submenu genreMenu{l10n::String::STRING_FOR_GENRE, {&technoItem}};
} // namespace

PLACE_SDRAM_BSS Submenu performFromMenu{l10n::String::STRING_FOR_PERFORM_FROM, {&genreMenu}};

} // namespace deluge::gui::menu_item::generative
