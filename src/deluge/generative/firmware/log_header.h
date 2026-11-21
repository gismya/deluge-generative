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

class ModelStackWithAutoParam;
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

/// The piece's header: the Current Recipe's sound picks, and every param on the song and on each part's Output and
/// Kit rows, as they are now. In the tick, at the piece's step 0 decision.
void logPieceHeader(SteeringLog& log, const SoundPicks& picks, const Roster& roster, const Genre& genre, Song& song);

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
