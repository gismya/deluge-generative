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

// The sounding-note table (spec §2.7, §12.5). Notes in the stream carry no length: the core remembers when each one
// should end and sends every note-off itself. A note stays here from its decision until its note-off is queued.

#pragma once

#include "generative/core/event.h"
#include "generative/core/types.h"
#include <algorithm>
#include <cstdint>
#include <span>

namespace deluge::generative {

/// The notes a part of `role` may have sounding (or held back) at once, between decision and note-off: a Kit part one
/// per lane of its kind (a new hit on a lane ends the old one), a pitched part an onset's notes plus the next onset's
/// overlapping them (a slide). Its table holds no more, and the event buffer is sized on it (generator.h).
constexpr int32_t maxSounding(Role role) {
	return maxLanes(role) > 0 ? onsetNotes(role) : 2 * onsetNotes(role);
}

/// The notes a roster may have sounding at once: each part's maxSounding().
constexpr int32_t soundingOf(std::span<const Role> roster) {
	int32_t notes = 0;
	for (Role role : roster) {
		notes += maxSounding(role);
	}
	return notes;
}

struct SoundingNote {
	int32_t onTick = 0;
	int32_t offTick = 0;
	/// Its velocity before Energy's layer: what a late entry re-strikes it with, under the Energy then (spec §5.6).
	int16_t baseVelocity = 0;
	Lane lane = Lane::None;
	uint8_t pitch : 7 = 0; ///< A MIDI note, 0–127.
	bool used : 1 = false;

	[[nodiscard]] Event noteOff(uint8_t part) const { return Event::noteOff(offTick, part, lane, pitch); }
	[[nodiscard]] bool is(const Event& e) const {
		return used && lane == e.lane && (lane != Lane::None || pitch == e.pitch);
	}
};
static_assert(sizeof(SoundingNote) == 12, "two tables of notes in each Generator block");

class SoundingNotes {
public:
	/// The notes the largest roster may have (kLargestRoster). Each part gets its role's share of this one pool, not
	/// the largest role's; genre_data_spec holds every genre's roster to it.
	static constexpr int32_t kTotal = soundingOf(kLargestRoster);
	static_assert(kTotal <= UINT8_MAX);

	/// Until layout(), each part has an even share of the pool.
	SoundingNotes() {
		for (int32_t part = 0; part <= kMaxParts; part++) {
			first_[part] = static_cast<uint8_t>(part * kTotal / kMaxParts);
		}
	}

	/// Lays out the parts' tables in the pool, emptied: part p holds `slots[p]` notes, maxSounding() of its role. A
	/// roster needing more than kTotal (bad data: genre_data_spec rejects it) gets what is left.
	void layout(const int32_t (&slots)[kMaxParts]) {
		int32_t next = 0;
		for (int32_t part = 0; part < kMaxParts; part++) {
			first_[part] = static_cast<uint8_t>(next);
			next = std::min<int32_t>(next + slots[part], kTotal);
		}
		first_[kMaxParts] = static_cast<uint8_t>(next);
		clear();
	}

	void clear() {
		for (SoundingNote& note : notes_) {
			note.used = false;
		}
	}

	/// A free slot for `part`, or nullptr when the part's table is full (at its capacity).
	SoundingNote* freeSlot(int32_t part) {
		for (SoundingNote& note : slots(part)) {
			if (!note.used) {
				return &note;
			}
		}
		return nullptr;
	}

	/// Keeps `note` for `part`, replacing any note on its key and any note ended by the time it starts. A table that is
	/// still full drops it. For notes nothing plays, such as a gated-out part's (spec §5.6).
	void hold(int32_t part, const SoundingNote& note) {
		SoundingNote* free = nullptr;
		for (SoundingNote& held : slots(part)) {
			if (held.used && (held.offTick <= note.onTick || (held.lane == note.lane && held.pitch == note.pitch))) {
				held.used = false;
			}
			if (!held.used && free == nullptr) {
				free = &held;
			}
		}
		if (free != nullptr) {
			*free = note;
			free->used = true;
		}
	}

	/// Frees every note on `noteOn`'s key.
	void release(const Event& noteOn) {
		for (SoundingNote& note : slots(noteOn.part)) {
			if (note.is(noteOn)) {
				note.used = false;
			}
		}
	}

	/// Hands each note of `part` in the table to `visit(note)`.
	template <typename Visit>
	void forEach(int32_t part, Visit visit) {
		for (SoundingNote& note : slots(part)) {
			if (note.used) {
				visit(note);
			}
		}
	}
	template <typename Visit>
	void forEach(int32_t part, Visit visit) const {
		for (const SoundingNote& note : slots(part)) {
			if (note.used) {
				visit(note);
			}
		}
	}

