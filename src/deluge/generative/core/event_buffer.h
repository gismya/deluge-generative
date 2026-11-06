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

// The fixed event buffer (spec §2.7, §12.2): events decided but not yet played. It holds the step being decided as
// well as the step still playing, and is sized for that by construction (generator.h). If a step overflows anyway,
// the same events are dropped in every driver: Ornament hits first, then note-ons by reverse role priority.

#pragma once

#include "generative/core/event.h"
#include <cstdint>

namespace deluge::generative {

/// How much an event is worth keeping when a step overflows. Higher keeps. Only note-ons can be dropped.
constexpr int32_t keepPriority(const Event& event, Role role) {
	if (event.kind != EventKind::NoteOn) {
		return 100;
	}
	int32_t byRole = kNumRoles - 1 - voiceStartRank(role); // Drums highest, FX lowest
	return event.ornament ? byRole : kNumRoles + byRole;
}

template <int32_t Capacity>
class EventBuffer {
public:
	void clear() {
		count_ = 0;
		stepStart_ = 0;
	}
	[[nodiscard]] int32_t size() const { return count_; }
	[[nodiscard]] static constexpr int32_t capacity() { return Capacity; }

	/// Marks the start of one step decision. Overflow only ever drops events of the step being decided.
	void beginStep() { stepStart_ = count_; }

	/// Adds an event of the step being decided. When the buffer is full, the lowest-priority note-on of this step
	/// that ranks below `event` makes room (the latest pushed of equals goes first), else `event` itself is refused.
	/// @param dropped Set to the event that didn't make it, if any.
	/// @return Whether something was dropped.
	bool push(const Event& event, Role role, Event* dropped) {
		if (count_ < Capacity) {
			events_[count_] = event;
			keep_[count_] = static_cast<int8_t>(keepPriority(event, role));
			count_++;
			return false;
		}
		int32_t incoming = keepPriority(event, role);
		int32_t victim = -1;
		for (int32_t i = stepStart_; i < count_; i++) {
			if (keep_[i] < incoming && (victim < 0 || keep_[i] <= keep_[victim])) {
				victim = i;
			}
		}
		if (victim < 0) {
			*dropped = event;
			return true;
		}
		*dropped = events_[victim];
		removeAt(victim);
		events_[count_] = event;
		keep_[count_] = static_cast<int8_t>(incoming);
		count_++;
		return true;
	}

	/// The tick of the earliest event, or INT32_MAX when empty.
	[[nodiscard]] int32_t nextTick() const {
		int32_t best = INT32_MAX;
		for (int32_t i = 0; i < count_; i++) {
			if (events_[i].tick < best) {
				best = events_[i].tick;
			}
		}
		return best;
	}

	/// Takes the next event in play order if its tick is at or before `now`: earliest tick, then by kind (Section,
	/// KeyScale, Control, NoteOff, NoteOn), then in the order the events were decided.
	bool popDue(int32_t now, Event* out) {
		int32_t best = -1;
		for (int32_t i = 0; i < count_; i++) {
			const Event& e = events_[i];
			if (e.tick > now) {
				continue;
			}
			if (best < 0 || e.tick < events_[best].tick
			    || (e.tick == events_[best].tick && e.kind < events_[best].kind)) {
				best = i;
			}
		}
		if (best < 0) {
			return false;
		}
		*out = events_[best];
		removeAt(best);
		return true;
	}

	// ---- For the Generator's note-off bookkeeping ----

	[[nodiscard]] const Event& at(int32_t i) const { return events_[i]; }
	Event& at(int32_t i) { return events_[i]; }
	void removeAt(int32_t i) {
		for (int32_t j = i + 1; j < count_; j++) {
			events_[j - 1] = events_[j];
			keep_[j - 1] = keep_[j];
		}
		count_--;
		if (i < stepStart_) {
			stepStart_--;
		}
	}

private:
	Event events_[Capacity];
	int8_t keep_[Capacity];
	int32_t count_ = 0;
	int32_t stepStart_ = 0;
};

} // namespace deluge::generative
