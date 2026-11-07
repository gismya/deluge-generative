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
#include "generative/core/pitch.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

/// Notes one part may have between decision and note-off: a Kit part one per lane (a new hit on a lane ends the old
/// one), a pitched part a chord plus the next step's chord overlapping it (a slide).
constexpr int32_t kMaxSoundingPerPart = kMaxLanesPerPart > 2 * kMaxChordNotes ? kMaxLanesPerPart : 2 * kMaxChordNotes;

struct SoundingNote {
	int32_t onTick = 0;
	int32_t offTick = 0;
	/// Its velocity before Energy's layer: what a late entry re-strikes it with, under the Energy then (spec §5.6).
	int16_t baseVelocity = 0;
	Lane lane = Lane::None;
	uint8_t pitch = 0;
	bool used = false;

	[[nodiscard]] Event noteOff(uint8_t part) const { return Event::noteOff(offTick, part, lane, pitch); }
	[[nodiscard]] bool is(const Event& e) const {
		return used && lane == e.lane && (lane != Lane::None || pitch == e.pitch);
	}
};

class SoundingNotes {
public:
	static constexpr int32_t kSlots = kMaxSoundingPerPart;

	void clear() {
		for (auto& part : notes_) {
			for (SoundingNote& note : part) {
				note.used = false;
			}
		}
	}

	/// A free slot for `part`, or nullptr when the part's table is full.
	SoundingNote* freeSlot(int32_t part) {
		for (SoundingNote& note : notes_[part]) {
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
		for (SoundingNote& held : notes_[part]) {
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
		for (SoundingNote& note : notes_[noteOn.part]) {
			if (note.is(noteOn)) {
				note.used = false;
			}
		}
	}

	/// Hands each note of `part` in the table to `visit(note)`.
	template <typename Visit>
	void forEach(int32_t part, Visit visit) {
		for (SoundingNote& note : notes_[part]) {
			if (note.used) {
				visit(note);
			}
		}
	}
	template <typename Visit>
	void forEach(int32_t part, Visit visit) const {
		for (const SoundingNote& note : notes_[part]) {
			if (note.used) {
				visit(note);
			}
		}
	}

	/// The part's note that started first, or nullptr when it has none.
	SoundingNote* oldest(int32_t part) {
		SoundingNote* best = nullptr;
		for (SoundingNote& note : notes_[part]) {
			if (note.used && (best == nullptr || note.onTick < best->onTick)) {
				best = &note;
			}
		}
		return best;
	}

	/// Frees the note `noteOn` started, if it is still here. @return Whether it was.
	bool forget(const Event& noteOn) {
		for (SoundingNote& note : notes_[noteOn.part]) {
			if (note.is(noteOn) && note.onTick == noteOn.tick) {
				note.used = false;
				return true;
			}
		}
		return false;
	}

	/// Ends every note on `noteOn`'s key no later than `noteOn` starts.
	void endKeyBy(const Event& noteOn) {
		for (SoundingNote& note : notes_[noteOn.part]) {
			if (note.is(noteOn) && note.offTick > noteOn.tick) {
				note.offTick = noteOn.tick;
			}
		}
	}

	/// Ends every pitched note still sounding after `tick` at `tick`. Drum and FX hits play on.
	void endPitchedBy(int32_t tick) {
		for (auto& part : notes_) {
			for (SoundingNote& note : part) {
				if (note.used && note.lane == Lane::None && note.offTick > tick) {
					note.offTick = tick;
				}
			}
		}
	}

	/// Frees every pitched note: notes held back at the old key would re-strike at its pitches (spec §7.6).
	void releasePitched() {
		for (auto& part : notes_) {
			for (SoundingNote& note : part) {
				if (note.lane == Lane::None) {
					note.used = false;
				}
			}
		}
	}

	/// Hands each note on `noteOn`'s key ending at or before it starts to `take(note)`. A note whose `take` returns
	/// true leaves the table.
	template <typename Take>
	void takeKeyEndingBy(const Event& noteOn, Take take) {
		for (SoundingNote& note : notes_[noteOn.part]) {
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
			for (SoundingNote& note : notes_[part]) {
				if (note.used && note.offTick <= tick && take(part, note)) {
					note.used = false;
				}
			}
		}
	}

private:
	SoundingNote notes_[kMaxParts][kMaxSoundingPerPart];
};

} // namespace deluge::generative
