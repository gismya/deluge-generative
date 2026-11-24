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

// What the Generator sets once when it loads a part's or lane's sound, before playback (spec §10.6, §5.8). None of
// it is a sound control, and nothing changes it live. The firmware maps these onto plain Sound and clip fields.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

/// A Sound's voice mode. Auto is how a sample row is set up, and the Kit rows the Generator doesn't choke keep it.
enum class VoiceMode : uint8_t { Auto, Poly, Mono, Legato, Choke };

/// CPU-load culling order: Low voices go first (spec §10.6).
enum class VoicePriorityLevel : uint8_t { Low, Medium, High };

struct SoundSettings {
	VoiceMode voiceMode = VoiceMode::Auto;
	/// The most voices, or 0 to keep the preset's.
	uint8_t maxVoices = 0;
	VoicePriorityLevel priority = VoicePriorityLevel::Medium;
	/// A part that slides (Loop) keeps a preset's portamento, or gets `portamento` if the preset's is off. Every
	/// other part gets portamento 0.
	bool slides = false;
	/// Portamento in the sound editor's 0–50, for a sliding part whose preset has it off (tuning).
	uint8_t portamento = 0;
	/// The post-FX volume knob, as a percent of the preset's position. 100 keeps the preset's.
	uint8_t volume = 100;
	/// Sets the preset's whole-sound transpose to 0, leaving each oscillator's, so a pitched part's Register is the
	/// pitch heard whatever the pick (ADR 0007). FX lanes' synths and Kit rows keep theirs.
	bool zeroTranspose = false;
	/// The sound's delay setup (spec §10.6): when `on`, its Delay's sync level and type, ping-pong and analog are
	/// written; feedback stays as the preset saved it.
	DelaySetup delay;
};

/// The Delay's sync level for `note` in a song whose ticks run at `magnitude` (its inside-world tick magnitude plus its
/// BPM offset): the stock levels count from 1, a whole note at the default magnitude 3, and a song at another
/// magnitude shifts them, as its 16th's 8 - magnitude does (spec §10.6). Held to the stock 1..9.
constexpr int32_t delaySyncLevel(DelayNote note, int32_t magnitude) {
	const int32_t level = 4 - magnitude + static_cast<int32_t>(note);
	// 0 is the stock's "off" and 9 its shortest, so a song far from the default magnitude gets the nearest synced
	// length rather than no delay.
	return level < 1 ? 1 : level > 9 ? 9 : level;
}

/// Scales a knob param's value to `percent` of its knob position, the int32 range read as 0 at INT32_MIN and full at
/// INT32_MAX, as the knob's 0–50 shows it. 50 halves the knob: a preset at 50 starts at 25, one at 40 at 20.
constexpr int32_t scaleKnob(int32_t value, uint8_t percent) {
	int64_t position = static_cast<int64_t>(value) - INT32_MIN;
	return static_cast<int32_t>(position * percent / 100 + INT32_MIN);
}

constexpr VoicePriorityLevel rolePriority(Role role) {
	switch (role) {
	case Role::Drums:
	case Role::Bass:
		return VoicePriorityLevel::High;
	case Role::Lead:
	case Role::Chords:
		return VoicePriorityLevel::Medium;
	case Role::Pad:
	case Role::FX:
		return VoicePriorityLevel::Low;
	}
	return VoicePriorityLevel::Medium;
}

/// A synth part's settings: its role's voice mode (spec §5.8), voice priority, portamento and, on a pitched part,
/// whole-sound transpose 0. Its volume is the part's scaled by `presetVolume`, the loaded preset's palette level
/// (PaletteEntry::volume, SoundLoad::volume).
constexpr SoundSettings partSoundSettings(const PartSpec& part, uint8_t presetVolume = 100) {
	SoundSettings settings;
	settings.priority = rolePriority(part.role);
	settings.zeroTranspose = part.role != Role::Drums && part.role != Role::FX;
	switch (part.role) {
	case Role::Bass:
		settings.voiceMode = VoiceMode::Legato;
		break;
	case Role::Lead:
		settings.voiceMode = part.flavour == Flavour::Loop ? VoiceMode::Legato : VoiceMode::Poly;
		break;
	case Role::Chords:
	case Role::Pad:
		settings.voiceMode = VoiceMode::Poly;
		settings.maxVoices = 4;
		break;
	case Role::Drums:
	case Role::FX:
		break;
	}
	settings.volume = static_cast<uint8_t>(part.volume * presetVolume / 100);
	settings.delay = part.delay;
	if (part.flavour == Flavour::Loop) {
		settings.slides = true;
		settings.portamento = part.role == Role::Bass ? 6 : 8;
	}
	return settings;
}

/// A Kit row's settings: CHOKE on the hat pair only, one choke group per Kit (spec §10.6).
constexpr SoundSettings laneSoundSettings(Lane lane) {
	SoundSettings settings;
	settings.priority = rolePriority(isFxLane(lane) ? Role::FX : Role::Drums);
	if (lane == Lane::ClosedHat || lane == Lane::OpenHat) {
		settings.voiceMode = VoiceMode::Choke;
	}
	return settings;
}
/// The same for `part`'s lane, with the delay setup its LaneSpec gives the row and the part's volume (spec §10.6).
constexpr SoundSettings laneSoundSettings(const PartSpec& part, Lane lane) {
	SoundSettings settings = laneSoundSettings(lane);
	settings.volume = part.volume;
	settings.delay = delaySetupOf(part, lane);
	return settings;
}

} // namespace deluge::generative
