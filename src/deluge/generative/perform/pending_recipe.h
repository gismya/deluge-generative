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

// A transition's pending Recipe (spec §12.9), as plain data: what replaces the Current Recipe at the start step, or at
// once at a stop's commit. PerformSteering::handOver() makes it Current; the adapter copies its sound picks.

#pragma once

#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::perform {

/// The pending Recipe, with its seed, takes and sound picks. The Current Recipe stays the playing one until the
/// handover.
///
/// `sounds` comes first for the start step's cost in the firmware's copy of the sound picks: see pendingRecipe in
/// transitions.cpp.
struct PendingRecipe {
	SoundPicks sounds;
	Recipe recipe;
	uint32_t seed = 0;
	Takes takes;
	/// A genre change, not a Regenerate: the adapter's preload staged the new genre's palette, which comes with the
	/// Recipe. The preload works it out once; nothing else compares the genres.
	bool genreChange = false;
};

/// Whose macros the Current Recipe has after a handover (spec §12.9).
enum class HandoverMacros : uint8_t {
	Live,    ///< the player's, as they are at the handover: a genre change or Regenerate keeps them
	Pending, ///< the pending Recipe's: a LOAD keeps the file's
};

} // namespace deluge::generative::perform
