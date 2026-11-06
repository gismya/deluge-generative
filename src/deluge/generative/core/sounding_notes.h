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
#include <cstdint>

namespace deluge::generative {

/// Most chord notes one pitched part plays at once (spec §5.8: Chords and Pad at most 4).
constexpr int32_t kMaxChordNotes = 4;
/// Notes one part may have between decision and note-off: a Kit part one per lane (a new hit on a lane ends the old
/// one), a pitched part a chord plus the next step's chord overlapping it (a slide).
constexpr int32_t kMaxSoundingPerPart = kMaxLanesPerPart > 2 * kMaxChordNotes ? kMaxLanesPerPart : 2 * kMaxChordNotes;

struct SoundingNote {
	int32_t onTick = 0;
	int32_t offTick = 0;
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
	void clear() {
		for (auto& part : notes_) {
			for (SoundingNote& note : part) {
				note.used = false;
			}
		}
	}

	SoundingNote* slots(int32_t part) { return notes_[part]; }
	[[nodiscard]] const SoundingNote* slots(int32_t part) const { return notes_[part]; }
	static constexpr int32_t kSlots = kMaxSoundingPerPart;

	/// A free slot for `part`, or nullptr when the part's table is full.
	SoundingNote* freeSlot(int32_t part) {
		for (SoundingNote& note : notes_[part]) {
			if (!note.used) {
				return &note;
			}
		}
		return nullptr;
	}
	/// The part's note that started first. Only call when the part has one.
	SoundingNote* oldest(int32_t part) {
		SoundingNote* best = nullptr;
		for (SoundingNote& note : notes_[part]) {
			if (note.used && (best == nullptr || note.onTick < best->onTick)) {
				best = &note;
			}
		}
		return best;
	}

private:
	SoundingNote notes_[kMaxParts][kMaxSoundingPerPart];
};

} // namespace deluge::generative
