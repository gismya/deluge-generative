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

// The lane list (spec §13.5): KIT on a focused Drums or FX part opens it over Perform, stock look, titled with the
// part's name. RE-ROLL ALL re-rolls the whole part, as NEW SOUND does; then a row per lane the part plays, with its
// pick's name (`KICK 909 Kick`). SELECT on a lane opens the lane browser in its pick's folder; on an FX lane that takes
// a preset (spec §10.5), a SAMPLE / SYNTH PRESET choice first. BACK goes back a level, and closes it from the top.

#pragma once

#include "gui/menu_item/submenu.h"
#include <cstdint>

namespace deluge::gui::menu_item::generative {

/// The lane list. Set its part with setLaneListPart() before opening it.
extern Submenu& laneListMenu;

/// The Drums or FX part the lane list opens on.
void setLaneListPart(int32_t part);

} // namespace deluge::gui::menu_item::generative
