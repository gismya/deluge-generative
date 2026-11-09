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

// The Perform menu (spec §13.6), stock look, opened by SELECT push in Perform: CANCEL CHANGE (only while a transition
// can be cancelled), GENRE ▸ (the genre playing, labelled = REGEN), REGENERATE, KEY / SCALE ▸ (the key and scale
// editor, which SCALE also opens), GLIDE and SEED + TAKES ▸. Both open with soundEditor.setup(nullptr, &menu), as the
// stock song-level menus do; the playback, pads' stock shortcuts and the gold knobs (F16) stay live underneath.

#pragma once

#include "gui/menu_item/submenu.h"

namespace deluge::gui::menu_item::generative {

/// The Perform menu.
extern Submenu performMenu;
/// The key and scale editor, KEY and SCALE (spec §13.5): it starts from the key ahead (a waiting change's, or the key
/// that plays), and each turn queues the key it shows to the next phrase line, or sets it at once while stopped.
extern Submenu& keyScaleMenu;

/// Every frame while the sound editor is open over Perform: if a stop, a start or a press elsewhere (a Y turn) moved
/// what the open Perform menu item shows, it reads its value again.
void refreshPerformMenu();

} // namespace deluge::gui::menu_item::generative
