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

// The Regenerate and genre-change page (spec §13.6, ADR 0008 "The menus"), a stock submenu titled REGENERATE,
// `<GENRE>?` or a LOAD's `<NAME>?`: TRANSITION (DROP, FADE or CUT), VARIANTS ▸ (a row per set of the incoming genre,
// each a stock multi-select of its variants; not for a LOAD, nor a genre with no sets), then GO, where the cursor
// starts. BACK cancels. Its state is the portable perform::RegeneratePage. The Compose screen's VARIANTS ▸ row (spec
// §14.4) opens the same per-set rows on the Current Recipe's masks.

#pragma once

#include "gui/menu_item/menu_item.h"

namespace deluge::generative {
struct Genre;
struct RecipeFile;
namespace perform {
struct RecipeName;
}
} // namespace deluge::generative

namespace deluge::gui::menu_item::generative {

/// GENRE ▸'s pick or REGENERATE in the Perform menu: opens the page over the menu, on `genre` (the genre playing for a
/// Regenerate). BACK goes back to the row it was opened from.
void openRegeneratePage(const deluge::generative::Genre& genre);

/// A LOAD while playing (spec §16.5): opens the page, with only TRANSITION, over Perform. GO loads `file`, named
/// `name`; both must outlive the page.
void openLoadPage(const deluge::generative::RecipeFile& file, const deluge::generative::perform::RecipeName& name);

/// The Compose screen's VARIANTS ▸ row, after SCALE: shown only when the Current Recipe's genre has sets.
extern MenuItem& composeVariantsMenu;

} // namespace deluge::gui::menu_item::generative
