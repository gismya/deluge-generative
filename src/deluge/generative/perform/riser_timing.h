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

// A sample Riser ends on the Drop (spec §9.5, §10.5): where its note starts, from the sample's length and the tempo
// planned across the Build. Driver-side, like tempo (spec §2.2): the core never sees it.

#pragma once

#include "generative/core/types.h"
#include <algorithm>
#include <cstdint>

namespace deluge::generative::perform {

/// Microseconds one 16th lasts at a tempo of 1 hundredth of a BPM: 60 s × 100 / 4 16ths a beat.
constexpr int32_t kStepMicrosAtOneHundredth = 1'500'000'000;

/// How long one 16th lasts at `tempo` (hundredths of a BPM), in microseconds.
constexpr int32_t stepMicros(int32_t tempo) {
	return kStepMicrosAtOneHundredth / std::max<int32_t>(tempo, 1);
}

/// The tick a sample `micros` long starts on so that it ends on `dropTick`, at the tempo `tempoAt(tick)` plans for each
/// tick; never before `earliest`, the note's own tick, so a sample longer than that starts there. Walks back from the
/// Drop a beat at a time, at the tempo planned for the beat's middle: a planned glide moves slowly enough that this is
/// within a millisecond or so, and outside a glide it is exact. Integer math; one divide a beat.
template <typename TempoAt>
int32_t riserStart(int32_t earliest, int32_t dropTick, int32_t micros, TempoAt tempoAt) {
	constexpr int32_t kBeatSteps = kStepsPerBar / 4;
	constexpr int32_t kBeatTicks = kBeatSteps * kTicksPerStep;
	int32_t left = micros;
	for (int32_t line = dropTick; line > earliest; line -= kBeatTicks) {
		const int32_t beat = stepMicros(tempoAt(line - kBeatTicks / 2)) * kBeatSteps;
		if (beat >= left) {
			const int64_t into = (int64_t{left} * kBeatTicks + beat / 2) / beat;
			return std::max<int32_t>(earliest, line - static_cast<int32_t>(into));
		}
		left -= beat;
	}
	return earliest; // longer than the Build: it starts with it, as a synth Riser does (M2 starts it partway in)
}

} // namespace deluge::generative::perform
