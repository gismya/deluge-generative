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

// The Compose build (spec §14.5, §15.7, §15.9): the Current Recipe rendered into a private Song as a modal runner job,
// under the size estimate and the SDRAM reserve, then swapped in stopped and opened in arranger view.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/runner/runner.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// The build ended, in the job's task, before the UI is set for the song. COMPOSING has closed already; the caller
/// closes anything of its own under it. On Done the new song is current and stopped.
/// Otherwise the player's song is untouched, or, when the build had to free it first, a blank song is current; the job
/// has already said why on the OLED.
using ComposeEnded = void (*)(JobOutcome outcome);

/// Why beginCompose() started nothing, or that it started.
enum class ComposeStart : uint8_t {
	Started,
	PerformActive,
	RunnerBusy,
	NoRecipe, ///< No genre given, and no Current Recipe to render
};

/// Starts a Compose build of `lengthSeconds` (spec §14.4's LENGTH) as a modal runner job, under the COMPOSING screen
/// it opens. With a `genre` the job first rolls a fresh Current Recipe for it, as a genre pick does (spec §14.1); with
/// nullptr it renders the Current Recipe, which must exist. Refused, with nothing started or opened, while Perform is
/// active or the runner is busy, or with no Recipe.
ComposeStart beginCompose(const Genre* genre, int32_t lengthSeconds, ComposeEnded ended);

/// The COMPOSING progress bar (spec §14.5): bars rendered so far, against the bars expected at the target length plus
/// one Outro phrase. `expected` is 0 until the render starts.
struct ComposeProgress {
	int32_t bars = 0;
	int32_t expected = 0;
};
[[nodiscard]] ComposeProgress composeProgress();

/// BUILD's first check (spec §14.5): -1 if a song of `lengthSeconds` for `recipe` fits alone, else the longest LENGTH
/// that does, for TOO LONG: MAX m:ss (0 if none does). The job checks again, after a genre roll.
[[nodiscard]] int32_t composeTooLong(const Recipe& recipe, int32_t lengthSeconds);
/// The popup for composeTooLong()'s `longest`: TOO LONG: MAX m:ss, or TOO LONG when no LENGTH fits (0).
void showComposeTooLong(int32_t longest);

/// Free memory a build may use once the player's song is gone: what an empty Deluge has free, internal and SDRAM
/// (spec §15.9). (tuning): H19 measures it.
constexpr int64_t kEmptyDelugeFreeBytes = 48 * 1024 * 1024;
/// The SDRAM a build leaves free (spec §15.9): the build stops when it would go below it. (tuning)
constexpr uint32_t kSdramReserveBytes = 512 * 1024;

} // namespace deluge::generative::firmware
