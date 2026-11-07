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
#include "generative/core/palette.h"
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include "generative/runner/runner.h"

class Song;
class Output;
class InstrumentClip;
class Drum;

namespace deluge::generative::firmware {

/// One part's Output in Perform's song.
struct PartOutput {
	Output* output = nullptr; ///< A Kit for Drums and FX, a SoundInstrument otherwise.
	InstrumentClip* clip = nullptr;
	Drum* drums[kMaxLanesPerPart] = {}; ///< Kit parts: one drum per row, by laneRow().
};

struct Roster {
	PartOutput parts[kMaxParts];
};

/// The song swing interval for a Recipe one (a note value, 0 the 16th): a sync level relative to the song's magnitude,
/// where 8 - magnitude is a 16th and each step down doubles the note value. Perform's song runs at the Generator's
/// magnitude (spec §12.3).
constexpr int32_t swingSyncLevel(int32_t interval) {
	return 8 - kGeneratorTickMagnitude - interval;
}

/// Builds Perform's song for `recipe`: a new Song at magnitude 2 with the Recipe's tempo and swing, and the roster's
/// Outputs linked in, each sound loaded through its palette's fallback chain with its load-time settings (spec §10).
/// Reads the card and runs as a runner job, at a chunk boundary after each Kit row and each Output. The player's song
/// is untouched; on failure nothing is left allocated. Returns ABORTED_BY_USER when cancelled, and a card error or
/// RAM error as it came (spec §10.7). `sounds` are the Recipe's sound picks and `palette` the
/// genre's palette with any card override over it. `soundsMissing` counts the parts on the default synth and played
/// lanes left silent, so the caller shows the one warning (spec §10.1, §10.2).
Error buildPerformSong(const Recipe& recipe, const SoundPicks& sounds, const Palette& palette, Song** songOut,
                       Roster* roster, Chunks& chunks, int32_t* soundsMissing = nullptr);

/// Writes a key and scale into `song`'s key by plain assignment (spec §12.8). Perform's clips are empty, so nothing is
/// remapped, and firmware that reads the song key, such as the arpeggiator's chord mode, agrees with what plays.
/// Never Song::setScale or setRootNote, which remap or re-infer from clip notes (spec §7.1).
void writeSongKey(Song* song, uint8_t keyRoot, Scale scale);

/// Frees a song buildPerformSong() made that was never swapped in.
void deletePrivateSong(Song* song);

} // namespace deluge::generative::firmware
