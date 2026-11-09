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

// The event stream (spec §2.2): what the step function produces and every driver consumes. The same Recipe, seed
// and takes, steered the same way at the same steps, give the same stream (spec §6.3).

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

/// Event kinds, in play order for events on the same tick: a Section line comes first, then key changes, controls,
/// note-offs and last note-ons (spec §12.6).
enum class EventKind : uint8_t { Section, KeyScale, Control, NoteOff, NoteOn };

struct Event {
	/// Generator ticks (24 per 16th) from step 0's grid line.
	int32_t tick = 0;
	EventKind kind = EventKind::NoteOn;
	uint8_t part = 0;       ///< NoteOn, NoteOff, Control: the part's index in the genre's roster.
	Lane lane = Lane::None; ///< NoteOn, NoteOff: the lane of a Drums or FX part. Control: the lane, or None kit-wide.
	/// NoteOn, NoteOff: absolute MIDI note of a pitched part. KeyScale: the key's root pitch class, 0 = C (keyRoot()).
	uint8_t pitch = 0;
	uint8_t velocity = 0;      ///< NoteOn: 1–127.
	bool muted : 1 = false;    ///< NoteOn: a muted or soloed-out part's note stays in the stream, flagged (spec §12.5).
	bool ornament : 1 = false; ///< NoteOn: an Ornament hit, dropped first on overflow (spec §2.7).
	bool snap : 1 = false;     ///< Control: jump to the offset at once, instead of ramping over the step (spec §11.7).
	SoundControl control = SoundControl::Brightness; ///< Control.
	SectionKind section = SectionKind::Intro;        ///< Section: the kind starting.
	uint16_t occurrence = 0;                         ///< Section: 0 for the first of its kind in the piece.
	/// Control: the offset from home in the control's own units, -kControlFull..kControlFull, already clamped to the
	/// control's range (spec §2.2, §11.3).
	int8_t offset = 0;
	Scale scale = Scale::Minor; ///< KeyScale.

	static constexpr Event noteOn(int32_t tick, uint8_t part, Lane lane, uint8_t pitch, uint8_t velocity) {
		Event e;
		e.tick = tick;
		e.kind = EventKind::NoteOn;
		e.part = part;
		e.lane = lane;
		e.pitch = pitch;
		e.velocity = velocity;
		return e;
	}
	static constexpr Event noteOff(int32_t tick, uint8_t part, Lane lane, uint8_t pitch) {
		Event e;
		e.tick = tick;
		e.kind = EventKind::NoteOff;
		e.part = part;
		e.lane = lane;
		e.pitch = pitch;
		return e;
	}
	static constexpr Event sectionStart(int32_t tick, SectionKind kind, uint16_t occurrence) {
		Event e;
		e.tick = tick;
		e.kind = EventKind::Section;
		e.section = kind;
		e.occurrence = occurrence;
		return e;
	}
	static constexpr Event keyScale(int32_t tick, uint8_t root, Scale scale) {
		Event e;
		e.tick = tick;
		e.kind = EventKind::KeyScale;
		e.pitch = root;
		e.scale = scale;
		return e;
	}

	static constexpr Event controlChange(int32_t tick, uint8_t part, Lane lane, SoundControl control, int8_t offset,
	                                     bool snap) {
		Event e;
		e.tick = tick;
		e.kind = EventKind::Control;
		e.part = part;
		e.lane = lane;
		e.control = control;
		e.offset = offset;
		e.snap = snap;
		return e;
	}

	/// KeyScale: the key's root pitch class, 0 = C.
	[[nodiscard]] constexpr uint8_t keyRoot() const { return pitch; }

	/// Whether two note events are for the same sounding key: the same part and lane, or the same part and pitch.
	[[nodiscard]] constexpr bool sameKey(const Event& other) const {
		return part == other.part && lane == other.lane && (lane != Lane::None || pitch == other.pitch);
	}

	constexpr bool operator==(const Event&) const = default;
};
/// The event buffer holds hundreds of these in each Generator block (spec §2.7, H18), so every kind shares one 16 B
/// layout: the Control fields reuse what notes leave free, and KeyScale's root lives in `pitch`.
static_assert(sizeof(Event) == 16, "an Event stays 16 B");

} // namespace deluge::generative
