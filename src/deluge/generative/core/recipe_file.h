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

// Recipe files (spec §16.1–§16.3): the Current Recipe, its sound picks, seed and takes, as XML. The core writes the
// file through a writer the firmware backs with the stock XMLSerializer, and reads it back from text, so the harness
// tests both ends. No allocation.
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <generativeRecipe
//   	genre="techno-acid"
//   	genreVersion="2"
//   	tempo="13250"
//   	swing="0"
//   	swingInterval="0"
//   	key="9"
//   	scale="minor"
//   	seed="3735928559">
//   	<macros
//   		energy="1600"
//   		density="1600"
//   		complexity="1600"
//   		motion="1600" />
//   	<part
//   		name="acid"
//   		take="2"
//   		octave="0"
//   		sound="SYNTHS/047 Basic Dirty Bass.XML" />
//   	<lane
//   		name="kick"
//   		sound="SAMPLES/DRUMS/Kick/909 Kick.wav" />
//   </generativeRecipe>
//
// Tempo is in hundredths of a BPM, macros in their full-resolution units (kMacroUnitsPerPoint a point), a part's octave
// shift in octaves (clamped on read to the part's octaveRange()). Parts go by
// their name in the genre table, lanes by their palette file name (lanePaletteName()). A part or lane with no pick
// leaves out its sound. Nothing else is stored: no steering, sound-control homes, freezes, mutes, Arc position or
// LENGTH (spec §16.1).

#pragma once

#include "generative/core/recipe.h"
#include <cstdint>
#include <string_view>

namespace deluge::generative {

/// What a Recipe file holds (spec §16.1).
struct RecipeFile {
	Recipe recipe;
	SoundPicks sounds;
	uint32_t seed = 0;
	Takes takes;
	/// The genre version the file names (spec §16.2): read, never written (a save writes the genre's own).
	uint16_t genreVersion = 0;
};

/// The root tag, and the text a whole file ends with (the firmware's write check).
constexpr const char* kRecipeFileTag = "generativeRecipe";
constexpr const char* kRecipeFileEnd = "</generativeRecipe>\n";

/// Where writeRecipeFile() writes. The firmware's forwards each call to the stock Serializer member of the same shape
/// (writeOpeningTagBeginning(), writeAttribute(), writeOpeningTagEnd(), closeTag(), writeClosingTag()). Attribute
/// values come already entity-encoded.
class RecipeFileWriter {
public:
	virtual ~RecipeFileWriter() = default;
	/// `<tag`, on a line of its own.
	virtual void openTag(const char* tag) = 0;
	/// ` name="value"`, on a line of its own.
	virtual void attribute(const char* name, const char* value) = 0;
	/// `>`: the tag's children follow, then closeTag().
	virtual void endAttributes() = 0;
	/// ` />`: the tag has no children.
	virtual void closeEmptyTag() = 0;
	/// `</tag>`.
	virtual void closeTag(const char* tag) = 0;
};

/// Writes `file` as a Recipe file. `file.recipe.genre` must be set.
void writeRecipeFile(RecipeFileWriter& writer, const RecipeFile& file);

enum class RecipeFileRead : uint8_t {
	Ok,
	/// The file names another version of its genre (spec §16.2): it loaded, and the OLED says so (`Older ACID
	/// TECHNO`).
	OtherVersion,
	/// The file names a genre this firmware doesn't have (spec §16.2): nothing loaded.
	UnknownGenre,
	/// Not a Recipe file, cut short, or a value that isn't one: nothing loaded.
	Unreadable,
};

/// Reads a Recipe file's text into `out`. A value the file leaves out takes the genre's default (macros, swing) or
/// the Recipe's; one out of range is clamped; parts, lanes, elements and attributes it doesn't know are skipped, so a
/// newer file still loads. A sound path too long for a pick, or with an entity it can't decode, leaves the pick
/// empty. On UnknownGenre and Unreadable, `out` holds nothing to use.
RecipeFileRead parseRecipeFile(std::string_view text, RecipeFile& out);

/// A scale's name in Recipe files ("minor", "hungarianMinor"): stable, never reused.
const char* scaleFileName(Scale scale);

} // namespace deluge::generative
