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

// STUB: a placeholder so drivers can be built against the interface. It plays a kick on every beat, a closed
// hat on every off-beat 8th and a bass root on every off-beat 8th. The real core replaces this file.

#include "generative/core/generator.h"

namespace deluge::generative {

Generator::Generator() = default;

void Generator::start(const Recipe& recipe, uint32_t seed) {
	recipe_ = recipe;
	seed_ = seed;
	nextStepToDecide_ = 0;
	currentStep_ = -1;
	numEvents_ = 0;
}

void Generator::setRecipe(const Recipe& recipe) {
	recipe_ = recipe;
}

namespace {
void push(Generator::Event* events, int32_t& n, Generator::Event e) {
	if (n < Generator::kMaxEvents) {
		events[n++] = e;
	}
}
} // namespace

int32_t Generator::tick(int32_t now, EventSink& sink) {
	while (nextStepToDecide_ * kTicksPerStep - kLookaheadTicks <= now) {
		int32_t step = nextStepToDecide_++;
		int32_t t = step * kTicksPerStep;
		uint8_t vel = static_cast<uint8_t>(70 + recipe_.energy);
		if (step % 4 == 0) {
			push(events_, numEvents_, {t, PartId::Drums, static_cast<uint8_t>(Lane::Kick), 0, vel});
			push(events_, numEvents_, {t + 6, PartId::Drums, static_cast<uint8_t>(Lane::Kick), 0, 0});
		}
		if (step % 4 == 2) {
			push(events_, numEvents_, {t, PartId::Drums, static_cast<uint8_t>(Lane::ClosedHat), 0, vel});
			push(events_, numEvents_, {t + 6, PartId::Drums, static_cast<uint8_t>(Lane::ClosedHat), 0, 0});
			int16_t note = static_cast<int16_t>(36 + recipe_.key);
			push(events_, numEvents_, {t, PartId::Bass, 0, note, vel});
			push(events_, numEvents_, {t + 12, PartId::Bass, 0, note, 0});
		}
	}

	// Emit due events: note-offs first, then note-ons, each in buffer order (already role-priority order).
	for (int32_t pass = 0; pass < 2; pass++) {
		for (int32_t i = 0; i < numEvents_; i++) {
			Event& e = events_[i];
			if (e.tick > now) {
				continue;
			}
			bool isOff = e.velocity == 0;
			if (pass == 0 && isOff) {
				sink.noteOff(now, e.part, e.lane, e.note);
			}
			else if (pass == 1 && !isOff) {
				sink.noteOn(now, e.part, e.lane, e.note, e.velocity);
				currentStep_ = e.tick / kTicksPerStep;
			}
		}
	}
	int32_t kept = 0;
	int32_t next = nextStepToDecide_ * kTicksPerStep - kLookaheadTicks;
	for (int32_t i = 0; i < numEvents_; i++) {
		if (events_[i].tick > now) {
			if (events_[i].tick < next) {
				next = events_[i].tick;
			}
			events_[kept++] = events_[i];
		}
	}
	numEvents_ = kept;
	int32_t wait = next - now;
	return wait < 1 ? 1 : wait;
}

void Generator::stop(EventSink& sink) {
	for (int32_t i = 0; i < numEvents_; i++) {
		if (events_[i].velocity == 0) {
			sink.noteOff(events_[i].tick, events_[i].part, events_[i].lane, events_[i].note);
		}
	}
	numEvents_ = 0;
}

} // namespace deluge::generative
