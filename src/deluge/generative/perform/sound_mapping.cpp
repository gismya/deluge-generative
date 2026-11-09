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

#include "generative/perform/sound_mapping.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

// The spec §11.2 table, one list per control and engine. Every range is a starting estimate (tuning).

// Shared by the synth engines.
constexpr TargetRange kDecay[] = {
    {Target::Env1Release, -6, 6},
    {Target::Env1Decay, -6, 6, Needs::SustainBelowFull},
    {Target::Env2Decay, -6, 6, Needs::Env2InUse},
};
constexpr TargetRange kSpace[] = {
    {Target::ReverbSend, 0, 10},
    {Target::DelayFeedback, -4, 4, Needs::DelayInUse},
};
constexpr TargetRange kWobble[] = {
    {Target::ModFxDepth, -8, 8, Needs::ModFxInUse},
    {Target::LfoRate, -4, 4},
    {Target::LfoCables},
};
constexpr TargetRange kThin[] = {{Target::HpfFrequency, 0, 20}};

constexpr TargetRange kSubtractiveBrightness[] = {
    {Target::LpfFrequency, -8, 8, Needs::LpfInUse},
    {Target::WaveIndex, -5, 5},
};
constexpr TargetRange kSubtractiveDrive[] = {
    {Target::LpfMorph, 0, 15, Needs::LpfInUse},
    {Target::Fold, 0, 10},
};

constexpr TargetRange kFmBrightness[] = {
    {Target::Modulator1Level, -6, 6, Needs::Modulator1InUse},
    {Target::Modulator2Level, -6, 6, Needs::Modulator2InUse},
};
constexpr TargetRange kFmDrive[] = {
    {Target::ModulatorFeedback, 0, 8},
    {Target::CarrierFeedback, 0, 8},
    {Target::Fold, 0, 10},
};

constexpr TargetRange kRingmodBrightness[] = {
    {Target::LpfFrequency, -8, 8, Needs::LpfInUse},
    {Target::PulseWidth, -6, 6},
};

constexpr TargetRange kDx7Brightness[] = {{Target::LpfFrequency, -10, 0, Needs::LpfInUse}};
constexpr TargetRange kDx7Decay[] = {
    {Target::Env1Release, -6, 0},
    {Target::Env1Decay, -6, 0, Needs::SustainBelowFull},
    {Target::Env2Decay, -6, 0, Needs::Env2InUse},
};
constexpr TargetRange kDx7Drive[] = {{Target::Fold, 0, 10}};
constexpr TargetRange kDx7Thin[] = {{Target::HpfFrequency, 0, 20, Needs::HpfInUse}};

// Factory kit rows are one-shots with sustain 50 and the LPF open, so Brightness only darkens and Decay only
// shortens. Their Drive (bitcrush and SRR) is a genre opt-in no genre takes yet, and Wobble is the Kit's.
constexpr TargetRange kKitRowBrightness[] = {{Target::LpfFrequency, -15, 0, Needs::LpfInUse}};
constexpr TargetRange kKitRowDecay[] = {
    {Target::Env1Sustain, -6, 0},
    {Target::Env1Decay, -6, 0},
};
constexpr TargetRange kReverbOnly[] = {{Target::ReverbSend, 0, 10}};

constexpr TargetRange kKitLevelBrightness[] = {{Target::LpfFrequency, -20, 0}};
constexpr TargetRange kKitLevelWobble[] = {{Target::ModFxDepth, -8, 8, Needs::ModFxInUse}};
constexpr TargetRange kKitLevelThin[] = {{Target::HpfFrequency, 0, 25}};

constexpr int32_t kNumEngines = 6;
constexpr std::span<const TargetRange> kNone{};

/// Indexed by engine, then by control.
constexpr std::span<const TargetRange> kTable[kNumEngines][kNumSoundControls] = {
    // Brightness, Decay, Drive, Space, Wobble, Thin
    {kSubtractiveBrightness, kDecay, kSubtractiveDrive, kSpace, kWobble, kThin},      // Subtractive
    {kFmBrightness, kDecay, kFmDrive, kSpace, kWobble, kNone},                        // FM: no HPF on factory FM
    {kRingmodBrightness, kDecay, kSubtractiveDrive, kSpace, kWobble, kThin},          // Ringmod: fold and ladder morph
    {kDx7Brightness, kDx7Decay, kDx7Drive, kSpace, kWobble, kDx7Thin},                // DX7
    {kKitRowBrightness, kKitRowDecay, kNone, kReverbOnly, kNone, kThin},              // Kit row
    {kKitLevelBrightness, kNone, kNone, kReverbOnly, kKitLevelWobble, kKitLevelThin}, // Kit level
};

} // namespace

std::span<const TargetRange> controlTargets(SoundControl control, Engine engine) {
	return kTable[static_cast<int32_t>(engine)][static_cast<int32_t>(control)];
}

int32_t targetValue(const TargetRange& range, int32_t home, int32_t offset) {
	offset = std::clamp<int32_t>(offset, -kControlFull, kControlFull);
	if (range.target == Target::LfoCables) {
		const int64_t scaled = int64_t{home} * (kControlFull + offset) / kControlFull;
		return static_cast<int32_t>(std::clamp<int64_t>(scaled, -kCableAmountFull, kCableAmountFull));
	}
	// The share of the range's end on the offset's side, in hundredths of a menu unit: low is 0 or below, high 0 or
	// above. Converted with no 64-bit division (the Cortex-A9 divides in software, for every param a step moves):
	// kParamUnitsPerMenuUnit is 858993 * 100 + 45.
	static_assert(kControlFull == 100 && kParamUnitsPerMenuUnit == 858993 * 100 + 45);
	const int32_t menuHundredths = offset < 0 ? -range.low * offset : range.high * offset;
	const int64_t value = home + int64_t{menuHundredths} * 858993 + menuHundredths * 45 / 100;
	return static_cast<int32_t>(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX));
}

} // namespace deluge::generative::perform
