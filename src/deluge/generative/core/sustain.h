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

// The Sustain flavour's voicing (spec §5.2, §7.4, §8.1): which chord tones a held voicing plays, where each sits in
// the Register, and how important each is to Density. Pure functions; the Generator holds the voicing between bar
// lines (`Generator::decideSustain()`).

#pragma once

#include "generative/core/genre.h"
#include "generative/core/pitch.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

/// A held voicing: up to 4 notes, each with its place in the chord.
struct Voicing {
	uint8_t pitch[kMaxChordNotes] = {};
	VoicePlace place[kMaxChordNotes] = {};
	uint8_t size = 0;
};

/// The place of `chord`'s note `i`, by its distance in scale steps from the root: the root, the 3rd (or the sus4 in
/// its place), the 5th, then the Extensions in the order the chord holds them.
VoicePlace voicePlace(const Chord& chord, int32_t i);

/// Voices `chord` in `key` inside `reg` (spec §7.3, §7.4). Voice leading: each note goes to the instance of its pitch
/// class in the Register nearest any note of `previous` (the tonic when it is empty), a tie going down. So a common
/// tone stays where it was and every other note moves by the smallest step from the voicing before. With
/// `doubleRoot`, a chord of under 4 notes adds the root an octave up (down, where up leaves the Register).
Voicing leadVoicing(const Chord& chord, const Key& key, const Register& reg, std::span<const uint8_t> previous,
                    bool doubleRoot);

/// A pedal: the key's root and the 5th above it, at their places in the Register (spec §5.2). No Extensions and no
/// voice leading: it never moves with the chord.
Voicing pedalVoicing(const Key& key, const Register& reg);

} // namespace deluge::generative
