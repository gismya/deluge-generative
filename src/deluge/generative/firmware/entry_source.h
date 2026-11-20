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

// Where Perform entry's Recipe comes from (spec §14.1): PERFORM FROM's three rows.

#pragma once

namespace deluge::generative {
struct Genre;
struct RecipeFile;
namespace perform {
struct RecipeName;
}
} // namespace deluge::generative

namespace deluge::generative::firmware {

/// Where Perform entry's Recipe comes from (spec §14.1): a genre's fresh roll, a Recipe file, or the Current Recipe as
/// it is. A file and its name must outlive the entry job (the load browser keeps its chosen file).
struct EntrySource {
	const Genre* genre = nullptr;
	const RecipeFile* file = nullptr;
	const perform::RecipeName* name = nullptr;

	static EntrySource current() { return {}; }
	static EntrySource of(const Genre& genre) { return {.genre = &genre}; }
	static EntrySource of(const RecipeFile& file, const perform::RecipeName& name) {
		return {.file = &file, .name = &name};
	}
};

} // namespace deluge::generative::firmware
