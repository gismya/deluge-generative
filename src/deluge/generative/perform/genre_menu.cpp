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

#include "generative/perform/genre_menu.h"

namespace deluge::generative::perform {

namespace {

/// What stands for `genre` at the top level: its parent, or itself.
const Genre* topOf(const Genre* genre) {
	return genre->parent != nullptr ? genre->parent : genre;
}

/// Whether `genres` has the same top level before index `i`: then `genres[i]` adds no row.
bool seenBefore(std::span<const Genre* const> genres, size_t i) {
	for (size_t j = 0; j < i; j++) {
		if (topOf(genres[j]) == topOf(genres[i])) {
			return true;
		}
	}
	return false;
}

} // namespace

GenreRow genreRow(std::span<const Genre* const> genres, int32_t row) {
	int32_t at = 0;
	for (size_t i = 0; i < genres.size() && row >= 0; i++) {
		if (seenBefore(genres, i)) {
			continue;
		}
		if (at == row) {
			return {topOf(genres[i]), genres[i]->parent != nullptr};
		}
		at++;
	}
	return {};
}

const Genre* subgenreAt(std::span<const Genre* const> genres, const Genre& parent, int32_t index) {
	int32_t at = 0;
	for (const Genre* genre : genres) {
		if (genre->parent == &parent && at++ == index) {
			return genre;
		}
	}
	return nullptr;
}

GenreSpot genreSpot(std::span<const Genre* const> genres, const Genre& genre) {
	GenreSpot spot;
	for (int32_t row = 0;; row++) {
		const GenreRow at = genreRow(genres, row);
		if (at.genre == nullptr) {
			return {}; // not in the list: the first row
		}
		if (at.genre == topOf(&genre)) {
			spot.row = row;
			break;
		}
	}
	if (genre.parent != nullptr) {
		for (int32_t i = 0; const Genre* sibling = subgenreAt(genres, *genre.parent, i); i++) {
			if (sibling == &genre) {
				spot.inGroup = i;
			}
		}
	}
	return spot;
}

} // namespace deluge::generative::perform
