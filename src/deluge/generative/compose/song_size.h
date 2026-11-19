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

// How big a Compose song gets and how far it runs (spec §14.5, §15.9), worked out before the build from the Recipe and
// LENGTH: the size estimate BUILD checks against free memory, and the bars the COMPOSING progress bar counts to.
// Portable, so the harness checks the arithmetic.

#pragma once

#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::compose {

/// LENGTH on the Compose screen (spec §14.4): 1:00 to 20:00 in 30 s steps, 4:00 to start.
constexpr int32_t kMinLengthSeconds = 60;
constexpr int32_t kMaxLengthSeconds = 20 * 60;
constexpr int32_t kLengthStepSeconds = 30;
constexpr int32_t kDefaultLengthSeconds = 4 * 60;

/// What a Compose song costs (spec §15.9, §20). (tuning): checked against real builds, H19.
struct SongCosts {
	/// The song itself with nothing in it, and what a build needs beside it: the Generator block and the Compose
	/// driver's buffers.
	int32_t song = 370 * 1024;
	int32_t kitRow = 6 * 1024;     ///< Each Kit row's clip setup: mostly its param copy.
	int32_t synthClip = 11 * 1024; ///< Each synth clip's setup.
	int32_t note = 30;
	/// A node on each param a control moves: drift's bound is one a bar on each automation lane (spec §15.6).
	int32_t node = 24;
};
inline constexpr SongCosts kSongCosts{};

/// The most bars a song for `recipe` with a target of `targetSteps` can run to (spec §15.1): the target, rounded up to
/// a phrase, then the Groove playing there runs its longest at the Recipe's Motion (one phrase at Motion 0), and a
/// Break, the longest Build and the Outro follow. A Groove that ends into a short Build plays on and a later one ends
/// the song; that is rare and isn't counted.
int32_t maxBars(const Recipe& recipe, int32_t targetSteps);

/// The bars the COMPOSING progress bar counts to (spec §14.5): the target plus one Outro phrase. The song may run
/// longer, so the bar may jump at the end.
int32_t expectedBars(const Recipe& recipe, int32_t targetSteps);

/// The size estimate, in bytes (spec §15.9): kSongCosts.song, the clips' setup, and per bar of maxBars() a note on
/// every step for each note a part can start there (a Kit's lanes, a Chords or Pad voicing) and a node on each
/// automation lane its sets can move.
int64_t estimateBytes(const Recipe& recipe, int32_t lengthSeconds);

/// The longest LENGTH, in kLengthStepSeconds steps up to kMaxLengthSeconds, whose estimate is at most `bytes`: the MAX
/// of `TOO LONG: MAX 12:30` (spec §14.5). 0 when not even kMinLengthSeconds fits.
int32_t longestFitting(const Recipe& recipe, int64_t bytes);

} // namespace deluge::generative::compose
