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

// Chord voicing for the Sustain and Stab flavours (spec §5.2, §7.4, §8.1): which chord tones a voicing plays, where
// each sits in the Register, and how important each is to Density, and the chord notes an Arp walks and a Motif slot
// plays. Pure functions; the Generator holds the voicing between bar lines (`Generator::decideSustain()`,
// `Generator::decideStab()`).

#pragma once

#include "generative/core/genre.h"
#include "generative/core/pitch.h"
#include <algorithm>
#include <cstdint>
#include <span>

namespace deluge::generative {

/// A voicing's notes, as a part holds them from one bar line to the next: up to 4 MIDI notes.
struct HeldVoicing {
	uint8_t pitch[kMaxChordNotes] = {};
	uint8_t size = 0;

	[[nodiscard]] std::span<const uint8_t> notes() const { return {pitch, size}; }
	/// The same notes in the same order: unused slots aside, which hold no note.
	[[nodiscard]] bool sameNotes(const HeldVoicing& other) const {
		return size == other.size && std::equal(pitch, pitch + size, other.pitch);
	}
};

/// A voicing as worked out: its notes, each with its place in the chord.
struct Voicing : HeldVoicing {
	VoicePlace place[kMaxChordNotes] = {};
};

/// Whether `interval`, in scale steps above a chord's root, is one of its triad's upper tones: the 3rd (or the sus4
/// in its place) or the 5th.
constexpr bool isTriadTone(int32_t interval) {
	return interval == kChordThird || interval == kChordThird + 1 || interval == kChordFifth;
}

/// The place of `chord`'s note `i`, by its distance in scale steps from the root: the root, the 3rd (or the sus4 in
/// its place), the 5th, then the Extensions in the order the chord holds them.
VoicePlace voicePlace(const Chord& chord, int32_t i);

/// Voices `chord` in `key` inside `reg` (spec §7.3, §7.4). Voice leading: each note goes to the instance of its pitch
/// class in the Register nearest any note of `previous` (the tonic when it is empty), a tie going down. So a common
/// tone stays where it was and every other note moves by the smallest step from the voicing before. With
/// `doubleRoot`, a chord of under 4 notes adds the root an octave up (down, where up leaves the Register).
Voicing leadVoicing(const Chord& chord, const Key& key, const Register& reg, std::span<const uint8_t> previous,
                    bool doubleRoot);

/// Voices `chord` open in `key` inside `reg` (spec §5.2): a Sustain's SustainSpec::openVoicing. With `previous` empty,
/// the root and the 5th go to the instance nearest the tonic and the 3rd (or the sus4) and the Extensions to the one
/// nearest the tonic an octave up. After that each note of `previous` is a voice: each chord note takes one, at the
/// instance nearest it, by the pairing that moves the voices least in all, the first found on a tie. A voice left
/// over drops; a note left over (a chord grown by a note) goes where the first voicing would put it. No doubled root.
Voicing openVoicing(const Chord& chord, const Key& key, const Register& reg, std::span<const uint8_t> previous);
/// A pedal: the key's root and the 5th above it, at their places in the Register (spec §5.2). No Extensions and no
/// voice leading: it never moves with the chord.
Voicing pedalVoicing(const Key& key, const Register& reg);

/// The `index`-th note of an Arp walk over `chord` in `key` (spec §5.2, §8.1): the chord's notes as voiced, lowest
/// first, each snapped to `set` (the root is always allowed), then the same notes an octave up for each octave after
/// the first. The walk starts from the root's lowest place in the Register, so a Register spanning arpRange() of the
/// walk's octaves holds it whole; a note past the span's top folds down by octaves, as every note does (spec §7.3).
uint8_t arpNote(const Chord& chord, int32_t index, const Key& key, const Register& reg, DegreeSet set);

/// The note a Motif slot plays over `chord` in `key` (spec §5.2, §7.5): the chord's note at `position`, counted up
/// from its root through its notes taken into one octave, lowest first (a 9th counts as a 2nd), a position past the
/// last going on an octave up and a negative one counting down. So pitch climbs with position. The note is moved by
/// `octave` octaves, then snapped to `set` and folded into the Register as a Loop step is, so the slot follows each
/// chord change. Its own octave picks where a pitch class fits twice (spec §7.3).
uint8_t motifNote(const Chord& chord, int32_t position, int32_t octave, const Key& key, const Register& reg,
                  DegreeSet set);

} // namespace deluge::generative
