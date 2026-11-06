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

// The portable Perform driver (spec §2.5): it keeps the clock, decides each step 12 ticks ahead, drains the event
// buffer and emits to a NoteSink. Portable like the core, so the harness runs the real driver logic; the firmware
// adapter (generative/firmware/) plugs in a sink that plays the parts' Outputs.

#pragma once

#include "generative/core/event.h"
#include "generative/core/generator.h"
#include "generative/core/steering.h"
#include <cstdint>

namespace deluge::generative::perform {

/// Where Perform's events go: the firmware adapter's Outputs, or a harness recorder. Muted note-ons never arrive
/// (Perform drops them at the output, spec §12.5).
class NoteSink {
public:
	virtual ~NoteSink() = default;
	virtual void play(const Event& event) = 0;
};

/// Times the step decisions inside advance(), for H1 (spec §20). Called only when a decision is due.
class DecisionTimer {
public:
	virtual ~DecisionTimer() = default;
	virtual void decisionsBegin() = 0;
	virtual void decisionsEnd(int32_t decided) = 0;
};

class PerformDriver {
public:
	/// Starts driving `generator`, which start() has just reset: the clock is at step 0's grid line.
	void begin(Generator& generator);
	[[nodiscard]] bool running() const { return generator_ != nullptr; }

	/// Advances the clock by `songTicks` swung song ticks at tick magnitude `magnitude` (6 << magnitude song ticks
	/// per 16th), decides every step whose decision is due, then plays every event due, in order.
	/// @return Song ticks until the driver next needs a call, at least 1. Never early: an event plays at the first
	/// call whose clock has reached it (spec §12.3).
	int32_t advance(int32_t songTicks, int32_t magnitude, SteeringQueue& queue, SteeringLog* log, NoteSink& sink);

	/// Times the step decisions in each advance(); nullptr for none.
	void setDecisionTimer(DecisionTimer* timer) { timer_ = timer; }

	/// Stop, exit or a jump: flushes the Generator and plays its note-offs. The driver is idle afterwards.
	void stop(NoteSink& sink);

	/// Generator ticks since step 0's grid line.
	[[nodiscard]] int32_t clock() const { return clock_; }

private:
	/// Moves the clock by `songTicks` at `magnitude`, carrying any remainder. Never divides (spec §12.3).
	void moveClock(int32_t songTicks, int32_t magnitude);
	/// Decides every step whose decision tick the clock has reached.
	void decideDue(SteeringQueue& queue, SteeringLog* log);
	/// Plays every buffered event the clock has reached, in order.
	void playDue(NoteSink& sink);
	/// Song ticks at `magnitude` until the next decision or event, at least 1.
	[[nodiscard]] int32_t nextWait(int32_t magnitude) const;

	Generator* generator_ = nullptr;
	DecisionTimer* timer_ = nullptr;
	int32_t clock_ = 0;
	/// Song ticks not yet worth a whole Generator tick, when the song runs at a finer magnitude than 24 per 16th.
	int32_t remainder_ = 0;
};

} // namespace deluge::generative::perform
