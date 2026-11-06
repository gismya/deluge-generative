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
	/// Bytes taken, by what took them: [0] fast heap, [1] SDRAM and the sample region together.
	struct Bytes {
		uint32_t outputs[2];    ///< the 6 Outputs, with their drums and backed-up ParamManagers
		uint32_t kitClips[2];   ///< kit clip objects and setup: a row per lane, each row's ParamManager
		uint32_t synthClips[2]; ///< synth clip objects and setup, with their one note row
		uint32_t notes[2];
		uint32_t nodes[2];    ///< drift automation nodes
		uint32_t arranger[2]; ///< ClipInstances
	} bytes;
};

/// Builds the shape. For a private song (H10, H11) it returns the Song, for the caller to measure and delete with
/// deleteSong(); with intoCurrentSong it returns currentSong.
Song* build(const Shape& shape, Result* result);
void deleteSong(Song* song);

/// Sets the cancel flag. Safe from the MIDI routine while a build yields.
void cancel();

} // namespace deluge::generative::compose_shape

#endif
