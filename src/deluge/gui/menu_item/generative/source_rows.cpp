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
#include "gui/menu_item/generative/source_rows.h"
#include "definitions.h"
#include "generative/firmware/current_recipe.h"
#include "gui/ui/load/load_recipe_ui.h"
#include "io/midi/test_port.h"

namespace deluge::gui::menu_item::generative {

using deluge::generative::firmware::currentRecipe;

bool CurrentRecipeRow::isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const {
	return currentRecipe.exists;
}

MenuItem* CurrentRecipeRow::selectButtonPress() {
	picked_();
	return NO_NAVIGATION;
}

MenuItem* FileRow::selectButtonPress() {
	// SELECT reaches a menu item only outside the card routine (SoundEditor::buttonAction()).
	loadRecipeUI.open(false);
	return NO_NAVIGATION;
}

void SourceMenu::beginSession(MenuItem* navigatedBackwardFrom) {
	if (navigatedBackwardFrom == nullptr) {
		TEST_EVENT("gen_from_menu %s current=%d", which_, current_ != nullptr && isItemRelevant(current_) ? 1 : 0);
	}
	Submenu::beginSession(navigatedBackwardFrom);
}

} // namespace deluge::gui::menu_item::generative
