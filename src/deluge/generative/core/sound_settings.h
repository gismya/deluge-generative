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
};

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

/// A synth part's settings: its role's voice mode (spec §5.8), voice priority and portamento.
constexpr SoundSettings partSoundSettings(const PartSpec& part) {
	SoundSettings settings;
	settings.priority = rolePriority(part.role);
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

} // namespace deluge::generative
