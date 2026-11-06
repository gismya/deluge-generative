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

#include "generative/core/pitch.h"
#include "generative/core/recipe.h"

namespace deluge::generative {

namespace {

/// Floor division for possibly negative scale steps.
constexpr int32_t floorDiv(int32_t a, int32_t b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// The degree (0–6) of `steps` from the tonic.
constexpr int32_t degreeOf(int32_t steps) {
	return steps - floorDiv(steps, kDegreesPerScale) * kDegreesPerScale;
}

constexpr int32_t pitchClass(int32_t note) {
	return (note % 12 + 12) % 12;
}

/// Semitones above the tonic, for any count of scale steps.
constexpr int32_t semitonesOf(int32_t steps, Scale scale) {
	return 12 * floorDiv(steps, kDegreesPerScale) + kScaleSemitones[static_cast<int32_t>(scale)][degreeOf(steps)];
}

/// Complexity at which each Extension level starts (tuning).
constexpr int32_t kExtensionLevelFrom[] = {macroPoints(17), macroPoints(34)};

} // namespace

int32_t tonicNote(const Register& reg, uint8_t keyRoot) {
	int32_t below = reg.anchor - pitchClass(reg.anchor - keyRoot);
	return reg.anchor - below > 6 ? below + 12 : below;
}

uint8_t noteFor(int32_t steps, const Key& key, const Register& reg) {
	int32_t tonic = tonicNote(reg, key.root);
	int32_t octave = floorDiv(steps, kDegreesPerScale);
	int32_t degree = steps - octave * kDegreesPerScale;
	int32_t note = tonic + 12 * octave + kScaleSemitones[static_cast<int32_t>(key.scale)][degree];
	while (note > tonic + reg.spanHigh) {
		note -= 12;
	}
	while (note < tonic + reg.spanLow) {
		note += 12;
	}
	return static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
}

int32_t snap(int32_t steps, DegreeSet set, Scale scale, int32_t chordRoot) {
	uint8_t allowed = kDegreeSets[static_cast<int32_t>(set)][static_cast<int32_t>(scale)]
	                  | static_cast<uint8_t>(1U << degreeOf(chordRoot));
	// Every set holds the tonic, so a match is at most half an octave away.
	for (int32_t distance = 0; distance <= kDegreesPerScale / 2; distance++) {
		if (allowed & (1U << degreeOf(steps - distance))) {
			return steps - distance;
		}
		if (allowed & (1U << degreeOf(steps + distance))) {
			return steps + distance;
		}
	}
	return steps;
}

Chord voiceChord(Scale scale, int32_t root, int32_t extensionLevel, ExtensionGuard guard) {
	// Intervals in scale steps above the root.
	constexpr int32_t kThird = 2;
	constexpr int32_t kFifth = 4;
	enum Extension : int32_t { kSeventh = 6, kNinth = 8, kSus4 = 3, kSixth = 5 };

	int32_t tones[kMaxChordNotes + 1] = {0, kThird, kFifth};
	int32_t size = 3;
	auto has = [&](int32_t interval) {
		for (int32_t i = 0; i < size; i++) {
			if (tones[i] == interval) {
				return true;
			}
		}
		return false;
	};
	auto remove = [&](int32_t interval) {
		for (int32_t i = 0; i < size; i++) {
			if (tones[i] == interval) {
				tones[i] = tones[--size];
				return;
			}
		}
	};
	// Rejected when it sits a guarded interval above any chord tone it would sound with.
	auto guarded = [&](int32_t extension, int32_t ignoring) {
		int32_t pitch = semitonesOf(root + extension, scale);
		for (int32_t i = 0; i < size; i++) {
			if (tones[i] == ignoring) {
				continue;
			}
			int32_t above = ((pitch - semitonesOf(root + tones[i], scale)) % 12 + 12) % 12;
			if (guard.rejectedAbove & (1U << above)) {
				return true;
			}
		}
		return false;
	};

	bool seventhRejected = false;
	int32_t added = 0;
	for (int32_t extension : {kSeventh, kNinth, kSus4, kSixth}) {
		if (added >= extensionLevel) {
			break;
		}
		if (extension == kSixth && !seventhRejected) {
			continue; // a 6 under a ♭7 is a semitone cluster the guard can't see
		}
		bool sus = extension == kSus4;
		if (guarded(extension, sus ? kThird : -1)) {
			seventhRejected |= extension == kSeventh;
			continue;
		}
		if (sus) {
			remove(kThird);
		}
		else if (size == kMaxChordNotes && has(kFifth)) {
			remove(kFifth);
		}
		tones[size++] = extension;
		added++;
	}

	// Lowest first.
	for (int32_t i = 1; i < size; i++) {
		for (int32_t j = i; j > 0 && tones[j] < tones[j - 1]; j--) {
			int32_t t = tones[j];
			tones[j] = tones[j - 1];
			tones[j - 1] = t;
		}
	}
	Chord chord;
	chord.size = static_cast<uint8_t>(size);
	for (int32_t i = 0; i < size; i++) {
		chord.steps[i] = static_cast<int8_t>(root + tones[i]);
	}
	return chord;
}

int32_t extensionLevel(int32_t complexity, int32_t floor, int32_t ceiling) {
	int32_t level = 0;
	for (int32_t from : kExtensionLevelFrom) {
		level += complexity >= from ? 1 : 0;
	}
	return level < floor ? floor : level > ceiling ? ceiling : level;
}

} // namespace deluge::generative
