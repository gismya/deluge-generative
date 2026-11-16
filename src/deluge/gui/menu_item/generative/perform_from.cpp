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

bool GenreItem::isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const {
	return genreAt(slot_) != nullptr;
}

MenuItem* GenreItem::selectButtonPress() {
	context_menu::discardSong.setGenre(genreAt(slot_));
	if (context_menu::discardSong.setupAndCheckAvailability()) {
		openUI(&context_menu::discardSong);
	}
	return NO_NAVIGATION;
}

namespace {
// GENRE ▸ (genre_rows.h): per row a genre item and a parent's submenu, and the genre items the open parent's submenu
// shows. As many of each as there can be genres.
static_assert(deluge::generative::kMaxGenres == 8, "one item per genre slot below");
PLACE_SDRAM_BSS GenreItem subgenreItems[deluge::generative::kMaxGenres] = {
    GenreItem{{0, true}}, GenreItem{{1, true}}, GenreItem{{2, true}}, GenreItem{{3, true}},
    GenreItem{{4, true}}, GenreItem{{5, true}}, GenreItem{{6, true}}, GenreItem{{7, true}},
};
MenuItem* subgenreList[] = {&subgenreItems[0], &subgenreItems[1], &subgenreItems[2], &subgenreItems[3],
                            &subgenreItems[4], &subgenreItems[5], &subgenreItems[6], &subgenreItems[7]};
PLACE_SDRAM_BSS GenreItem rowItems[deluge::generative::kMaxGenres] = {
    GenreItem{{0}}, GenreItem{{1}}, GenreItem{{2}}, GenreItem{{3}},
    GenreItem{{4}}, GenreItem{{5}}, GenreItem{{6}}, GenreItem{{7}},
};
PLACE_SDRAM_BSS GenreGroup groups[deluge::generative::kMaxGenres] = {
    {0, subgenreList}, {1, subgenreList}, {2, subgenreList}, {3, subgenreList},
    {4, subgenreList}, {5, subgenreList}, {6, subgenreList}, {7, subgenreList},
};
PLACE_SDRAM_BSS GenreMenu genreMenu{
    l10n::String::STRING_FOR_GENRE,
    {&rowItems[0], &groups[0], &rowItems[1], &groups[1], &rowItems[2], &groups[2], &rowItems[3], &groups[3],
     &rowItems[4], &groups[4], &rowItems[5], &groups[5], &rowItems[6], &groups[6], &rowItems[7], &groups[7]},
};
} // namespace

PLACE_SDRAM_BSS Submenu performFromMenu{l10n::String::STRING_FOR_PERFORM_FROM, {&genreMenu}};

} // namespace deluge::gui::menu_item::generative
