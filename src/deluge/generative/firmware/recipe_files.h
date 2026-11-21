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

// Recipe files on the card (spec §16): the RECIPES/ folder, the names SAVE suggests, writing the Current Recipe and
// reading a file back. The format and the name rules are portable (core/recipe_file.h, perform/recipe_name.h).

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/recipe_file.h"
#include "generative/perform/recipe_name.h"
#include <cstdint>
#include <string_view>

namespace deluge::generative::firmware {

/// The top-level folder Recipe files live in (spec §16.3), created on the first save.
constexpr const char* kRecipeFolder = "RECIPES";

/// The longest Recipe file the firmware reads: every part and lane with the longest path, entity-encoded, fits.
constexpr int32_t kMaxRecipeFileBytes = 16384;

/// Whether the button handler may read or write a Recipe file now: not while the runner is busy or a card access is
/// under way (spec §12.10). LOAD's browser asks this.
[[nodiscard]] bool recipeCardAccessAllowed(bool inCardRoutine);
/// Whether a SAVE can run now: recipeCardAccessAllowed(), with a Current Recipe to save.
[[nodiscard]] bool recipeSaveAllowed(bool inCardRoutine);

/// The name SAVE suggests for the Current Recipe (spec §16.3), from what RECIPES/ holds: its remembered name's next
/// free letter variation, or the default name (or its first free variation) if it has none. False, with `out` empty,
/// if no variation is free. A card error returns false with `error` set.
bool suggestRecipeName(perform::RecipeName& out, Error& error);

/// Writes the Current Recipe, as it is when called, to `path` (`RECIPES/<name>.XML`), creating RECIPES/ if need be.
/// Without `mayOverwrite` an existing file returns Error::FILE_ALREADY_EXISTS and is left alone. On success the
/// Current Recipe remembers `name` (adoptSavedName() against `suggestion`), unless it was replaced meanwhile. A failed
/// write changes nothing else: a file it had created is removed again.
Error saveCurrentRecipe(const char* path, std::string_view name, const perform::RecipeName& suggestion,
                        bool mayOverwrite);

/// The quick save (spec §16.4, Perform playing): saves the Current Recipe at once under the suggested name, never
/// overwriting. NoFreeName when no letter variation is free; Failed with `error` on a card error.
enum class QuickSave : uint8_t { Saved, NoFreeName, Failed };
QuickSave quickSaveRecipe(perform::RecipeName& saved, Error& error);

/// Reads the Recipe file at `path` into `out` (Recipe LOAD and the test port's check). A missing or oversized file, or
/// a card error (then `error` is set), reads as Unreadable.
RecipeFileRead readRecipeFile(const char* path, RecipeFile& out, Error& error);

/// A LOAD's notice for a file of another genre version (spec §16.2): `Older ACID TECHNO`, or Newer for a file from a
/// later firmware. Nothing for the genre's own version.
void showVersionNotice(const RecipeFile& file);
/// A LOAD's warning for its missing sounds, re-rolled (spec §16.6): `2 sounds replaced`. Nothing for none.
void showSoundsReplaced(int32_t count);

} // namespace deluge::generative::firmware
