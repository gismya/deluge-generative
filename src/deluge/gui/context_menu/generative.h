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

// Generative mode's entry menus (spec §14.1, §14.3), both stock context menus:
//   GENERATIVE, opened by SHIFT+SONG in session view over the session view: PERFORM (COMPOSE comes in M3);
//   DISCARD SONG?, asked every time before Generative mode replaces the song: OK.

#pragma once

#include "gui/context_menu/context_menu.h"

namespace deluge::generative {
struct Genre;
}

namespace deluge::gui::context_menu {

class Generative final : public ContextMenu {
public:
	Generative() = default;
	bool canSeeViewUnderneath() override { return true; }
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;
};

class DiscardSong final : public ContextMenu {
public:
	DiscardSong() = default;
	/// The genre whose fresh Recipe Perform plays once the player says OK.
	void setGenre(const generative::Genre* genre) { genre_ = genre; }
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;

private:
	const generative::Genre* genre_ = nullptr;
};

extern Generative generativeMenu;
extern DiscardSong discardSong;

} // namespace deluge::gui::context_menu
