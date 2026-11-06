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

// The Current Recipe (spec §14.2): the one Recipe, with its seed and takes, that Generative mode works on. Kept in
// RAM until power-off. Perform steers it and the Compose screen edits it.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/genre.h"
#include "generative/core/palette_file.h"
#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::firmware {

struct CurrentRecipe {
	Recipe recipe;
	/// The Recipe's sound picks, kept beside it so the Generator's copy of the Recipe stays small (spec §2.1).
	SoundPicks sounds;
	uint32_t seed = 0;
	Takes takes;
	bool exists = false;

	/// Picking a genre rolls a fresh Recipe: rolled facts and sound picks, a new seed, takes at 0 and the genre's
	/// default macros (spec §14.1). Facts, sound picks and the seed come from fresh hardware-timer entropy
	/// (spec §3.2, §6.5, §10.1). Sound picks read the card, so this runs in task context. On a card error it returns
	/// the error and leaves the Current Recipe as it was.
	///
	/// It reads the genre's palette override file, `GENERATIVE/PALETTES/<GENRE NAME>.XML`, when the card has one
	/// (spec §10.1). A malformed or oversized file is ignored, and the built-in palette is used.
	Error rollFromGenre(const Genre& genre);

	/// The palette the Recipe's sounds come from: the genre's built-in one, with the card's override over it if the
	/// last genre pick read one.
	[[nodiscard]] const Palette& palette() const;

private:
	PaletteOverride override_;
	const Genre* overrideGenre_ = nullptr;
};

extern CurrentRecipe currentRecipe;

/// Fresh entropy from a hardware timer.
uint32_t freshEntropy();

} // namespace deluge::generative::firmware
