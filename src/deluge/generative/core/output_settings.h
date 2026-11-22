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

// Non-param sound-editor settings logged for Capture and AS IT BEGAN (spec §17.2, §17.6): oscillator type and sample,
// filter mode, arp mode, polyphony, synth mode, and the other discrete/non-AutoParam settings on a Sound, Kit, or Song.
// Portable layout so the harness checks the header block encoding and replay without firmware types.

#pragma once

#include "generative/core/types.h"
#include <cstdint>
#include <cstring>

namespace deluge::generative {

/// Non-param setting IDs stored in CommandKind::SetSetting (Command::length), SteeringLog::kSettingPathTag (0x85), and
/// SteeringLog::kSettingsTag (0x92) header blocks.
///
/// Order matters: SynthMode and Osc0Type/Osc1Type come first so applying a header block in ascending ID order sets
/// synth mode (which resets filter modes) and oscillator type (which resets sample/wavetable ranges) before filter
/// modes, sample paths, pitch and sample flags.
enum class OutputSetting : uint16_t {
	// Applied first (side effects on filter modes and oscillator ranges).
	SynthMode = 0,
	Osc0Type,
	Osc1Type,

	// ModControllableAudio (Song, Kit global FX, Synth Sound, Kit row SoundDrum).
	LpfMode,
	HpfMode,
	FilterRoute,
	ModFxType,
	ClippingAmount,
	DelayFlags, ///< pingPong | (analog << 1) | (syncLevel << 8) | (syncType << 16)
	SidechainAttack,
	SidechainRelease,
	SidechainSync, ///< syncLevel | (syncType << 8)
	CompAttack,
	CompRelease,
	CompThreshold,
	CompRatio,
	CompSidechain,
	CompBlend,
	StutterFlags, ///< useSongStutter | (quantized << 1) | (reversed << 2) | (pingPong << 3)

	// Sound (Synth SoundInstrument, Kit row SoundDrum).
	Polyphonic,     ///< polyphonic | (maxVoiceCount << 8) | (voicePriority << 16)
	TransposeCents, ///< (transpose & 0xFFFF) | ((uint8_t)cents << 16)
	SidechainSend,
	OscSync,
	Osc0RetrigPhase,
	Osc1RetrigPhase,
	Unison, ///< (uint8_t)numUnison | ((uint8_t)unisonDetune << 8) | ((uint8_t)unisonStereoSpread << 16)
	Lfo0,   ///< waveType | (syncLevel << 8) | (syncType << 16)
	Lfo1,
	Lfo2,
	Lfo3,
	Mod0Pitch, ///< (transpose & 0xFFFF) | ((uint8_t)cents << 16)
	Mod0RetrigPhase,
	Mod1Pitch, ///< (transpose & 0xFFFF) | ((uint8_t)cents << 16) | (toMod0 << 24)
	Mod1RetrigPhase,

	// Source[0..1] (on Sound).
	Osc0Pitch,       ///< (transpose & 0xFFFF) | ((uint8_t)cents << 16); range 0's holder when SAMPLE
	Osc0SampleFlags, ///< repeatMode | (reversed << 4) | (pitchSpeed << 5) | (interp << 6) | ((uint8_t)timeStretch << 8)
	Osc1Pitch,
	Osc1SampleFlags,
	Osc0SamplePath, ///< File path (kSettingPathTag in records, path entry in kSettingsTag header)
	Osc1SamplePath, ///< File path

	// ArpeggiatorSettings (InstrumentClip::arpSettings on Synth/Kit; SoundDrum::arpSettings on Kit row).
	ArpMain, ///< mode | (preset << 4) | (octaveMode << 8) | (noteMode << 12) | (kitArp << 16) | (lock << 17) | (mpe <<
	         ///< 18)
	ArpCounts, ///< numOctaves | (numStepRepeats << 8) | (chordTypeIndex << 16)
	ArpSync,   ///< syncLevel | (syncType << 8)

	// Song Reverb (kSongParams only: live in AudioEngine::reverb, stored on Song).
	ReverbModel,
	ReverbRoomSize,
	ReverbDamping,
	ReverbWidth,
	ReverbHpf,
	ReverbLpf,
	ReverbPan,
	ReverbSidechainAttack,
	ReverbSidechainRelease,
	ReverbSidechainVolume,
	ReverbSidechainShape,
	ReverbSidechainSync, ///< syncLevel | (syncType << 8)
};

constexpr int32_t kNumOutputSettings = static_cast<int32_t>(OutputSetting::ReverbSidechainSync) + 1;
static_assert(kNumOutputSettings <= 64, "edited settings mask fits in uint64_t");

constexpr bool isPathSetting(OutputSetting setting) {
	return setting == OutputSetting::Osc0SamplePath || setting == OutputSetting::Osc1SamplePath;
}

constexpr uint64_t settingBit(OutputSetting setting) {
	return uint64_t{1} << static_cast<uint16_t>(setting);
}

/// First bytes of a kSettingsTag (0x92) header block. Followed by `scalarCount` packed (uint16_t setting, int32_t
/// value) entries (kScalarSettingBytes each) and `pathCount` packed (uint16_t setting, uint8_t length, char[length])
/// entries. Unaligned: read each field with memcpy.
struct SettingsBlockHeader {
	uint8_t output; ///< A part, or kSongParams.
	Lane lane;      ///< A Kit row's lane, or Lane::None for the Output's own settings (or the song's).
	uint8_t scalarCount;
	uint8_t pathCount;
};
static_assert(sizeof(SettingsBlockHeader) == 4);

constexpr int32_t kScalarSettingBytes = sizeof(uint16_t) + sizeof(int32_t);
static_assert(kScalarSettingBytes == 6);
constexpr int32_t kPathSettingPrefixBytes = sizeof(uint16_t) + sizeof(uint8_t);
static_assert(kPathSettingPrefixBytes == 3);

/// Reads a kSettingsTag (0x92) header block: validates its bounds, then calls `onScalar(setting, value)` for each
/// scalar entry and `onPath(setting, path, pathLength)` for each path entry. @return false if the block is short.
template <typename OnScalar, typename OnPath>
bool readSettingsBlock(const void* data, int32_t length, SettingsBlockHeader& header, OnScalar onScalar,
                       OnPath onPath) {
	if (length < static_cast<int32_t>(sizeof(SettingsBlockHeader))) {
		return false;
	}
	const auto* at = static_cast<const uint8_t*>(data);
	const uint8_t* end = at + length;
	std::memcpy(&header, at, sizeof header);
	at += sizeof header;
	for (int32_t i = 0; i < header.scalarCount; i++) {
		if (end - at < kScalarSettingBytes) {
			return false;
		}
		uint16_t setting = 0;
		int32_t value = 0;
		std::memcpy(&setting, at, sizeof setting);
		std::memcpy(&value, at + sizeof setting, sizeof value);
		at += kScalarSettingBytes;
		onScalar(static_cast<OutputSetting>(setting), value);
	}
	for (int32_t i = 0; i < header.pathCount; i++) {
		if (end - at < kPathSettingPrefixBytes) {
			return false;
		}
		uint16_t setting = 0;
		std::memcpy(&setting, at, sizeof setting);
		const uint8_t pathLen = at[sizeof setting];
		at += kPathSettingPrefixBytes;
		if (end - at < pathLen) {
			return false;
		}
		onPath(static_cast<OutputSetting>(setting), reinterpret_cast<const char*>(at), pathLen);
		at += pathLen;
	}
	return true;
}

} // namespace deluge::generative
