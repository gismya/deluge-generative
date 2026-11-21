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

// What only the firmware knows of a piece's log (spec §17.2): its header's sound picks and param snapshot, and a sound
// re-roll's new picks. Capture (#145) reads the snapshot blocks back in this layout.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/steering_log.h"
#include "generative/firmware/perform_song.h"
#include <cstdint>
#include <optional>

class AutoParam;
class ModelStackWithAutoParam;
class ParamManager;
class Song;

namespace deluge::generative::firmware {

/// A param snapshot block's first bytes. Then `count` values: for a param set, each param's value as an int32, by
/// param id; for patch cables, a CableValue each. Unaligned: read each with memcpy.
struct SnapshotBlock {
	uint8_t output;     ///< A part, or kSongParams.
	Lane lane;          ///< A Kit row's lane, or Lane::None for the Output's own params (or the song's).
	uint8_t collection; ///< The collection's index among its ParamManager's main ones: 0, 1, or 2 for patch cables.
	uint16_t count;
};
static_assert(sizeof(SnapshotBlock) == 6);
/// A patch cable in a snapshot block: its destination (ParamDescriptor's data), its amount and its source, packed in
/// that order.
struct CableValue {
	uint32_t destination;
	int32_t value;
	uint8_t source;
};
constexpr int32_t kCableValueBytes =
    sizeof(CableValue::destination) + sizeof(CableValue::value) + sizeof(CableValue::source);
static_assert(kCableValueBytes == 9);

/// The piece's header: the Current Recipe's sound picks, and every param on the song (if `songEdited`) and on the
/// Output and Kit rows of each part in `editedParts` (a bit a part), as they are now (spec §17.2, H13 option A): an
/// Output not edited since it loaded holds its preset's values, which Capture loads from the same file. In the tick,
/// at the piece's step 0 decision.
void logPieceHeader(SteeringLog& log, const SoundPicks& picks, const Roster& roster, const Genre& genre, Song& song,
                    uint8_t editedParts, bool songEdited);
/// Which Output `manager` belongs to: a part's (its clip's or a Kit row's), kSongParams for the song's, or -1.
int32_t editedOutput(const ParamManager* manager, const Roster& roster, const Genre& genre, Song& song);
/// The parts whose Output (or Kit rows) `piece`'s header snapshot holds, a bit a part, and whether it holds the song's.
uint8_t snapshotParts(const SteeringLog::Piece& piece, bool* song);
/// The song's params back to what a Perform song is built with (an edited song AS IT BEGAN can't read back), its
/// tempo as it is.
void resetSongParams(Song& song);

/// The param snapshot of `piece`'s header written back (AS IT BEGAN, spec §13.2): each param on the song and on each
/// part's Output and Kit rows that differs from the snapshot takes its value, and each patch cable whose destination
/// and source are still there its amount. A Sound with any param written is patched again. Params the snapshot
/// doesn't hold (a cable added since) keep theirs. Writes without notifying, so call it with no voice sounding: on a
/// restart's line after its flush, or stopped. @return the params written.
int32_t restoreSnapshot(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre, Song& song);

/// The param a SetParam names (spec §17.2): on the song, or on part `edit.target`'s Output (its clip's params) or
/// `edit.lane`'s Kit row, keyed by `edit.length` as paramEdit() keys it. A patch cable is found by its source and its
/// destination's param.
struct LoggedParam {
	ParamManager* manager = nullptr;
	AutoParam* param = nullptr;
	int32_t collection = 0; ///< Its collection's index among the manager's summaries
	int32_t slot = 0;       ///< Its bit in the collection's summary: the param id, or the cable's index
};
std::optional<LoggedParam> loggedParam(const Command& edit, const Roster& roster, const Genre& genre, Song& song);

/// `part`'s new picks (`picks`, its preset or each of its lanes' samples), landed on `step` with `token`.
void logSoundPaths(SteeringLog& log, int32_t step, uint8_t part, uint8_t token, const SoundPicks& picks,
                   const Genre& genre);

/// A player edit to a param no sound control moves, as a SetParam (spec §17.2): the part whose Output (or Kit row)
/// holds it, or the song's, and the param keyed as SoundControls::paramKey() keys a bound one (bits 13–14 its
/// collection, 8–12 a patch cable's source, 0–7 the param). A cable aimed at another cable's depth keeps only its
/// final param. nullopt for a param on none of them.
std::optional<Command> paramEdit(const ModelStackWithAutoParam& stack, int32_t value, const Roster& roster,
                                 const Genre& genre, Song& song);

} // namespace deluge::generative::firmware
