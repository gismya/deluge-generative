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
#include "generative/core/recipe_file.h"
#include "generative/core/types.h"
#include "generative/perform/recipe_name.h"
#include "generative/runner/runner.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// The edit ended, in the job's task, with the modal closed and the Current Recipe changed on Done. A card error has
/// shown its message already.
using RecipeRollEnded = void (*)(JobOutcome outcome);

/// Card-reading edits. Each is false, with nothing started, while Perform is active or the runner is busy, or (all
/// but beginGenreRoll()) with no Current Recipe to edit.
///
/// A fresh Recipe from `genre`, as a genre pick (spec §14.1).
bool beginGenreRoll(const Genre& genre, RecipeRollEnded ended);
/// REGENERATE (spec §9.7): the genre's facts, every sound, the seed and the takes rolled again.
bool beginRegenerate(RecipeRollEnded ended);
/// LOAD on the Compose screen (spec §16.5): `file` replaces the Current Recipe, which remembers `name`, with its
/// missing sounds re-rolled and the sounds replaced popup (spec §16.6). Both are copied. Needs no Current Recipe.
bool beginRecipeLoad(const RecipeFile& file, const perform::RecipeName& name, RecipeRollEnded ended);
/// `part`'s sound picks re-rolled (a Kit part: every lane it plays). False for a part the genre doesn't have.
bool beginPartRoll(int32_t part, RecipeRollEnded ended);
/// One Drums or FX lane's pick re-rolled.
bool beginLaneRoll(Lane lane, RecipeRollEnded ended);

/// SHIFT+push on a SOUNDS row (spec §14.4): the part's picks (a synth part's preset) or the lane's go back to the ones
/// before the last re-roll of that row, which become the previous ones in turn. False if there are none: the row
/// was never re-rolled since the Current Recipe was made (any replacement of it, outside the screen too, forgets
/// them: CurrentRecipe::generation).
bool previousPartPick(int32_t part);
bool previousLanePick(Lane lane);
/// A Perform sound swap gave `part` (of `genre`) a new pick (spec §13.4): its SOUNDS row, and a Kit part's lane rows,
/// no longer have a previous pick from a re-roll here (spec §14.4).
void forgetPreviousPick(const Genre& genre, int32_t part);

/// NEW SEED (spec §6.5): a fresh seed, every take at 0.
void newSeed();

/// LENGTH (spec §14.4): the target length in seconds, kept until power-off and never saved.
int32_t composeLength();
void setComposeLength(int32_t seconds);

} // namespace deluge::generative::firmware
