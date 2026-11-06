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

// PERFORM FROM (spec §14.1): the full-screen stock submenu PERFORM opens. M1 has only GENRE ▸, the genres;
// CURRENT RECIPE and FILE... arrive in M3 (§23.9).

#pragma once

#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/submenu.h"

namespace deluge::generative {
struct Genre;
}

namespace deluge::gui::menu_item::generative {

/// A genre row, for allGenres()[index]: picking it rolls a fresh Recipe from the genre once DISCARD SONG? says OK.
/// Rows past the last built-in genre hide themselves.
class GenreItem final : public MenuItem {
public:
	explicit GenreItem(int32_t index) : index_(index) {}
	[[nodiscard]] std::string_view getName() const override;
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override;

private:
	[[nodiscard]] const deluge::generative::Genre* genre() const;
	int32_t index_;
};

/// The PERFORM FROM menu, opened with soundEditor.setup(nullptr, &performFromMenu).
extern Submenu performFromMenu;

} // namespace deluge::gui::menu_item::generative
