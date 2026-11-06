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

#include "generative/perform/perform_driver.h"

namespace deluge::generative::perform {

namespace {

/// Song ticks per 16th are 6 << magnitude and the Generator's are 24 = 6 << 2, so converting is a shift by
/// 2 - magnitude: never a divide (spec §12.3).
constexpr int32_t kGeneratorMagnitude = 2;
static_assert((6 << kGeneratorMagnitude) == kTicksPerStep);

} // namespace

void PerformDriver::begin(Generator& generator) {
	generator_ = &generator;
	clock_ = 0;
	remainder_ = 0;
}

int32_t PerformDriver::advance(int32_t songTicks, int32_t magnitude, SteeringQueue& queue, SteeringLog* log,
                               NoteSink& sink) {
	if (generator_ == nullptr) {
		return INT32_MAX;
	}
	moveClock(songTicks, magnitude);
	decideDue(queue, log);
	playDue(sink);
	return nextWait(magnitude);
}

void PerformDriver::moveClock(int32_t songTicks, int32_t magnitude) {
	const int32_t shift = kGeneratorMagnitude - magnitude;
	if (shift >= 0) {
		clock_ += songTicks << shift;
	}
	else {
		int32_t total = remainder_ + songTicks;
		clock_ += total >> -shift;
		remainder_ = total & ((1 << -shift) - 1);
	}
}

int32_t PerformDriver::decideDue(SteeringQueue& queue, SteeringLog* log) {
	int32_t decided = 0;
	while (decisionTick(generator_->nextStep()) <= clock_) {
		generator_->decideNextStep(queue, log);
		decided++;
	}
	return decided;
}

int32_t PerformDriver::nextWait(int32_t magnitude) const {
	const int32_t shift = kGeneratorMagnitude - magnitude;
	int32_t nextDue = decisionTick(generator_->nextStep());
	int32_t nextEvent = generator_->nextEventTick();
	if (nextEvent < nextDue) {
		nextDue = nextEvent;
	}
	int32_t wait = nextDue - clock_; // > 0 once everything due has been decided and played
	int32_t songWait = shift >= 0 ? (wait + (1 << shift) - 1) >> shift : (wait << -shift) - remainder_;
	return songWait > 0 ? songWait : 1;
}

void PerformDriver::playDue(NoteSink& sink) {
	Event event;
	while (generator_->popDue(clock_, &event)) {
		if (event.kind == EventKind::NoteOn && event.muted) {
			continue;
		}
		sink.play(event);
	}
}

void PerformDriver::stop(NoteSink& sink) {
	if (generator_ == nullptr) {
		return;
	}
	generator_->flush(clock_);
	playDue(sink);
	generator_ = nullptr;
}

} // namespace deluge::generative::perform
