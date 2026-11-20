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
#include "gui/menu_item/generative/source_rows.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"

namespace deluge::gui::menu_item::generative {

using deluge::generative::firmware::EntrySource;

namespace {

/// DISCARD SONG? over PERFORM FROM, whose OK enters Perform on `source` (spec §14.3).
void askDiscardSong(const EntrySource& source) {
	context_menu::discardSong.setPerform(source);
	if (context_menu::discardSong.setupAndCheckAvailability()) {
		openUI(&context_menu::discardSong);
	}
}

} // namespace

bool GenreItem::isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const {
	return genreAt(slot_) != nullptr;
}

MenuItem* GenreItem::selectButtonPress() {
	if (const deluge::generative::Genre* genre = genreOrError(slot_)) {
		askDiscardSong(EntrySource::of(*genre));
	}
	return NO_NAVIGATION;
}

namespace {
PLACE_SDRAM_BSS GenreMenuItems<GenreItem> genreItems{l10n::String::STRING_FOR_GENRE};
PLACE_SDRAM_BSS CurrentRecipeRow currentRecipeRow{l10n::String::STRING_FOR_CURRENT_RECIPE,
                                                  [] { askDiscardSong(EntrySource::current()); }};
PLACE_SDRAM_BSS FileRow fileRow{l10n::String::STRING_FOR_FILE_DOTS};
PLACE_SDRAM_BSS SourceMenu performFromItem{l10n::String::STRING_FOR_PERFORM_FROM,
                                           "perform",
                                           &currentRecipeRow,
                                           {&currentRecipeRow, &genreItems.menu(), &fileRow}};
} // namespace

Submenu& performFromMenu = performFromItem;

bool performFromShowing() {
	return soundEditorShows(performFromMenu);
}

void performFromFile(const deluge::generative::RecipeFile& file, const deluge::generative::perform::RecipeName& name) {
	askDiscardSong(EntrySource::of(file, name));
}

} // namespace deluge::gui::menu_item::generative
