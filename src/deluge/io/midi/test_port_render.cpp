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
#include <algorithm>

namespace TestPort {

RenderTiming renderTiming;

void recordRender(uint32_t start, size_t numSamples) {
	if (numSamples == 0) {
		return;
	}
	const uint32_t counts = getSystemTicks() - start;
	renderTiming.counts += counts;
	renderTiming.samples += numSamples;
	renderTiming.maxCounts = std::max(renderTiming.maxCounts, counts);
	const uint32_t us = counts / (DELUGE_CLOCKS_PER / 1'000'000);
	renderTiming.hist[std::min<uint32_t>(us / 250, 6)]++;
	static uint32_t lastStart = 0;
	if (lastStart != 0) {
		renderTiming.gapMaxCounts = std::max(renderTiming.gapMaxCounts, start - lastStart);
	}
	lastStart = start;
}

void recordDrained(size_t drained) {
	renderTiming.drainedMax = std::max<uint32_t>(renderTiming.drainedMax, drained);
}

void clusterMissed(bool playing) {
	(playing ? renderTiming.lateClusters : renderTiming.unloadedStarts)++;
}

void voiceCulled(const Sound& sound) {
	renderTiming.culls++;
	renderTiming.lastCulled = &sound;
}

} // namespace TestPort

#endif
