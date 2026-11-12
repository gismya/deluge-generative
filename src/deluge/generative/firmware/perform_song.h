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
#include "generative/core/pitch.h"
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

/// Builds part `part`'s Output from `sounds` and links it into `song`, with its empty 1-bar clip, as buildPerformSong()
/// does for each part. `song` may be the current one, playing: a sound re-roll's preload (spec §10.7) builds the new
/// Output beside the old one, and links it in one chunk at the end, silent until notes go to it. Reads the card;
/// runs in a runner job. On failure nothing is left allocated. `soundsMissing` counts what fell back to nothing.
Error buildPartOutput(Song* song, const Genre& genre, int32_t part, const SoundPicks& sounds, const Palette& palette,
                      PartOutput* out, Chunks& chunks, int32_t* soundsMissing = nullptr);

/// Whether a part's Output makes no sound now (spec §12.9): a Synth that skips rendering; a Kit with no drum rendering,
/// no repeats left on its delay, no stutter, no arp notes and no non-audio drum voices.
[[nodiscard]] bool partSilent(const PartOutput& part);

/// Ramps a part's Output volume down to silence over `ticks` song ticks (spec §12.9: a retiring Output still sounding
/// at its cap): a Synth's post-FX volume, a Kit's own, after its FX. A live ramp, so it moves only while playing.
void fadeOutPart(const PartOutput& part, int32_t ticks);

/// Whether the gold knobs are on a part's Output: its clip, its Kit or Sound, or one of its drums. MIDI follow, the
/// knob popup and the knob rings read the same binding.
[[nodiscard]] bool knobsOn(const PartOutput& part);

/// Unlinks a part's Output and its clip from `song`, playing or not, and frees them (spec §12.9), clearing the
/// pointers stock keeps at them. Yields through the audio routine once per Kit row. Task context only: never in a
/// tick. `part` is left empty.
void freePartOutput(Song* song, PartOutput& part);

/// Deletes every audio file no song uses (spec §13.8, §20). Stock firmware keeps an unused Sample as a stealable
/// cache, still holding internal and external heap (its cluster list, its path), and each entry or sound re-roll picks
/// new sounds, so each would leave another set behind. This deletes them as stealing would. It's the loop of
/// AudioFileManager::deleteAllUnusedAudioFiles() (the fork's helper for stock cardReinserted(),
/// docs/upstream-candidates.md), but keeps temp recordings: they stay for deleteAnyTempRecordedSamplesFromMemory(),
/// which also resets the CLIPS recording number when it deletes them.
void deleteUnusedAudioFiles();

/// Writes a key and scale into `song`'s key by plain assignment (spec §12.8). Perform's clips are empty, so nothing is
/// remapped, and firmware that reads the song key, such as the arpeggiator's chord mode, agrees with what plays.
/// Never Song::setScale or setRootNote, which remap or re-infer from clip notes (spec §7.1).
void writeSongKey(Song* song, Key key);

/// Frees a song buildPerformSong() made that was never swapped in.
void deletePrivateSong(Song* song);

} // namespace deluge::generative::firmware
