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

/// How much a note-on is worth keeping when a step overflows: higher keeps. Ornament hits go first, then parts in
/// reverse role priority (spec §2.7).
constexpr int32_t keepPriority(const Event& noteOn, Role role) {
	int32_t byRole = kNumRoles - 1 - voiceStartRank(role); // Drums highest, FX lowest
	return noteOn.ornament ? byRole : kNumRoles + byRole;
}
/// Every other event: never dropped while a note-on can make room.
constexpr int32_t kNeverDropped = 2 * kNumRoles;

/// What a push did. `dropped` is set when the buffer was full and an event didn't make it: a lower-ranked note-on
/// that made room, or the pushed note-on itself (then `stored` is false).
struct PushResult {
	bool stored = true;
	bool dropped = false;
	Event droppedEvent;
};

template <int32_t Capacity>
class EventBuffer {
public:
	void clear() {
		count_ = 0;
		stepStart_ = 0;
	}
	[[nodiscard]] int32_t size() const { return count_; }
	[[nodiscard]] static constexpr int32_t capacity() { return Capacity; }

	/// Marks the start of one step decision. A note-on only ever displaces note-ons of the step being decided.
	void beginStep() { stepStart_ = count_; }

	/// Adds a note-on of the step being decided. When the buffer is full, the lowest-priority note-on of this step
	/// that ranks below it makes room (the latest pushed of equals goes first); else it is the one dropped.
	PushResult pushNoteOn(const Event& event, Role role) {
		int32_t keep = keepPriority(event, role);
		if (count_ < Capacity) {
			append(event, keep);
			return {};
		}
		int32_t victim = lowestNoteOn(stepStart_, keep);
		if (victim < 0) {
			return {false, true, event};
		}
		PushResult result{true, true, events_[victim]};
		removeAt(victim);
		append(event, keep);
		return result;
	}

	/// Adds anything but a note-on: a note-off, Section, KeyScale or Control event. Never refused while the buffer
	/// holds a note-on: when full, the lowest-ranked note-on anywhere in it makes room. The buffer's size keeps a
	/// buffer full of other events impossible (generator.h); if it happens anyway, the event is dropped.
	PushResult push(const Event& event) {
		if (count_ < Capacity) {
			append(event, kNeverDropped);
			return {};
		}
		int32_t victim = lowestNoteOn(0, INT32_MAX);
		if (victim < 0) {
			return {false, true, event};
		}
		PushResult result{true, true, events_[victim]};
		removeAt(victim);
		append(event, kNeverDropped);
		return result;
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
	/// KeyScale, Control, NoteOff, NoteOn), note-ons in voice-start order (role priority, Ornament hits last, spec
	/// §12.6), then in the order the events were decided.
	bool popDue(int32_t now, Event* out) {
		int32_t best = -1;
		for (int32_t i = 0; i < count_; i++) {
			const Event& e = events_[i];
			if (e.tick > now) {
				continue;
			}
			if (best < 0 || e.tick < events_[best].tick) {
				best = i;
				continue;
			}
			const Event& b = events_[best];
			if (e.tick == b.tick && (e.kind < b.kind || (e.kind == b.kind && keep_[i] > keep_[best]))) {
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
	void append(const Event& event, int32_t keep) {
		events_[count_] = event;
		keep_[count_] = static_cast<int8_t>(keep);
		count_++;
	}
	/// The note-on from index `from` on ranking lowest and below `below`, the latest of equals; -1 if none.
	[[nodiscard]] int32_t lowestNoteOn(int32_t from, int32_t below) const {
		int32_t victim = -1;
		for (int32_t i = from; i < count_; i++) {
			if (events_[i].kind == EventKind::NoteOn && keep_[i] < below && (victim < 0 || keep_[i] <= keep_[victim])) {
				victim = i;
			}
		}
		return victim;
	}

	Event events_[Capacity];
	int8_t keep_[Capacity];
	int32_t count_ = 0;
	int32_t stepStart_ = 0;
};

} // namespace deluge::generative
