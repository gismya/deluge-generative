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

// The rows of GENRE ▸ (spec §14.1, §13.6), in the Generative menu and the Perform menu: a genre with no parent is a
// row of its own, and a parent is one row, where its first subgenre is, that opens a submenu of its subgenres by their
// short names. The menu opens on a genre's row, and in its parent's submenu on the genre. Both menus read their rows
// from here, so they group alike.

#pragma once

#include "generative/core/genre.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// A row of GENRE ▸.
struct GenreRow {
	const Genre* genre = nullptr; ///< The row's genre, or the parent it stands for. nullptr: past the last row.
	bool group = false;           ///< A parent's row: it opens the submenu of its subgenres.
};

/// Where a genre sits in GENRE ▸.
struct GenreSpot {
	int32_t row = 0;
	int32_t inGroup = -1; ///< Its place in its parent's submenu, or -1 for a genre with no parent.
};

/// Row `row` of GENRE ▸ over `genres` (allGenres() in the firmware), in their order.
GenreRow genreRow(std::span<const Genre* const> genres, int32_t row);
/// The `index`th subgenre of `parent` in `genres`, or nullptr.
const Genre* subgenreAt(std::span<const Genre* const> genres, const Genre& parent, int32_t index);
/// Where `genre` sits: GENRE ▸ opens on that row, and its parent's submenu on that place.
GenreSpot genreSpot(std::span<const Genre* const> genres, const Genre& genre);

} // namespace deluge::generative::perform
