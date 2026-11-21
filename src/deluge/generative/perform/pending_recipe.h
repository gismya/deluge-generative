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

/// Whose macros the Current Recipe has after a handover (spec §12.9).
enum class HandoverMacros : uint8_t {
	Live,    ///< the player's, as they are at the handover: a genre change or Regenerate keeps them
	Pending, ///< the pending Recipe's: a LOAD keeps the file's
};

/// Whether the Recipe that comes in keeps the Part biases of the one playing (spec §3.3): a Regenerate (the live
/// macros, the same genre, so the same roster) does; a genre change starts at the new Recipe's (0) and a LOAD at its
/// file's. The one rule for the handover, the incoming instance's start step and a Regenerate while stopped.
constexpr bool keepsPartBias(HandoverMacros macros, const Genre* incoming, const Genre* playing) {
	return macros == HandoverMacros::Live && incoming == playing;
}

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
	/// The adapter's preload staged the incoming genre's palette, which comes with the Recipe: a genre change, or a
	/// LOAD (which reads its genre's card override). The preload works it out once; nothing else compares the genres.
	bool stagedPalette = false;
	/// Whose macros the handover keeps: Live for a genre change or Regenerate, Pending for a LOAD (spec §12.9). With
	/// Pending the incoming instance keeps them at the start step too, instead of taking the live ones.
	HandoverMacros macros = HandoverMacros::Live;
};

} // namespace deluge::generative::perform
