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

// Recipe file names (spec §16.3): the default name, the letter variation SAVE suggests, and the name the Current
// Recipe remembers. Portable: the firmware lists RECIPES/ into a NameFamily and asks here.

#pragma once

#include <cstdint>
#include <string_view>

namespace deluge::generative {
struct Genre;
}

namespace deluge::generative::perform {

/// The longest Recipe name the Current Recipe remembers. A longer one, typed in the save UI, is saved but not
/// remembered: the next save suggests the default name again.
constexpr int32_t kMaxRecipeNameChars = 96;

/// A Recipe file's name, without its folder or `.XML`: a stem, and at most one letter after it, its letter variation
/// (stock's SONG001 → SONG001A, spec §16.3). Zero-filled is empty, so it can sit in `.sdram_bss`.
struct RecipeName {
	char text[kMaxRecipeNameChars + 1] = {};
	/// The stem's length: the letters SAVE suggests go after it. All of `text`, or all but its letter.
	uint8_t stem = 0;

	[[nodiscard]] bool empty() const { return text[0] == 0; }
	void clear() {
		text[0] = 0;
		stem = 0;
	}
	/// Sets the name, its stem `stem` characters long (the whole name when negative). False, and empty, if it doesn't
	/// fit or is empty.
	bool set(std::string_view name, int32_t stem = -1);
	[[nodiscard]] std::string_view stemText() const { return {text, stem}; }
};

/// A never-saved Recipe's name: `<GENRE> <seed code>` (`ACID TECHNO 1A2B3C4`), all of it the stem.
void defaultRecipeName(const Genre& genre, uint32_t seed, RecipeName& out);

/// What a folder holds of one stem's family: the stem itself and its letter variations.
struct NameFamily {
	bool stemTaken = false;
	uint32_t takenLetters = 0; ///< Bit 0 is `<stem>A`, through bit 25 for `<stem>Z`.

	/// Counts one of the folder's files, `<name>.XML` or a name alone. Case doesn't matter, as on the card.
	void add(std::string_view fileName, std::string_view stem);
};

/// The name a save suggests (spec §16.3), from `base`: the Current Recipe's remembered name, or the default name when
/// it has none. The default name itself is offered while `offerStem` and it's free; otherwise the first free letter
/// variation after `base`'s own letter. False if none is free (a quick save's NO FREE NAME).
bool suggestRecipeName(const RecipeName& base, bool offerStem, const NameFamily& family, RecipeName& out);

/// After a save as `saved`, the name the Current Recipe remembers (spec §16.3): `suggestion` with its stem when the
/// player saved it as offered (in any case), else `saved` as a stem of its own. False, and `remembered` empty, if
/// `saved` is too long to remember.
bool adoptSavedName(const RecipeName& suggestion, std::string_view saved, RecipeName& remembered);

} // namespace deluge::generative::perform
