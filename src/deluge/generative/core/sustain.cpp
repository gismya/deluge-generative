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

#include "generative/core/sustain.h"

namespace deluge::generative {

GEN_STEP_CODE VoicePlace voicePlace(const Chord& chord, int32_t i) {
	switch (chord.steps[i] - chord.steps[0]) {
	case 0:
		return VoicePlace::Root;
	case kChordThird:
	case kChordThird + 1: // a sus4 in the 3rd's place
		return VoicePlace::Third;
	case kChordFifth:
		return VoicePlace::Fifth;
	default:
		break;
	}
	// The Extensions: the first and second one the chord holds, past its triad tones.
	int32_t before = 0;
	for (int32_t j = 1; j < i; j++) {
		const int32_t interval = chord.steps[j] - chord.steps[0];
		before += interval == kChordThird || interval == kChordThird + 1 || interval == kChordFifth ? 0 : 1;
	}
	return before == 0 ? VoicePlace::Extension : VoicePlace::SecondExtension;
}

namespace {

/// The instance of `note`'s pitch class inside [low, high] nearest any of `near`, a tie going down.
GEN_STEP_CODE int32_t nearestInstance(int32_t note, int32_t low, int32_t high, std::span<const int32_t> near) {
	int32_t candidate = low + ((note - low) % 12 + 12) % 12;
	int32_t best = candidate;
	int32_t bestDistance = INT32_MAX;
	for (; candidate <= high; candidate += 12) {
		int32_t distance = INT32_MAX;
		for (int32_t n : near) {
			const int32_t d = candidate > n ? candidate - n : n - candidate;
			distance = d < distance ? d : distance;
		}
		if (distance < bestDistance) {
			best = candidate;
			bestDistance = distance;
		}
	}
	return best;
}

uint8_t midi(int32_t note) {
	return static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
}

} // namespace

GEN_STEP_CODE Voicing leadVoicing(const Chord& chord, const Key& key, const Register& reg,
                                  std::span<const uint8_t> previous, bool doubleRoot) {
	const int32_t tonic = tonicNote(reg, key.root);
	const int32_t low = tonic + reg.spanLow;
	const int32_t high = tonic + reg.spanHigh;
	int32_t near[kMaxChordNotes] = {tonic};
	int32_t nearCount = 1;
	if (!previous.empty()) {
		nearCount = 0;
		for (uint8_t note : previous) {
			near[nearCount++] = note;
		}
	}
	Voicing out;
	for (int32_t i = 0; i < chord.size; i++) {
		const int32_t note = tonic + semitonesOf(chord.steps[i], key.scale);
		out.pitch[i] = midi(nearestInstance(note, low, high, {near, static_cast<size_t>(nearCount)}));
		out.place[i] = voicePlace(chord, i);
	}
	out.size = chord.size;
	const int32_t root = out.pitch[0];
	const int32_t doubled = root + 12 <= high ? root + 12 : root - 12;
	if (doubleRoot && out.size < kMaxChordNotes && doubled >= low) {
		out.pitch[out.size] = midi(doubled);
		out.place[out.size] = VoicePlace::Doubling;
		out.size++;
	}
	return out;
}

GEN_STEP_CODE Voicing pedalVoicing(const Key& key, const Register& reg) {
	Voicing out;
	out.pitch[0] = noteFor(0, key, reg);
	out.place[0] = VoicePlace::Root;
	out.pitch[1] = noteFor(kChordFifth, key, reg);
	out.place[1] = VoicePlace::Fifth;
	out.size = 2;
	return out;
}

} // namespace deluge::generative
