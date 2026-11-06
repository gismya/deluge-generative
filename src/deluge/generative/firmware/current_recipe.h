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
#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::firmware {

struct CurrentRecipe {
	Recipe recipe;
	uint32_t seed = 0;
	Takes takes;
	bool exists = false;

	/// Picking a genre rolls a fresh Recipe: rolled facts and sound picks, a new seed, takes at 0 and the genre's
	/// default macros (spec §14.1). Facts, sound picks and the seed come from fresh hardware-timer entropy
	/// (spec §3.2, §6.5, §10.1). Sound picks read the card, so this runs in task context. On a card error it returns
	/// the error and leaves the Current Recipe as it was.
	Error rollFromGenre(const Genre& genre);
};

extern CurrentRecipe currentRecipe;

/// Fresh entropy from a hardware timer.
uint32_t freshEntropy();

} // namespace deluge::generative::firmware
