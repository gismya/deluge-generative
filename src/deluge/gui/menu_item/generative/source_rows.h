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
// The source rows of the Generative menu's lists (spec §14.1): PERFORM FROM and COMPOSE FROM have CURRENT RECIPE,
// GENRE ▸ and FILE...; the Compose screen's FROM has GENRE ▸ and FILE.... GENRE ▸ is genre_rows.h's. Each list is a
// SourceMenu.

#pragma once

#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/submenu.h"
#include <initializer_list>

namespace deluge::gui::menu_item::generative {

/// Whether the sound editor is the current UI and shows `root` or a menu under it.
bool soundEditorShows(const MenuItem& root);

/// CURRENT RECIPE: shown once a Current Recipe exists; a push calls `picked`.
class CurrentRecipeRow final : public MenuItem {
public:
	CurrentRecipeRow(l10n::String name, void (*picked)()) : MenuItem(name), picked_(picked) {}
	[[nodiscard]] bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override;

private:
	void (*picked_)();
};

/// FILE...: the Recipe load browser (spec §16.5). What a chosen file does is the browser's, by the menu under it
/// (load_recipe_ui.cpp: PERFORM FROM asks DISCARD SONG?, the Compose lists load it and show the Compose screen).
class FileRow final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override;
};

/// A source list, its first item `current` when it lists CURRENT RECIPE. Test builds report each time it shows afresh
/// (opened, or focus back from a UI over it): gen_from_menu <which> current=0|1, the second saying whether CURRENT
/// RECIPE shows.
class SourceMenu : public Submenu {
public:
	SourceMenu(l10n::String name, const char* which, CurrentRecipeRow* current, std::initializer_list<MenuItem*> items)
	    : Submenu(name, items), which_(which), current_(current) {}
	void beginSession(MenuItem* navigatedBackwardFrom) override;

private:
	const char* which_;
	CurrentRecipeRow* current_; ///< nullptr: the list has no CURRENT RECIPE
};

} // namespace deluge::gui::menu_item::generative
