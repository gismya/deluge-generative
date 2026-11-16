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

#include "generative/core/chord_walk.h"
#include "generative/core/recipe.h"
#include <algorithm>

namespace deluge::generative {

namespace {

/// The degree's triad: its 3rd and 5th in semitones above its root.
struct Triad {
	int32_t third;
	int32_t fifth;
};
constexpr Triad triadOf(Scale scale, int32_t degree) {
	const int32_t root = semitonesOf(degree, scale);
	return {semitonesOf(degree + kChordThird, scale) - root, semitonesOf(degree + kChordFifth, scale) - root};
}

// Intervals in semitones.
constexpr int32_t kMinorThird = 3;
constexpr int32_t kMajorThird = 4;
constexpr int32_t kTritone = 6;
constexpr int32_t kPerfectFifth = 7;

/// V, else IV when V is guarded: where a guarded cadence target swaps to, and where a phrase's last chord leaves home
/// when nothing else weighs and the graph's target is home or none.
GEN_STEP_CODE int32_t dominant(Scale scale, ChordGuard guard) {
	return chordGuarded(scale, kV, guard) ? kIV : kV;
}

} // namespace

GEN_STEP_CODE bool chordGuarded(Scale scale, int32_t degree, ChordGuard guard) {
	if (degree == kHomeChord) {
		return false;
	}
	const Triad triad = triadOf(scale, degree);
	if (guard.rule == ChordGuardRule::TritoneFifth) {
		return triad.fifth == kTritone;
	}
	return triad.fifth != kPerfectFifth || (triad.third != kMinorThird && triad.third != kMajorThird);
}

GEN_STEP_CODE int32_t cadenceTarget(const ChordGraph& graph, Scale scale, ChordGuard guard) {
	if (graph.cadenceTarget == kNoCadenceTarget) {
		return -1;
	}
	return chordGuarded(scale, graph.cadenceTarget, guard) ? dominant(scale, guard) : graph.cadenceTarget;
}

GEN_STEP_CODE ChordWeights chordWeights(const ChordGraph& graph, Scale scale, int32_t current, int32_t previous,
                                        const ChordPlace& place, ChordGuard guard, std::span<const ChordMove> colour,
                                        WalkRules rules) {
	ChordWeights out;
	int32_t* w = out.weight;
	const auto addMoves = [&](std::span<const ChordMove> moves) {
		for (const ChordMove& move : moves) {
			w[move.to] += move.from == current && move.to != current ? move.weight : 0;
		}
	};
	addMoves(graph.moves);
	addMoves(colour);
	if (current != kHomeChord) {
		w[kHomeChord] += graph.homePull;
	}
	w[current] += graph.repeatWeight;

	const int32_t target = cadenceTarget(graph, scale, guard);
	if (target >= 0) {
		w[target] += (place.cadence ? rules.cadenceBonus : 0) + (place.beforeDrop ? rules.dropCadenceBonus : 0);
	}

	// Thinned, never zeroed: a guarded chord the graph weighs stays a candidate, unless the factor is 0.
	for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
		if (chordGuarded(scale, degree, guard) && w[degree] > 0) {
			w[degree] = guard.factor > 0 ? std::max<int32_t>(w[degree] * guard.factor / 1000, 1) : 0;
		}
	}

	// The home bonuses don't stack: the larger counts. Each shrinks from full at Complexity 0 to a quarter at 50.
	int32_t bonus = 0;
	bonus = place.phraseFirst && rules.phraseHomeBonus > bonus ? rules.phraseHomeBonus : bonus;
	bonus = place.sectionStart && rules.sectionHomeBonus > bonus ? rules.sectionHomeBonus : bonus;
	const int32_t complexity = std::clamp<int32_t>(place.complexity, 0, kMacroFull);
	w[kHomeChord] += bonus * (4 * kMacroFull - 3 * complexity) / (4 * kMacroFull);

	if (place.phraseLast) {
		w[kHomeChord] = 0;
	}
	if (!graph.abaAllowed && previous >= 0 && previous != current) {
		w[previous] = 0;
	}
	return out;
}

GEN_STEP_CODE int32_t nextChord(const ChordGraph& graph, Scale scale, int32_t current, int32_t previous,
                                const ChordPlace& place, uint32_t draw, ChordGuard guard,
                                std::span<const ChordMove> colour, WalkRules rules) {
	if (place.home) {
		return kHomeChord;
	}
	const ChordWeights weights = chordWeights(graph, scale, current, previous, place, guard, colour, rules);
	int32_t total = 0;
	for (int32_t weight : weights.weight) {
		total += weight;
	}
	if (total <= 0) {
		if (!place.phraseLast || current != kHomeChord) {
			return current;
		}
		const int32_t target = cadenceTarget(graph, scale, guard);
		return target > kHomeChord ? target : dominant(scale, guard);
	}
	int32_t pick = static_cast<int32_t>((static_cast<uint64_t>(draw) * static_cast<uint32_t>(total)) >> 32);
	for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
		if (pick < weights.weight[degree]) {
			return degree;
		}
		pick -= weights.weight[degree];
	}
	return current;
}

} // namespace deluge::generative
