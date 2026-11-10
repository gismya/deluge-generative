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

// Perform's grid pages (spec §13.5): SONG, CLIP and KEYBOARD, and which main pads SHIFT turns into the sound editor's
// shortcuts (spec §13.7).

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

enum class GridPage : uint8_t { Song, Clip, Keyboard };

/// Whether SHIFT + main pad (`x`, `row`), its row counted from the top from 0, opens the sound editor at the printed
/// param on `page`. Every main pad does, but those whose SHIFT means something already: the KEYBOARD page's FILL pads
/// (a fresh pick), and the CLIP page's NEW SOUND (the previous sound) and SOUND EDITOR (its root menu either way).
bool shiftOpensSoundEditor(GridPage page, int32_t x, int32_t row);

} // namespace deluge::generative::perform
