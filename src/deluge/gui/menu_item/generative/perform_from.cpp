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

const deluge::generative::Genre* GenreItem::genre() const {
	auto genres = deluge::generative::allGenres();
	return index_ < static_cast<int32_t>(genres.size()) ? genres[index_] : nullptr;
}

std::string_view GenreItem::getName() const {
	const auto* g = genre();
	return g ? g->oledName : "";
}

bool GenreItem::isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const {
	return genre() != nullptr;
}

MenuItem* GenreItem::selectButtonPress() {
	context_menu::discardSong.setGenre(genre());
	if (context_menu::discardSong.setupAndCheckAvailability()) {
		openUI(&context_menu::discardSong);
	}
	return NO_NAVIGATION;
}

namespace {
// One row per built-in genre, in allGenres() order.
static_assert(deluge::generative::kMaxGenres == 5, "one GenreItem per genre slot below");
PLACE_SDRAM_BSS GenreItem genreItems[deluge::generative::kMaxGenres] = {GenreItem{0}, GenreItem{1}, GenreItem{2},
                                                                        GenreItem{3}, GenreItem{4}};
PLACE_SDRAM_BSS Submenu genreMenu{
    l10n::String::STRING_FOR_GENRE,
    {&genreItems[0], &genreItems[1], &genreItems[2], &genreItems[3], &genreItems[4]},
};
} // namespace

PLACE_SDRAM_BSS Submenu performFromMenu{l10n::String::STRING_FOR_PERFORM_FROM, {&genreMenu}};

} // namespace deluge::gui::menu_item::generative
