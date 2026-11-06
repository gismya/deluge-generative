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

// Perform's own song (spec §13.1, §12.8): built privately from the Current Recipe while the player's song keeps
// playing, with one Output per part. Each Output has one empty, always-active, looping 1-bar session clip, and a Kit
// clip one NoteRow per lane in the fixed lane order. Notes never go into these clips (ADR 0002).

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/recipe.h"
#include "generative/core/types.h"

class Song;
class Output;
class InstrumentClip;
class Drum;

namespace deluge::generative::firmware {

/// One part's Output in Perform's song.
struct PartOutput {
	Output* output = nullptr; ///< A Kit for Drums and FX, a SoundInstrument otherwise.
	InstrumentClip* clip = nullptr;
	Drum* drums[kMaxLanesPerPart] = {}; ///< Kit parts: one drum per lane, by the part's lane index.
};

struct Roster {
	PartOutput parts[kMaxParts];
};

/// Builds Perform's song for `recipe`: a new Song at magnitude 2 with the Recipe's tempo and swing, and the roster's
/// Outputs linked in. Reads the card and yields to audio between loads, so it runs in task context. The player's
/// song is untouched; on failure nothing is left allocated.
Error buildPerformSong(const Recipe& recipe, Song** songOut, Roster* roster);

/// Frees a song buildPerformSong() made that was never swapped in.
void deletePrivateSong(Song* song);

} // namespace deluge::generative::firmware
