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
#include "definitions.h"
#include "io/midi/midi_queue.h"
#include "io/midi/output_margin.h"
#include <algorithm>
#include <cstdlib>

namespace TestPort {

RenderTiming renderTiming;

namespace {
deluge::io::midi::OutputMargin outputMargin{kSystemTicksPerSampleQ8};
constexpr uint32_t kCountsPerUs = DELUGE_CLOCKS_PER / 1'000'000;
const char* lastTaskName = nullptr;      ///< The task that finished last
uint32_t lastTaskCounts = 0;             ///< and its run
const char* runningTask = nullptr;       ///< The innermost task running
const char* callerOfLastStart = nullptr; ///< The task running when the last one started: the audio routine's caller

/// An underrun: blame the task that called the routine, or else the one that finished just before it. Once the named
/// slots are taken, the last slot counts the rest as task "other".
void blameTask() {
	bool nested = callerOfLastStart != nullptr;
	const char* name = nested ? callerOfLastStart : lastTaskName != nullptr ? lastTaskName : "?";
	RenderTiming::LateTask* slot = &renderTiming.lateTasks[RenderTiming::kNamedLateTasks];
	for (int32_t i = 0; i < RenderTiming::kNamedLateTasks; i++) {
		RenderTiming::LateTask& late = renderTiming.lateTasks[i];
		if (late.name == nullptr || (late.name == name && late.nested == nested)) {
			slot = &late;
			break;
		}
	}
	if (slot == &renderTiming.lateTasks[RenderTiming::kNamedLateTasks]) {
		name = "other";
		nested = false;
	}
	slot->name = name;
	slot->nested = nested;
	slot->underruns++;
	if (!nested) {
		slot->runMaxCounts = std::max(slot->runMaxCounts, lastTaskCounts);
	}
}
} // namespace

void recordRender(uint32_t start, size_t numSamples) {
	if (numSamples == 0) {
		return;
	}
	const uint32_t counts = getSystemTicks() - start;
	renderTiming.counts += counts;
	renderTiming.samples += numSamples;
	renderTiming.maxCounts = std::max(renderTiming.maxCounts, counts);
}

void audioRoutineEntered(uint32_t dmaFrame) {
	if (!outputMargin.primed()) {
		return;
	}
	const uint32_t now = getSystemTicks();
	const int32_t margin = outputMargin.entered(now);
	const int32_t drift = outputMargin.dmaDrift(now, dmaFrame, SSI_TX_BUFFER_NUM_SAMPLES);
	if (std::abs(drift) > std::abs(renderTiming.dmaDriftMax)) {
		renderTiming.dmaDriftMax = drift;
	}
	renderTiming.routineCalls++;
	renderTiming.marginMinCounts = std::min(renderTiming.marginMinCounts, margin);
	if (margin < 0) {
		renderTiming.underruns++;
		blameTask();
		return;
	}
	const uint32_t us = static_cast<uint32_t>(margin) / kCountsPerUs;
	renderTiming.margins[us < 500 ? 0 : us < 1000 ? 1 : us < 2000 ? 2 : 3]++;
}

void audioRoutineLeft(uint32_t queuedSamples, uint32_t dmaFrame) {
	outputMargin.left(getSystemTicks(), queuedSamples, dmaFrame);
}

const char* taskStarted(const char* name) {
	callerOfLastStart = runningTask;
	runningTask = name;
	return callerOfLastStart;
}

void taskRan(const char* name, uint32_t counts, const char* caller) {
	lastTaskName = name;
	lastTaskCounts = counts;
	runningTask = caller;
}

void uiTimerRan(int32_t timer, uint32_t counts) {
	if (timer < 0 || timer >= RenderTiming::kUiTimers) {
		return;
	}
	renderTiming.uiTimerMaxCounts[timer] = std::max(renderTiming.uiTimerMaxCounts[timer], counts);
	if (counts > 2 * 1000 * kCountsPerUs) {
		renderTiming.uiTimerSlow[timer]++;
	}
}

void voiceCulled(const Sound& sound) {
	renderTiming.culls++;
	renderTiming.lastCulled = &sound;
}

} // namespace TestPort

#endif
