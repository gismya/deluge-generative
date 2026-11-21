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

// The Compose screen (spec §14.4): a stock-style submenu that edits the Current Recipe before BUILD. Its title is the
// Recipe's genre, tempo, key and scale, side-scrolled when it is wider than the screen. Rows: FROM ▸ (GENRE ▸),
// REGENERATE, SEED + TAKES ▸, TEMPO, SWING, KEY, SCALE, the four macros, SOUNDS ▸, LENGTH and BUILD. KEY and SCALE
// edit in place (the value blinks); TEMPO, SWING and LENGTH open the stock big-number editor, the macros the stock
// continuous one. With no Current Recipe only FROM shows.
//
// Opened with soundEditor.setup(nullptr, &composeScreen), over the session or arranger view: by COMPOSE FROM's rows
// (the Generative menu's COMPOSE), and by the test port's "compose screen" line, which can open it with no Recipe.

#pragma once

#include "generative/core/recipe_file.h"
#include "generative/perform/recipe_name.h"
#include "gui/menu_item/submenu.h"

namespace deluge::gui::menu_item::generative {

/// The Compose screen.
extern Submenu& composeScreen;
/// COMPOSE FROM (spec §14.1): CURRENT RECIPE, GENRE ▸ and FILE..., each showing the Compose screen in its place.
/// Opened with soundEditor.setup(nullptr, &composeFromMenu).
extern Submenu& composeFromMenu;

/// Opens the Compose screen over the current UI. False, with nothing opened, while Perform is the root UI.
bool openComposeScreen();

/// Whether the sound editor shows the Compose screen or a menu under it: SAVE there saves the Current Recipe (spec
/// §14.4, §16.4).
bool composeScreenShowing();

/// DISCARD SONG?'s OK after BUILD (spec §14.5): starts the Compose build of the Current Recipe under COMPOSING.
void buildComposeRecipe();

/// REGENERATE?'s OK (spec §14.4): rolls the Current Recipe again as a modal job over the screen.
void regenerateComposeRecipe();
/// A Recipe file chosen in the load browser (spec §16.5): it replaces the Current Recipe as a modal job, ready for
/// BUILD, and the Compose screen shows it (opened in COMPOSE FROM's place, or back up from a menu under it). BUSY while
/// the runner is busy.
void loadComposeRecipe(const deluge::generative::RecipeFile& file, const deluge::generative::perform::RecipeName& name);

} // namespace deluge::gui::menu_item::generative
