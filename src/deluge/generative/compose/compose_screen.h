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

// The Compose screen's text (spec §14.4), portable so the harness checks it: the title, and the values the SCALE,
// LENGTH and SOUNDS rows show. The menu (gui/menu_item/generative/compose_screen.*) draws them.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <cstdint>
#include <span>

namespace deluge::generative {
struct PartSpec;
}

namespace deluge::generative::compose {

/// The longest title: a 14-character genre name, a 3-digit tempo, a key and the longest scale name.
constexpr int32_t kTitleChars = 40;
constexpr int32_t kLengthChars = 5; ///< `20:00`
/// A pick's shown name: the file name without its folder or extension, cut at this.
constexpr int32_t kPickChars = 40;
/// A SOUNDS row's own text: `DRUMS: 11 LANES`, `6 PARTS`.
constexpr int32_t kRowChars = 24;

/// The tempo the screen shows, in whole BPM, from the Recipe's hundredths.
constexpr int32_t tempoBpm(int32_t tempo) {
	return (tempo + 50) / 100;
}

/// The title: genre, tempo, key and scale, `PSYTRANCE 146 A# HUNGARIAN MINOR`.
void composeTitle(const Recipe& recipe, std::span<char> out);

/// A scale's full name, `HUNGARIAN MINOR`.
const char* scaleFullName(Scale scale);
/// SCALE's value: the full name when it has at most `fitChars` characters, else the short one (`HUNG MIN`).
const char* scaleRowName(Scale scale, int32_t fitChars);

/// LENGTH's value, `m:ss`.
void lengthText(int32_t seconds, std::span<char> out);

/// A sound pick's name: its file name without folder or extension (`017 ACID LINE`), `NONE` for an empty pick.
void pickName(const SoundPath& path, std::span<char> out);

/// A Kit part's SOUNDS row: `DRUMS: 8 LANES`.
void kitRowName(const PartSpec& part, std::span<char> out);
/// SOUNDS' value: `5 PARTS`.
void partsText(int32_t parts, std::span<char> out);

} // namespace deluge::generative::compose
