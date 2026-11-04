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

// Generative mode POC: the shape of a genre's tables. A genre is pure data (issue #11): the generic Generator
// reads these fields and never branches on which genre is playing. The POC fills only the slots it uses:
// phrase length, Density curve, drum lanes and one Loop-flavour Bass part.

#pragma once

#include "generative/core/generator.h"
#include <cstdint>

namespace deluge::generative {

/// Steps in a bar-long importance or accent row.
constexpr int32_t kGridSteps = kStepsPerBar;
/// Importance value that always plays, at any Density, with no jitter. 0 never plays.
constexpr uint8_t kImportanceAnchor = 100;
/// Full accent in an accent row or a Loop step. Accent boosts scale linearly up to this.
constexpr uint8_t kAccentFull = 64;

/// One drum lane's rhythm and feel.
struct DrumLaneTable {
	/// Step importance per 16th of the bar (#24). kImportanceAnchor always plays, 0 never does. Other steps play
	/// when importance + seeded jitter reaches the Density threshold.
	uint8_t importance[kGridSteps];
	/// Accent amount per 16th, 0..kAccentFull.
	uint8_t accent[kGridSteps];
	/// The lane plays only while Density is at or above this (#12, #24).
	uint8_t densityGate;
	/// Velocity at Energy 0 and Energy 50, before accents.
	uint8_t velocityLow;
	uint8_t velocityHigh;
	/// Velocity added by a full accent at Energy 0 and at Energy 50.
	uint8_t accentLow;
	uint8_t accentHigh;
	/// Seeded velocity wobble, ± this much.
	uint8_t velocityJitter;
	/// Lean in ticks for on-beat (even) and off-beat (odd) 16ths, -kMaxLean..kMaxLean (#20, ADR 0003).
	int8_t leanOnBeat;
	int8_t leanOffBeat;
	/// Ticks from note-on to note-off (the kit plays one-shots, so this only needs to be short and non-zero).
	uint8_t gateTicks;
	/// Phrase-end drop: in a phrase's last bar, the lane rests for its last N beats, N seeded per phrase between
	/// these two values. 0..0 means no drop.
	uint8_t phraseEndDropBeatsMin;
	uint8_t phraseEndDropBeatsMax;
};

/// One scale degree a Loop step may land on, with its draw weight.
struct DegreeWeight {
	uint8_t degree; ///< 0..6 on the seven-step grid (#21).
	int8_t octave;  ///< Octaves from the part's tonic before Register folding.
	uint8_t weight;
};

/// One gate length a Loop step may use, with its draw weight.
struct LengthWeight {
	uint8_t ticks;
	uint8_t weight;
};

constexpr int32_t kMaxDegreeWeights = 8;
constexpr int32_t kMaxLengthWeights = 4;

/// A Loop-flavour part (#12): a stored line of kGridSteps steps that Density thins and Motion mutates.
struct LoopTable {
	/// Step importance per 16th, as for drum lanes. Density rests the lower-importance steps.
	uint8_t importance[kGridSteps];
	/// Percent chances, drawn per Loop step when it is rolled.
	uint8_t restChance;   ///< A structural rest, whatever the Density.
	uint8_t accentChance; ///< An accented step.
	uint8_t tieChance;    ///< The note holds through the next step, which then rests.
	uint8_t slideChance;  ///< The note overlaps the next one, so a LEGATO voice glides.
	/// Pitch material, in scale steps.
	DegreeWeight degrees[kMaxDegreeWeights];
	uint8_t numDegrees;
	/// When true, Loop step 0 is always the tonic, so mutation never loses the line's home.
	bool tonicOnFirstStep;
	/// Gate lengths for plain (unslid) steps.
	LengthWeight lengths[kMaxLengthWeights];
	uint8_t numLengths;
	/// Ticks added to plain gate lengths at Energy 50 (scaled linearly from 0 at Energy 0).
	uint8_t lengthEnergyBonus;
	/// How far a slid note overlaps the next step's note, in ticks.
	uint8_t slideOverlapTicks;
	/// Minimum ticks of silence a plain note leaves before the next step's note.
	uint8_t minGapTicks;
	/// Velocity at Energy 0 and Energy 50, and what a full accent adds at each.
	uint8_t velocityLow;
	uint8_t velocityHigh;
	uint8_t accentLow;
	uint8_t accentHigh;
	uint8_t velocityJitter;
	int8_t leanOnBeat;
	int8_t leanOffBeat;
	/// Register (#21): the tonic is the key root's pitch class nearest the anchor. Every note is octave-folded
	/// into anchor + spanLow .. anchor + spanHigh. The span must be at least 11 semitones wide.
	uint8_t registerAnchor;
	int8_t spanLow;
	int8_t spanHigh;
	/// Loop steps re-rolled at each phrase line at Motion 50 (scaled linearly from 0 at Motion 0), before a
	/// seeded ±1.
	uint8_t mutateStepsAtMaxMotion;
};

/// A genre's tables. Fields are read live by the Generator; a Recipe never copies them.
struct GenreTables {
	const char* name;
	/// Bars per phrase (#11). Phrase lines are where the Loop mutates and drums may drop.
	uint8_t phraseBars;
	/// The importance a step needs to play, at Density 0 and at Density 50 (linear between). Shared by all
	/// parts and lanes so Density moves every part the same way.
	int16_t thresholdAtDensityMin;
	int16_t thresholdAtDensityMax;
	/// Seeded jitter added to non-anchor importances, ± this much.
	uint8_t importanceJitter;
	/// Chance (percent, at Motion 50, linear from 0) that the drums' jitter pattern changes at a phrase line.
	uint8_t drumVariationAtMaxMotion;
	DrumLaneTable lanes[kNumLanes];
	LoopTable bass;
};

/// The Techno genre (brief in issue #11).
extern const GenreTables kGenreTechno;

} // namespace deluge::generative
