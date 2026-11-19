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

#include "generative/compose/song_size.h"
#include "generative/compose/compose_driver.h"
#include "generative/core/capacity.h"
#include "generative/core/motion_curve.h"
#include <algorithm>

namespace deluge::generative::compose {

namespace {

constexpr int32_t ceilDiv(int32_t a, int32_t b) {
	return (a + b - 1) / b;
}

/// The most phrases a Groove plays at `motion` (spec §9.3): its longest base over the Groove curve's multiple. One at
/// Motion 0, where Compose ends it on its first phrase line past the target.
int32_t longestGroovePhrases(const ArcSpec& arc, int32_t motion) {
	const int32_t multiple = motionMultiple(curves::grooveLength, motion);
	if (multiple <= 0) {
		return 1;
	}
	return static_cast<int32_t>((int64_t{arc.groovePhrases.high} * kCurveOne + multiple - 1) / multiple);
}

int32_t longestBuildBars(const ArcSpec& arc) {
	int32_t bars = 0;
	for (const WeightedBars& build : arc.buildBars) {
		bars = std::max<int32_t>(bars, build.bars);
	}
	return bars;
}

/// Notes the roster can start in one step, and automation lanes its sets can move.
struct PerStep {
	int32_t notes = 0;
	int32_t lanes = 0;
};

PerStep perStep(const Genre& genre) {
	PerStep out;
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		out.notes += part.synth() ? onsetNotes(part.role) : laneCount(part);
		for (int32_t set = 0; set < controlSetsOf(part); set++) {
			out.lanes += controlsMovedBy(controlSetKind(part, set));
			out.lanes += throwsDelay(part, controlSetLane(part, set)) ? 1 : 0; // the Throw's delay lift
		}
	}
	return out;
}

} // namespace

int32_t maxBars(const Recipe& recipe, int32_t targetSteps) {
	const Genre& genre = *recipe.genre;
	const ArcSpec& arc = genre.arc;
	const int32_t phraseBars = genre.phraseBars;
	const int32_t targetPhrases = ceilDiv(ceilDiv(std::max<int32_t>(targetSteps, 1), kStepsPerBar), phraseBars);
	const int32_t phrases = targetPhrases + longestGroovePhrases(arc, recipe.macros[Macro::Motion])
	                        + arc.breakPhrases.high + arc.outroPhrases;
	return phrases * phraseBars + longestBuildBars(arc);
}

int32_t expectedBars(const Recipe& recipe, int32_t targetSteps) {
	const Genre& genre = *recipe.genre;
	return ceilDiv(std::max<int32_t>(targetSteps, 1), kStepsPerBar) + genre.arc.outroPhrases * genre.phraseBars;
}

int64_t estimateBytes(const Recipe& recipe, int32_t lengthSeconds) {
	const Genre& genre = *recipe.genre;
	int64_t bytes = kSongCosts.song;
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		bytes += part.synth() ? kSongCosts.synthClip : int64_t{laneCount(part)} * kSongCosts.kitRow;
	}
	const PerStep step = perStep(genre);
	const int64_t perBar = int64_t{step.notes} * kStepsPerBar * kSongCosts.note + int64_t{step.lanes} * kSongCosts.node;
	return bytes + perBar * maxBars(recipe, targetSteps(lengthSeconds, recipe.tempo));
}

int32_t longestFitting(const Recipe& recipe, int64_t bytes) {
	for (int32_t seconds = kMaxLengthSeconds; seconds >= kMinLengthSeconds; seconds -= kLengthStepSeconds) {
		if (estimateBytes(recipe, seconds) <= bytes) {
			return seconds;
		}
	}
	return 0;
}

} // namespace deluge::generative::compose
