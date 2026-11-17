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

// Test builds: the Perform adapter's timing and memory reports, as test events: H1 (the step decisions in the tick),
// H2 and H16 (Control events in the tick, the render, voices culled) and gen_mem (spec §20, §13.8).

#pragma once

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "generative/perform/perform_driver.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <cstdint>

namespace deluge::generative::firmware {

/// Nanoseconds in a span of getSystemTicks() (33.33 MHz).
unsigned long ticksToNs(uint32_t ticks);
/// Free bytes on the heaps Perform uses, as a gen_mem event tagged `when`.
void reportFreeMemory(const char* when);
/// Milliseconds since a getSystemTicks() reading, for the jobs' timing events.
unsigned long msSince(uint32_t start);

/// H1 (spec §20): the cost of the step decisions in the tick. Reported as a test event every 4 bars, then reset.
class DecisionTiming final : public perform::DecisionTimer {
public:
	void decisionsBegin() override { started_ = getSystemTicks(); }
	void decisionsEnd(int32_t decided) override {
		uint32_t counts = getSystemTicks() - started_;
		steps_ += decided;
		totalCounts_ += counts;
		uint32_t perStep = counts / decided;
		maxCounts_ = perStep > maxCounts_ ? perStep : maxCounts_;
		if (steps_ >= 4 * kStepsPerBar) {
			TEST_EVENT("gen_h1 steps=%lu mean_ns=%lu max_ns=%lu", static_cast<unsigned long>(steps_),
			           ticksToNs(totalCounts_ / steps_), ticksToNs(maxCounts_));
			steps_ = totalCounts_ = maxCounts_ = 0;
		}
	}

private:
	uint32_t started_ = 0;
	uint32_t steps_ = 0;
	uint32_t totalCounts_ = 0;
	uint32_t maxCounts_ = 0;
};
extern DecisionTiming decisionTiming;

/// H2 (spec §20): what applying Control events costs in the tick, per step that sends any. The ramps' cost in the
/// render is TestPort::renderTiming.
class ControlTiming {
public:
	void begin() { started_ = getSystemTicks(); }
	void end(int32_t tick) {
		const uint32_t counts = getSystemTicks() - started_;
		if (!stepOpen_ || tick != stepTick_) {
			closeStep(); // the events of one step share its grid line's tick
			stepTick_ = tick;
		}
		stepCounts_ += counts;
		stepOpen_ = true;
	}
	/// Reports and resets, with the render timing since the last report.
	void report(const char* ramps);
	void reset();

private:
	void closeStep() {
		if (!stepOpen_) {
			return;
		}
		stepsSending_++;
		totalCounts_ += stepCounts_;
		maxCounts_ = std::max(maxCounts_, stepCounts_);
		stepCounts_ = 0;
		stepOpen_ = false;
	}
	uint32_t started_ = 0;
	int32_t stepTick_ = 0;
	bool stepOpen_ = false;
	uint32_t stepCounts_ = 0;
	uint32_t stepsSending_ = 0;
	uint32_t totalCounts_ = 0;
	uint32_t maxCounts_ = 0;
};
extern ControlTiming controlTiming;

} // namespace deluge::generative::firmware

#endif // ENABLE_SYSEX_TEST
