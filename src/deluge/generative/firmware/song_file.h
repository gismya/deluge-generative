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

// Saving a song without the save UI (spec §17.7, §19 F13): Capture saves each song it builds through the normal save
// path for the current song, with no browser, QWERTY or overwrite prompt.

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>

namespace deluge::generative::firmware {

/// The longest song name saveCurrentSong() takes, with room for a clash's ` 9999`.
constexpr int32_t kMaxSongFileName = 48;

/// Saves the current song in SONGS as `name`, as stock's save does (SaveSongUI::performSave()), never over a file: a
/// name already on the card takes the firmware's next free variation (`NAME 2`, Browser's default-name rule). On
/// success the song has the name it was saved as and SONGS as its folder, and `saved` holds the name. On failure the
/// file it began is deleted (stock leaves a truncated one) and the song is as it was. Card work: task context, not
/// in a card routine.
Error saveCurrentSong(const char* name, char (&saved)[kMaxSongFileName + 1]);

} // namespace deluge::generative::firmware
