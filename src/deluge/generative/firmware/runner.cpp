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

#include "generative/firmware/runner.h"
#include "OSLikeStuff/scheduler_api.h"

namespace deluge::generative::firmware {

namespace {

/// About 2 ms of work between yields, in getSystemTicks() units (spec §12.10).
constexpr uint32_t kChunkBudget = DELUGE_CLOCKS_PER / 500;
/// A dynamic task's slot in the medium band of the scheduler's table (deluge.cpp).
constexpr uint8_t kJobPriority = 20;

class SchedulerHost final : public RunnerHost {
public:
	bool startTask() override {
		// Never inside the card routine: a job does its own card reads (spec §10.7).
		return addOnceTask([] { runner.runActive(); }, kJobPriority, 0, "generative job",
		                   RESOURCE_SD | RESOURCE_SD_ROUTINE)
		       >= 0;
	}
	void yield() override {
		// Runs every task that is due (audio, the tick, buttons, encoders, MIDI and the PIC), then comes back. Not
		// yield(), which runs only one task: in a build of a few chunks, buttons might never get a turn, and BACK
		// would wait for the end of the build.
		yieldToIdle([] { return false; });
	}
	uint32_t now() override { return getSystemTicks(); }
};

SchedulerHost schedulerHost;

} // namespace

Runner runner{schedulerHost, kChunkBudget};

} // namespace deluge::generative::firmware
