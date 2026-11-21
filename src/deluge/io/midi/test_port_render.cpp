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

#include "io/midi/test_port_render.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "io/midi/midi_queue.h"
#include "io/midi/output_margin.h"
#include <algorithm>

namespace TestPort {

RenderTiming renderTiming;

namespace {
deluge::io::midi::OutputMargin outputMargin{kSystemTicksPerSampleQ8};
uint32_t lastRenderStart = 0;
constexpr uint32_t kCountsPerUs = DELUGE_CLOCKS_PER / 1'000'000;
} // namespace

void recordRender(uint32_t start, size_t numSamples) {
	if (numSamples == 0) {
		return;
	}
	const uint32_t counts = getSystemTicks() - start;
	renderTiming.counts += counts;
	renderTiming.samples += numSamples;
	renderTiming.maxCounts = std::max(renderTiming.maxCounts, counts);
	// The spike's measure (#447): the span between render starts, against the TX ring's 128 samples (2.9 ms)
	if (lastRenderStart != 0) {
		const uint32_t gap = start - lastRenderStart;
		renderTiming.renderGapMaxCounts = std::max(renderTiming.renderGapMaxCounts, gap);
		const uint32_t us = gap / kCountsPerUs;
		renderTiming.renderGaps[us < 1000 ? 0 : us < 2000 ? 1 : us < 2902 ? 2 : us < 5000 ? 3 : us < 10000 ? 4 : 5]++;
	}
	lastRenderStart = start;
}

void audioRoutineEntered() {
	if (!outputMargin.primed()) {
		return;
	}
	const int32_t margin = outputMargin.entered(getSystemTicks());
	renderTiming.routineCalls++;
	renderTiming.marginMinCounts = std::min(renderTiming.marginMinCounts, margin);
	if (margin < 0) {
		renderTiming.underruns++;
		return;
	}
	const uint32_t us = static_cast<uint32_t>(margin) / kCountsPerUs;
	renderTiming.margins[us < 500 ? 0 : us < 1000 ? 1 : us < 2000 ? 2 : 3]++;
}

void audioRoutineLeft(uint32_t queuedSamples) {
	outputMargin.left(getSystemTicks(), queuedSamples);
}

void voiceCulled(const Sound& sound) {
	renderTiming.culls++;
	renderTiming.lastCulled = &sound;
}

} // namespace TestPort

#endif