	/// The part's note that started first, or nullptr when it has none.
	SoundingNote* oldest(int32_t part) {
		SoundingNote* best = nullptr;
		for (SoundingNote& note : slots(part)) {
			if (note.used && (best == nullptr || note.onTick < best->onTick)) {
				best = &note;
			}
		}
		return best;
	}

	/// Frees the note `noteOn` started, if it is still here. @return Whether it was.
	bool forget(const Event& noteOn) {
		for (SoundingNote& note : slots(noteOn.part)) {
			if (note.is(noteOn) && note.onTick == noteOn.tick) {
				note.used = false;
				return true;
			}
		}
		return false;
	}

	/// A note on `part`'s `lane` and `pitch` still sounding at `onTick` lasts on to `offTick` instead of starting
	/// again: a tie. @return false if none was sounding there.
	bool extend(int32_t part, Lane lane, uint8_t pitch, int32_t onTick, int32_t offTick) {
		for (SoundingNote& note : slots(part)) {
			if (note.used && note.lane == lane && note.pitch == pitch && note.offTick > onTick) {
				note.offTick = offTick > note.offTick ? offTick : note.offTick;
				return true;
			}
		}
		return false;
	}

	/// Ends every note on `noteOn`'s key no later than `noteOn` starts.
	void endKeyBy(const Event& noteOn) {
		for (SoundingNote& note : slots(noteOn.part)) {
			if (note.is(noteOn) && note.offTick > noteOn.tick) {
				note.offTick = noteOn.tick;
			}
		}
	}

	/// Ends every pitched note still sounding after `tick` at `tick`. Drum and FX hits play on.
	void endPitchedBy(int32_t tick) {
		for (int32_t part = 0; part < kMaxParts; part++) {
			endPitchedBy(part, tick);
		}
	}

	/// Ends `part`'s pitched notes still sounding after `tick` at `tick`.
	void endPitchedBy(int32_t part, int32_t tick) {
		for (SoundingNote& note : slots(part)) {
			if (note.used && note.lane == Lane::None && note.offTick > tick) {
				note.offTick = tick;
			}
		}
	}

	/// Ends every note of `part` still sounding after `tick` at `tick`, its Kit lanes' too.
	void endPartBy(int32_t part, int32_t tick) {
		for (SoundingNote& note : slots(part)) {
			if (note.used && note.offTick > tick) {
				note.offTick = tick;
			}
		}
	}

	/// Frees every pitched note: notes held back at the old key would re-strike at its pitches (spec §7.6).
	void releasePitched() {
		for (SoundingNote& note : notes_) {
			if (note.lane == Lane::None) {
				note.used = false;
			}
		}
	}

	/// Hands each note on `noteOn`'s key ending at or before it starts to `take(note)`. A note whose `take` returns
	/// true leaves the table.
	template <typename Take>
	void takeKeyEndingBy(const Event& noteOn, Take take) {
		for (SoundingNote& note : slots(noteOn.part)) {
			if (note.is(noteOn) && note.offTick <= noteOn.tick && take(note)) {
				note.used = false;
			}
		}
	}

	/// Hands each note ending at or before `tick` to `take(part, note)`, part by part. A note whose `take` returns
	/// true leaves the table.
	template <typename Take>
	void takeEndingBy(int32_t tick, Take take) {
		for (int32_t part = 0; part < kMaxParts; part++) {
			takeEndingBy(part, tick, [&](SoundingNote& note) { return take(part, note); });
		}
	}

	/// Hands each of `part`'s notes ending at or before `tick` to `take(note)`. A note whose `take` returns true
	/// leaves the table.
	template <typename Take>
	void takeEndingBy(int32_t part, int32_t tick, Take take) {
		for (SoundingNote& note : slots(part)) {
			if (note.used && note.offTick <= tick && take(note)) {
				note.used = false;
			}
		}
	}

private:
	[[nodiscard]] std::span<SoundingNote> slots(int32_t part) {
		return {notes_ + first_[part], notes_ + first_[part + 1]};
	}
	[[nodiscard]] std::span<const SoundingNote> slots(int32_t part) const {
		return {notes_ + first_[part], notes_ + first_[part + 1]};
	}

	SoundingNote notes_[kTotal];
	/// Part p's table runs from notes_[first_[p]] up to notes_[first_[p + 1]].
	uint8_t first_[kMaxParts + 1];
};

} // namespace deluge::generative
