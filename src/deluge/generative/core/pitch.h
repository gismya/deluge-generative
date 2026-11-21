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

// Pitch (spec §7). The Generator works in scale steps against its own copy of the key and scale, and resolves each
// note to an absolute MIDI note only when it emits it, so a key or scale change reaches every part on its next note
// with nothing rewritten (spec §7.1).
//
// A pitch inside the Generator is a count of scale steps from the key's tonic: degree + 7 × octave, any sign. The
// order of resolution is flavour step → Answers → snap → Register fold → absolute note (spec §7.5).

#pragma once

#include "generative/core/types.h"
#include <cstdint>
#include <initializer_list>

namespace deluge::generative {

/// The key and scale the Generator plays (spec §3.2).
struct Key {
	uint8_t root = 0; ///< Pitch class, 0 = C.
	Scale scale = Scale::Minor;

	constexpr bool operator==(const Key&) const = default;
};

/// Whether `root` names a pitch class and `scale` a scale: the test a key change passes before it queues or lands.
constexpr bool validKey(int32_t root, int32_t scale) {
	return root >= 0 && root < 12 && scale >= 0 && scale < kNumScales;
}
constexpr bool validKey(Key key) {
	return validKey(key.root, static_cast<int32_t>(key.scale));
}

/// The key lean's related roots (spec §9.7): bit r is set when the key on root r in `scale` shares 6 or 7 of its 7
/// notes with `playing`.
uint16_t relatedRoots(Key playing, Scale scale);

/// `note` clamped to MIDI's 0..127.
GEN_STEP_CODE constexpr uint8_t clampMidi(int32_t note) {
	return static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
}

// ---- Register (spec §7.3) ----

/// A pitch home: the tonic is the key root nearest the anchor, and notes fold into the span around it.
struct Register {
	uint8_t anchor = 0; ///< MIDI note. 0 = the role's default.
	int8_t spanLow = 0;
	int8_t spanHigh = 0;
};

/// The Register a role uses when its genre doesn't set one (spec §7.3, tuning). Drums and FX have none.
constexpr Register defaultRegister(Role role) {
	switch (role) {
	case Role::Bass:
		return {36, -5, 12};
	case Role::Chords:
		return {60, -5, 12};
	case Role::Pad:
		return {60, -12, 12};
	case Role::Lead:
		return {72, -5, 19};
	default:
		return {};
	}
}

/// Whether a role has a Register: the pitched ones. Drums and FX play lanes, not pitches (spec §7.3).
constexpr bool hasRegister(Role role) {
	return defaultRegister(role).anchor != 0;
}

/// A part's octave shift runs from -kMaxOctaveShift to kMaxOctaveShift octaves (spec §3.1, §7.3).
constexpr int32_t kMaxOctaveShift = 2;

/// `reg` moved by `octaves`: its anchor by 12 semitones an octave, its span with it (spec §7.3). The fold and snap
/// rules run unchanged against the moved Register, so every note of the part moves by the same 12 × `octaves`.
constexpr Register shiftedRegister(Register reg, int32_t octaves) {
	reg.anchor = static_cast<uint8_t>(reg.anchor + 12 * octaves);
	return reg;
}

/// The part's tonic: the instance of `keyRoot` nearest the Register's anchor, the lower one on a 6-semitone tie.
int32_t tonicNote(const Register& reg, uint8_t keyRoot);

/// The MIDI note for `steps` scale steps from the tonic, folded by octaves into the Register's span. Where a pitch
/// class fits twice, the note's own octave picks which (spec §7.3).
uint8_t noteFor(int32_t steps, const Key& key, const Register& reg);

// ---- Degree sets (spec §7.5) ----

/// The scale degrees a Bass or Lead part may land on, named for the sound it wants and resolved per scale.
/// Append new sets at the end.
enum class DegreeSet : uint8_t { Full, Pentatonic };
constexpr int32_t kNumDegreeSets = 2;

/// Each set's degrees per scale, as bits (bit 0 the 1st degree), a Generator-wide table (tuning). The set is fixed
/// to the key's home.
constexpr uint8_t degreeBits(std::initializer_list<int32_t> degrees) {
	uint8_t bits = 0;
	for (int32_t degree : degrees) {
		bits |= static_cast<uint8_t>(1U << (degree - 1));
	}
	return bits;
}
constexpr uint8_t kAllDegrees = 0x7F;
constexpr uint8_t kDegreeSets[kNumDegreeSets][kNumScales] = {
    {kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees,
     kAllDegrees, kAllDegrees, kAllDegrees, kAllDegrees},
    {
        degreeBits({1, 2, 3, 5, 6}), // Major
        degreeBits({1, 3, 4, 5, 7}), // Minor
        degreeBits({1, 3, 4, 5, 7}), // Dorian
        degreeBits({1, 3, 4, 5, 7}), // Phrygian
        degreeBits({1, 2, 3, 5, 6}), // Lydian
        degreeBits({1, 2, 3, 5, 6}), // Mixolydian
        degreeBits({1, 3, 5, 6, 7}), // Locrian
        degreeBits({1, 3, 4, 5, 6}), // Melodic Minor
        degreeBits({1, 3, 4, 5, 7}), // Harmonic Minor: keeps the leading tone
        degreeBits({1, 3, 4, 5, 7}), // Hungarian Minor: two semitones, no better set exists
        degreeBits({1, 3, 4, 5, 6}), // Marva
        degreeBits({1, 2, 3, 5, 6}), // Arabian
    },
};

/// Floor division for possibly negative scale steps.
constexpr int32_t floorDiv(int32_t a, int32_t b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// The degree (0–6) of `steps` from the tonic.
constexpr int32_t degreeOf(int32_t steps) {
	return steps - floorDiv(steps, kDegreesPerScale) * kDegreesPerScale;
}

/// Semitones above the tonic, for any count of scale steps.
constexpr int32_t semitonesOf(int32_t steps, Scale scale) {
	return 12 * floorDiv(steps, kDegreesPerScale) + kScaleSemitones[static_cast<int32_t>(scale)][degreeOf(steps)];
}

/// Moves `steps` (from the tonic) to the nearest degree in `set`, a tie going down. `chordRoot`, the current chord's
/// root in steps from the tonic, is always allowed.
int32_t snap(int32_t steps, DegreeSet set, Scale scale, int32_t chordRoot);

// ---- Chords and Extensions (spec §7.4) ----

/// A triad's 3rd and 5th, in scale steps above its root.
constexpr int32_t kChordThird = 2;
constexpr int32_t kChordFifth = 4;

/// A voiced chord: its notes in scale steps from the tonic, lowest first. The root comes first.
struct Chord {
	int8_t steps[kMaxChordNotes] = {};
	uint8_t size = 0;
};

/// The Extension guard, data with a Generator-wide default so a later genre field, Recipe value or player setting
/// can override it: an Extension this many semitones above any chord tone (mod 12) is rejected, and the next one in
/// priority is tried. One bit per interval class.
struct ExtensionGuard {
	uint16_t rejectedAbove = 1U << 1; ///< The default: a semitone, which catches ♭9 and ♭6 over the 5th.
};

/// The Extensions, each by its interval in scale steps above the chord's root.
enum class Extension : int8_t { Sus4 = 3, Sixth = 5, Seventh = 6, Ninth = 8 };
constexpr int32_t kExtensions = 4;

/// The order Extensions are tried in (spec §7.4): the Generator-wide 7, 9, sus4, 6 unless a genre sets its own. Each
/// Extension once, the 6 after the 7 (harmonyValid()).
struct ExtensionOrder {
	Extension tries[kExtensions] = {Extension::Seventh, Extension::Ninth, Extension::Sus4, Extension::Sixth};
};
/// 9 and sus4 before the 7 (Ambient's): add9 at level 1, a sus4 add9 at level 2, and a 7th only where the 9 or the
/// sus4 is guarded.
constexpr ExtensionOrder kNinthFirst{{Extension::Ninth, Extension::Sus4, Extension::Seventh, Extension::Sixth}};

/// Whether `order` tries each Extension once, the 6 after the 7: a 6 before it would never play.
constexpr bool extensionOrderValid(const ExtensionOrder& order) {
	int32_t seen = 0;
	for (Extension extension : order.tries) {
		const bool known = extension == Extension::Sus4 || extension == Extension::Sixth
		                   || extension == Extension::Seventh || extension == Extension::Ninth;
		if (!known) {
			return false; // checked before the shift: an unknown value could be negative or past the int's width
		}
		const int32_t bit = 1 << static_cast<int32_t>(extension);
		if ((seen & bit) != 0
		    || (extension == Extension::Sixth && (seen & 1 << static_cast<int32_t>(Extension::Seventh)) == 0)) {
			return false;
		}
		seen |= bit;
	}
	return true;
}

/// The chord on `root` (steps from the tonic) in `scale`, with up to `extensionLevel` Extensions tried in `order`
/// (7, 9, sus4, 6 by default) and voiced at 4 notes at most: sus4 replaces the 3rd, an Extension that would make a
/// 5th note drops the 5th, and the 6 is only tried when the 7 was rejected.
Chord voiceChord(Scale scale, int32_t root, int32_t extensionLevel, ExtensionGuard guard = {},
                 ExtensionOrder order = {});

/// How many Extensions a chord gets at `complexity`, by Generator-wide thresholds (tuning), clamped to the genre's
/// floor and ceiling. Deterministic, with no draw.
int32_t extensionLevel(int32_t complexity, int32_t floor, int32_t ceiling);

} // namespace deluge::generative
