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

// Stand-in for H10-H12 (docs/research/spec-review-technical.md on research/spec-review): builds a song shaped like
// Compose output (§15) or a long Capture through the model API, to measure its memory, build time and save time.
// Compiled in only with ENABLE_SYSEX_TEST.

#pragma once

#if ENABLE_SYSEX_TEST

#include <cstdint>

class Song;

namespace deluge::generative::compose_shape {

struct Shape {
	int32_t occurrences;   ///< Section occurrences; each gets one clip per part
	int32_t bars;          ///< the whole song
	int32_t yieldBudgetUs; ///< 0: never yield (H10); else yield to the scheduler once a run reaches this (H11)
	bool intoCurrentSong;  ///< H12: build into currentSong (stopped), occurrences past the 23rd arrangement-only
};

struct Result {
	int32_t error;
	int32_t clips;
	int32_t notes;
	int32_t nodes;
	int32_t barsBuilt;
	uint32_t buildUs;
	uint32_t longestRunUs; ///< the longest stretch without a yield
	uint32_t yields;
	bool cancelled;
	uint32_t cancelLatencyUs; ///< cancel flag set -> the build noticed it
};

/// Builds the shape. For a private song (H10, H11) it returns the Song, for the caller to measure and delete with
/// deleteSong(); with intoCurrentSong it returns currentSong.
Song* build(const Shape& shape, Result* result);
void deleteSong(Song* song);

/// Sets the cancel flag. Safe from the MIDI routine while a build yields.
void cancel();

} // namespace deluge::generative::compose_shape

#endif
