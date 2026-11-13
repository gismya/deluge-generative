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

// The Chord-graph walk (spec §7.4): one keyed draw per chord picks the next chord from the genre's graph, the
// Generator-wide colour moves and walk rules, with the guard thinning chords that aren't plain major or minor.
// Chords are degrees 0–6 from the tonic, so the same graph plays unchanged in every scale and a key change keeps the
// walk's place. Integer math only (ADR 0004).

#pragma once

#include "generative/core/pitch.h"
#include "generative/core/types.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

/// The home chord's degree: the tonic triad (spec §7.4).
constexpr int32_t kHomeChord = 0;

/// The chord guard, data with Generator-wide defaults so a later genre field, Recipe value or player setting can
/// override it (spec §7.4).
enum class ChordGuardRule : uint8_t {
	Strict,       ///< any triad that isn't plain major or minor
	TritoneFifth, ///< only a triad whose fifth is a tritone
};
struct ChordGuard {
	ChordGuardRule rule = ChordGuardRule::Strict;
	int16_t factor = 150; ///< Per-mille, applied to a guarded degree's weight (tuning).
};

/// A weighted move in a graph, or a colour move: from one degree to another.
struct ChordMove {
	uint8_t from = 0;
	uint8_t to = 0;
	uint8_t weight = 0;
};

/// The Generator-wide colour moves (spec §7.4), one row per scale (tuning): weight added to the moves that give the
/// scale its character, in whichever graph plays. Data, so a later override can replace a row.
constexpr int32_t kMaxColourMoves = 2;
constexpr ChordMove kColourMoves[kNumScales][kMaxColourMoves] = {
    {},                       // Major
    {},                       // Minor
    {{0, 3, 20}},             // Dorian: i–IV
    {{0, 1, 20}},             // Phrygian: i–♭II
    {{0, 1, 20}},             // Lydian: I–II
    {{0, 6, 20}},             // Mixolydian: I–♭VII
    {},                       // Locrian
    {},                       // Melodic Minor
    {{4, 0, 20}, {3, 4, 20}}, // Harmonic Minor: V–i, iv–V
    {},                       // Hungarian Minor
    {},                       // Marva
    {},                       // Arabian
};

/// A genre's Chord graph (spec §7.4), the walking half of its Harmony slot.
struct ChordGraph {
	/// Weight added to home from any other chord.
	uint8_t homePull = 0;
	/// Up to kMaxChordMoves favourite moves.
	std::span<const ChordMove> moves;
	/// Weight of staying on the current chord. A repeat counts as a chord for the cadence period.
	uint8_t repeatWeight = 0;
	/// Whether the walk may go straight back to the chord before the current one (A-B-A).
	bool abaAllowed = true;
	/// The degree every cadencePeriod-th chord leans to, and before a Drop. Guarded in the scale, it swaps to V,
	/// else IV.
	uint8_t cadenceTarget = 0;
	/// In chords, counted from each Section start. 0: no regular cadence.
	uint8_t cadencePeriod = 0;
	/// Harmonic rhythm, in bars per chord: divides the phrase or is a whole multiple of it.
	uint8_t barsPerChord = 2;
};
constexpr int32_t kMaxChordMoves = 8;

/// The walk rules' bonuses, Generator-wide values (spec §7.4, tuning), in the graph's weight units.
struct WalkRules {
	int16_t cadenceBonus = 20;
	int16_t dropCadenceBonus = 40; ///< Heavier than the regular cadence, and added to it when both fall on a chord.
	int16_t phraseHomeBonus = 30;
	int16_t sectionHomeBonus = 15; ///< Smaller than the phrase home bonus.
};

/// Where a chord falls, for the walk rules. Worked out by the Generator from its Arc.
struct ChordPlace {
	bool home = false;         ///< Always home: the piece's first chord, or a Drop's.
	bool sectionStart = false; ///< The first chord of a Section.
	bool phraseFirst = false;  ///< The first of a phrase that holds 2 or more chords.
	bool phraseLast = false;   ///< The last of such a phrase: home is excluded.
	bool cadence = false;      ///< Every cadencePeriod-th chord since the Section start.
	bool beforeDrop = false;   ///< The last chord before a Drop.
	int32_t complexity = 0;    ///< The Recipe's Complexity, which shrinks the home bonuses.
};

/// Whether `degree`'s triad in `scale` is guarded. Home never is: the guard can't move it.
bool chordGuarded(Scale scale, int32_t degree, ChordGuard guard);

/// The graph's cadence target in `scale`, swapped to V, else IV, when it is guarded.
int32_t cadenceTarget(const ChordGraph& graph, Scale scale, ChordGuard guard);

/// Each degree's weight as the next chord after `current` (and `previous`, the chord before it, or -1).
struct ChordWeights {
	int32_t weight[kDegreesPerScale] = {};
};
ChordWeights chordWeights(const ChordGraph& graph, Scale scale, int32_t current, int32_t previous,
                          const ChordPlace& place, ChordGuard guard = {}, std::span<const ChordMove> colour = {},
                          WalkRules rules = {});

/// The next chord: home when the place says so, else a pick by chordWeights() from `draw`, a uniform 32-bit value.
/// When every weight is 0 the walk stays on `current`, or goes to the cadence target if home is excluded.
int32_t nextChord(const ChordGraph& graph, Scale scale, int32_t current, int32_t previous, const ChordPlace& place,
                  uint32_t draw, ChordGuard guard = {}, std::span<const ChordMove> colour = {}, WalkRules rules = {});

/// The Generator-wide colour row for `scale`.
constexpr std::span<const ChordMove> colourMoves(Scale scale) {
	return kColourMoves[static_cast<int32_t>(scale)];
}

} // namespace deluge::generative
