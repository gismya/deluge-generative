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

// The Compose screen's edits to the Current Recipe (spec §14.4). Those that read the card (a genre pick, REGENERATE,
// a sound re-roll) run as a modal runner job over the screen, with the working animation, and BACK cancels them with
// the Recipe as it was. The rest act at once: back to a previous pick, NEW SEED.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/types.h"
#include "generative/runner/runner.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// What a card-reading edit does.
enum class RecipeRoll : uint8_t {
	Genre,      ///< A fresh Recipe from a genre, as a genre pick (spec §14.1)
	Regenerate, ///< REGENERATE (spec §9.7): the genre's facts, every sound, the seed and the takes rolled again
	Part,       ///< A part's sound picks re-rolled (a Kit part: every lane)
	Lane,       ///< One Drums or FX lane's pick re-rolled
};

/// The edit ended, in the job's task, with the modal closed and the Current Recipe changed on Done. A card error has
/// shown its message already.
using RecipeRollEnded = void (*)(JobOutcome outcome);

/// Starts a card-reading edit: `genre` for Genre, `part` for Part, `lane` for Lane. False, with nothing started, while
/// Perform is active or the runner is busy, or with no Current Recipe to edit (Genre needs none).
bool beginRecipeRoll(RecipeRoll roll, const Genre* genre, int32_t part, Lane lane, RecipeRollEnded ended);

/// SHIFT+push on a SOUNDS row (spec §14.4): the part's picks (a synth part's preset) or the lane's go back to the ones
/// before the last re-roll of that row, which become the previous ones in turn. False if there are none: the row
/// was never re-rolled since the Recipe was made.
bool previousPartPick(int32_t part);
bool previousLanePick(Lane lane);

/// NEW SEED (spec §6.5): a fresh seed, every take at 0.
void newSeed();

/// LENGTH (spec §14.4): the target length in seconds, kept until power-off and never saved.
int32_t composeLength();
void setComposeLength(int32_t seconds);

} // namespace deluge::generative::firmware
