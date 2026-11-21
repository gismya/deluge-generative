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

#include "generative/compose/compose_screen.h"
#include "generative/core/genre.h"
#include "generative/perform/queue_text.h"
#include "generative/perform/text.h"
#include <cstring>

namespace deluge::generative::compose {

using perform::Text;

namespace {

/// Indexed by Scale.
constexpr const char* kFullNames[kNumScales] = {
    "MAJOR",   "MINOR",         "DORIAN",         "PHRYGIAN",        "LYDIAN", "MIXOLYDIAN",
    "LOCRIAN", "MELODIC MINOR", "HARMONIC MINOR", "HUNGARIAN MINOR", "MARVA",  "ARABIAN",
};

} // namespace

void composeTitle(const Recipe& recipe, std::span<char> out) {
	Text(out)
	    .add(recipe.genre != nullptr ? recipe.genre->oledName : "")
	    .add(" ")
	    .add(tempoBpm(recipe.tempo))
	    .add(" ")
	    .add(perform::keyName(recipe.keyRoot))
	    .add(" ")
	    .add(scaleFullName(recipe.scale));
}

const char* scaleFullName(Scale scale) {
	const auto index = static_cast<int32_t>(scale);
	return index < kNumScales ? kFullNames[index] : "?";
}

const char* scaleRowName(Scale scale, int32_t fitChars) {
	const char* full = scaleFullName(scale);
	const auto index = static_cast<int32_t>(scale);
	if (index >= kNumScales || static_cast<int32_t>(std::strlen(full)) <= fitChars) {
		return full;
	}
	return perform::scaleName(scale); // the short names Perform's queue shows: at most 10 characters
}

void lengthText(int32_t seconds, std::span<char> out) {
	const int32_t s = seconds % 60;
	Text(out).add(seconds / 60).add(":").add(s < 10 ? "0" : "").add(s);
}

void pickName(const SoundPath& path, std::span<char> out) {
	if (path.empty()) {
		Text(out).add("NONE");
		return;
	}
	const char* name = std::strrchr(path.c_str(), '/');
	name = name != nullptr ? name + 1 : path.c_str();
	const char* dot = std::strrchr(name, '.');
	const auto length = static_cast<int32_t>(dot != nullptr ? dot - name : std::strlen(name));
	Text(out).add(name, length);
}

void kitRowName(const PartSpec& part, std::span<char> out) {
	Text(out).add(part.name).add(": ").add(static_cast<int32_t>(part.lanes.size())).add(" LANES");
}

void partsText(int32_t parts, std::span<char> out) {
	Text(out).add(parts).add(parts == 1 ? " PART" : " PARTS");
}

} // namespace deluge::generative::compose
