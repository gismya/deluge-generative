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

// PERFORM FROM (spec §14.1): the full-screen stock submenu PERFORM opens, with CURRENT RECIPE (once one exists),
// GENRE ▸ and FILE.... Each asks DISCARD SONG? (spec §14.3), FILE... once the file is chosen.

#pragma once

#include "generative/core/recipe_file.h"
#include "generative/perform/recipe_name.h"
#include "gui/menu_item/generative/genre_rows.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/submenu.h"

namespace deluge::gui::menu_item::generative {

/// A genre of GENRE ▸ (genre_rows.h): picking it rolls a fresh Recipe from the genre once DISCARD SONG? says OK.
/// Slots with no genre (a parent's row, or past the last) hide themselves.
class GenreItem final : public MenuItem {
public:
	explicit GenreItem(GenreSlot slot) : slot_(slot) {}
	[[nodiscard]] std::string_view getName() const override { return genreLabel(slot_); }
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override;

private:
	GenreSlot slot_;
};

/// The PERFORM FROM menu, opened with soundEditor.setup(nullptr, &performFromMenu).
extern Submenu& performFromMenu;

/// Whether the sound editor shows PERFORM FROM or a menu under it: a Recipe file chosen in the load browser over it
/// enters Perform (performFromFile()).
bool performFromShowing();

/// FILE...'s chosen file (spec §14.1, §14.3): asks DISCARD SONG?, whose OK enters Perform on the file. `file` and
/// `name` must outlive the entry (the load browser's chosen file).
void performFromFile(const deluge::generative::RecipeFile& file, const deluge::generative::perform::RecipeName& name);

} // namespace deluge::gui::menu_item::generative
