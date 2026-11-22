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

#if ENABLE_SYSEX_TEST

#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/capture_files.h"
#include "generative/firmware/free_memory.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/sound_controls.h"
#include "io/midi/test_port_render.h"
#include "processing/engines/audio_engine.h"

namespace deluge::generative::firmware {

unsigned long ticksToNs(uint32_t ticks) {
	return static_cast<unsigned long>(uint64_t{ticks} * 1'000'000'000 / DELUGE_CLOCKS_PER);
}

constexpr uint32_t kCountsPerUs = DELUGE_CLOCKS_PER / 1'000'000;

DecisionTiming decisionTiming;
ControlTiming controlTiming;
GraphicsTiming graphicsTiming;

void GraphicsTiming::report() {
	if (frames_ == 0) {
		return;
	}
	TEST_EVENT("gen_graphics frames=%lu mean_us=%lu max_us=%lu settings_max_us=%lu",
	           static_cast<unsigned long>(frames_), static_cast<unsigned long>((totalCounts_ / frames_) / kCountsPerUs),
	           static_cast<unsigned long>(maxCounts_ / kCountsPerUs),
	           static_cast<unsigned long>(settingsMaxCounts_ / kCountsPerUs));
	reset();
}

void ControlTiming::report(const char* ramps) {
	closeStep();
	cardLog.report(); // the card log's window and the streaming counters, before the reset below (#448)
	graphicsTiming.report();
	TestPort::RenderTiming render = TestPort::renderTiming;
	TestPort::renderTiming = {};
	const auto renderMean =
	    static_cast<uint32_t>(render.samples > 0 ? uint64_t{render.counts} * 128 / render.samples : 0);
	TEST_EVENT("gen_h2 ramps=%s moved=%s steps=%lu params=%lu mean_ns=%lu max_ns=%lu render_ns=%lu "
	           "render_max_ns=%lu",
	           ramps, stepsSending_ > 0 ? "yes" : "no", static_cast<unsigned long>(stepsSending_),
	           static_cast<unsigned long>(soundControls.takeParamsMoved()),
	           stepsSending_ > 0 ? ticksToNs(totalCounts_ / stepsSending_) : 0UL, ticksToNs(maxCounts_),
	           ticksToNs(renderMean), ticksToNs(render.maxCounts));
	// H16 (spec §20): voices culled for CPU in the same 4 bars, the part of the last one, the voices sounding now,
	// the slowest render.
	TEST_EVENT("gen_h16 culls=%lu culled=%s voices=%ld render_max_us=%lu", static_cast<unsigned long>(render.culls),
	           render.culls == 0 ? "-"
	           : render.lastCulled == nullptr
	               ? "freed"
	               : performAdapter.output().partName(render.lastCulled, performAdapter.steering().rosterGenre()),
	           static_cast<long>(AudioEngine::getNumVoices()), ticksToNs(render.maxCounts) / 1000);
	// The audio output's margin (spec §20, #449): the routine's entries, those after the TX ring ran dry, the least
	// audio left at one, entries with under 0.5, 1 and 2 ms left and more, and the DMA's largest move off the timer (in
	// samples; near 0 says the margins are real time).
	const long marginMinUs =
	    render.routineCalls == 0 ? 0L : render.marginMinCounts / static_cast<int32_t>(kCountsPerUs);
	TEST_EVENT("gen_audio calls=%lu underruns=%lu margin_min_us=%ld margins=%lu/%lu/%lu/%lu drift=%ld",
	           static_cast<unsigned long>(render.routineCalls), static_cast<unsigned long>(render.underruns),
	           marginMinUs, static_cast<unsigned long>(render.margins[0]),
	           static_cast<unsigned long>(render.margins[1]), static_cast<unsigned long>(render.margins[2]),
	           static_cast<unsigned long>(render.margins[3]), static_cast<long>(render.dmaDriftMax));
	// Each underrun's culprit: the task the routine ran inside, or else the one that finished just before it (with its
	// longest run then)
	for (const TestPort::RenderTiming::LateTask& late : render.lateTasks) {
		if (late.name != nullptr) {
			TEST_EVENT("gen_audio_late task=%s %s underruns=%lu run_max_us=%lu", late.name,
			           late.nested ? "inside" : "before", static_cast<unsigned long>(late.underruns),
			           static_cast<unsigned long>(late.runMaxCounts / kCountsPerUs));
		}
	}
	// The UI timers whose handler ran over 2 ms (TimerName's value), how often, and their longest run
	for (int32_t timer = 0; timer < TestPort::RenderTiming::kUiTimers; timer++) {
		if (render.uiTimerSlow[timer] > 0) {
			TEST_EVENT("gen_audio_ui_timer timer=%ld over_2ms=%lu run_max_us=%lu", static_cast<long>(timer),
			           static_cast<unsigned long>(render.uiTimerSlow[timer]),
			           static_cast<unsigned long>(render.uiTimerMaxCounts[timer] / kCountsPerUs));
		}
	}
	stepsSending_ = totalCounts_ = maxCounts_ = 0;
}

void ControlTiming::reset() {
	stepOpen_ = false;
	stepCounts_ = stepsSending_ = totalCounts_ = maxCounts_ = 0;
	soundControls.takeParamsMoved();
	graphicsTiming.reset();
	TestPort::renderTiming = {};
}

/// Free bytes on the heaps Perform uses, for the leave check (spec §13.8: memory after leaving matches memory before
/// entry) and a sound re-roll's. The stealable region holds the sample cache, so it's reported apart.
void reportFreeMemory(const char* when) {
	auto freeIn = [](int32_t region) { return static_cast<unsigned long>(freeInRegion(region)); };
	TEST_EVENT("gen_mem %s internal=%lu external=%lu stealable=%lu", when, static_cast<unsigned long>(freeInternal()),
	           freeIn(MEMORY_REGION_EXTERNAL) + freeIn(MEMORY_REGION_EXTERNAL_SMALL), freeIn(MEMORY_REGION_STEALABLE));
}

unsigned long msSince(uint32_t start) {
	return static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000));
}

} // namespace deluge::generative::firmware

#endif // ENABLE_SYSEX_TEST
