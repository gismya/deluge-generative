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

// How a sound control moves params (spec §11.2): driver-side, since the core sends only offsets (spec §2.2). Portable,
// so the harness checks the table and the arithmetic; the firmware adapter turns each Target into the params it names
// and checks each Needs against the loaded preset.

#pragma once

#include "generative/core/types.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// The engine a set of controls drives: a column of the spec §11.2 table.
enum class Engine : uint8_t {
	Subtractive, ///< subtractive and wavetable synths
	Fm,
	Ringmod,
	Dx7,
	KitRow,   ///< a Kit row playing a sample
	KitLevel, ///< a whole Kit, through its affect-entire params
};

/// What a control moves. Each names one param, or the same param on each oscillator, modulator, carrier or LFO
/// that uses it; the adapter expands it. Level, pan, arp and randomizer params, tempo and swing are never targets
/// (spec §11.8).
enum class Target : uint8_t {
	LpfFrequency,
	LpfMorph,
	HpfFrequency,
	WaveIndex,  ///< each wavetable oscillator
	PulseWidth, ///< each oscillator
	Modulator1Level,
	Modulator2Level,
	ModulatorFeedback, ///< each modulator in use
	CarrierFeedback,   ///< both carriers
	Fold,
	Env1Release,
	Env1Decay,
	Env1Sustain,
	Env2Decay,
	ReverbSend,
	DelayFeedback,
	ModFxDepth,
	LfoRate,   ///< each LFO patched to something
	LfoCables, ///< each cable from an LFO: its amount scaled ×0..×2 instead of offset
};

/// Whether the loaded preset uses a target, so it takes part (spec §11.1). The adapter checks it once per piece.
enum class Needs : uint8_t {
	Always,
	LpfInUse,         ///< the LPF isn't off
	HpfInUse,         ///< the HPF is above 0
	SustainBelowFull, ///< env1's sustain is below 50, so its decay is heard
	Env2InUse,        ///< env2 is patched to something
	DelayInUse,       ///< delay feedback is above 0: the macro layer never raises it from 0 (spec §11.2)
	ModFxInUse,       ///< the mod FX type isn't none: a type change allocates memory
	Modulator1InUse,  ///< modulator 1's level is above 0
	Modulator2InUse,  ///< modulator 2's level is above 0
};

/// A target and the range a control moves it, in menu units (0–50) relative to its home (tuning). The whole of `low`
/// is reached at -kControlFull and `high` at +kControlFull. LfoCables ignores the range: it scales.
struct TargetRange {
	Target target = Target::LpfFrequency;
	int8_t low = 0;
	int8_t high = 0;
	Needs needs = Needs::Always;
};

namespace mapping {

// The spec §11.2 table, one list per control and engine. Every range is a starting estimate (tuning).

// Shared by the synth engines.
inline constexpr TargetRange kDecay[] = {
    {Target::Env1Release, -6, 6},
    {Target::Env1Decay, -6, 6, Needs::SustainBelowFull},
    {Target::Env2Decay, -6, 6, Needs::Env2InUse},
};
inline constexpr TargetRange kSpace[] = {
    {Target::ReverbSend, 0, 10},
    {Target::DelayFeedback, -4, 4, Needs::DelayInUse},
};
inline constexpr TargetRange kWobble[] = {
    {Target::ModFxDepth, -8, 8, Needs::ModFxInUse},
    {Target::LfoRate, -4, 4},
    {Target::LfoCables},
};
inline constexpr TargetRange kThin[] = {{Target::HpfFrequency, 0, 20}};

inline constexpr TargetRange kSubtractiveBrightness[] = {
    {Target::LpfFrequency, -8, 8, Needs::LpfInUse},
    {Target::WaveIndex, -5, 5},
};
inline constexpr TargetRange kSubtractiveDrive[] = {
    {Target::LpfMorph, 0, 15, Needs::LpfInUse},
    {Target::Fold, 0, 10},
};

inline constexpr TargetRange kFmBrightness[] = {
    {Target::Modulator1Level, -6, 6, Needs::Modulator1InUse},
    {Target::Modulator2Level, -6, 6, Needs::Modulator2InUse},
};
inline constexpr TargetRange kFmDrive[] = {
    {Target::ModulatorFeedback, 0, 8},
    {Target::CarrierFeedback, 0, 8},
    {Target::Fold, 0, 10},
};

inline constexpr TargetRange kRingmodBrightness[] = {
    {Target::LpfFrequency, -8, 8, Needs::LpfInUse},
    {Target::PulseWidth, -6, 6},
};

inline constexpr TargetRange kDx7Brightness[] = {{Target::LpfFrequency, -10, 0, Needs::LpfInUse}};
inline constexpr TargetRange kDx7Decay[] = {
    {Target::Env1Release, -6, 0},
    {Target::Env1Decay, -6, 0, Needs::SustainBelowFull},
    {Target::Env2Decay, -6, 0, Needs::Env2InUse},
};
inline constexpr TargetRange kDx7Drive[] = {{Target::Fold, 0, 10}};
inline constexpr TargetRange kDx7Thin[] = {{Target::HpfFrequency, 0, 20, Needs::HpfInUse}};

// Factory kit rows are one-shots with sustain 50 and the LPF open, so Brightness only darkens and Decay only
// shortens. Their Drive (bitcrush and SRR) is a genre opt-in no genre takes yet, and Wobble is the Kit's.
inline constexpr TargetRange kKitRowBrightness[] = {{Target::LpfFrequency, -15, 0, Needs::LpfInUse}};
inline constexpr TargetRange kKitRowDecay[] = {
    {Target::Env1Sustain, -6, 0},
    {Target::Env1Decay, -6, 0},
};
inline constexpr TargetRange kReverbOnly[] = {{Target::ReverbSend, 0, 10}};

inline constexpr TargetRange kKitLevelBrightness[] = {{Target::LpfFrequency, -20, 0}};
inline constexpr TargetRange kKitLevelWobble[] = {{Target::ModFxDepth, -8, 8, Needs::ModFxInUse}};
inline constexpr TargetRange kKitLevelThin[] = {{Target::HpfFrequency, 0, 25}};

inline constexpr int32_t kNumEngines = 6;
inline constexpr std::span<const TargetRange> kNone{};

/// Indexed by engine, then by control.
inline constexpr std::span<const TargetRange> kTable[kNumEngines][kNumSoundControls] = {
    // Brightness, Decay, Drive, Space, Wobble, Thin
    {kSubtractiveBrightness, kDecay, kSubtractiveDrive, kSpace, kWobble, kThin},      // Subtractive
    {kFmBrightness, kDecay, kFmDrive, kSpace, kWobble, kNone},                        // FM: no HPF on factory FM
    {kRingmodBrightness, kDecay, kSubtractiveDrive, kSpace, kWobble, kThin},          // Ringmod: fold and ladder morph
    {kDx7Brightness, kDx7Decay, kDx7Drive, kSpace, kWobble, kDx7Thin},                // DX7
    {kKitRowBrightness, kKitRowDecay, kNone, kReverbOnly, kNone, kThin},              // Kit row
    {kKitLevelBrightness, kNone, kNone, kReverbOnly, kKitLevelWobble, kKitLevelThin}, // Kit level
};

} // namespace mapping

/// The targets `control` moves on `engine`, from the spec §11.2 table. Empty where a control does nothing there.
constexpr std::span<const TargetRange> controlTargets(SoundControl control, Engine engine) {
	return mapping::kTable[static_cast<int32_t>(engine)][static_cast<int32_t>(control)];
}

/// The most params `target` expands into on one set of controls, with up to `cables` patch cables: each of two
/// oscillators, modulators or carriers, each of four LFOs, every cable from an LFO.
constexpr int32_t maxParams(Target target, int32_t cables) {
	switch (target) {
	case Target::WaveIndex:
	case Target::PulseWidth:
	case Target::ModulatorFeedback:
	case Target::CarrierFeedback:
		return 2;
	case Target::LfoRate:
		return 4;
	case Target::LfoCables:
		return cables;
	default:
		return 1;
	}
}

/// The most params one set of controls can move, on the engine whose targets expand into the most (spec §11.2).
/// The most params one control moves on a set, over every engine.
constexpr int32_t maxParamsFor(SoundControl control, int32_t cables) {
	int32_t most = 0;
	for (int32_t engine = 0; engine < mapping::kNumEngines; engine++) {
		int32_t params = 0;
		for (const TargetRange& range : controlTargets(control, static_cast<Engine>(engine))) {
			params += maxParams(range.target, cables);
		}
		most = params > most ? params : most;
	}
	return most;
}

constexpr int32_t maxParamsPerSet(int32_t cables) {
	int32_t most = 0;
	for (int32_t engine = 0; engine < mapping::kNumEngines; engine++) {
		int32_t params = 0;
		for (int32_t control = 0; control < kNumSoundControls; control++) {
			for (const TargetRange& range :
			     controlTargets(static_cast<SoundControl>(control), static_cast<Engine>(engine))) {
				params += maxParams(range.target, cables);
			}
		}
		most = params > most ? params : most;
	}
	return most;
}

/// Param units per menu unit for a standard 0–50 param, as the stock menus scale them (2^31 / 25, rounded down).
constexpr int64_t kParamUnitsPerMenuUnit = 85899345;
/// A patch cable's amount runs from -kCableAmountFull to kCableAmountFull.
constexpr int32_t kCableAmountFull = 1 << 30;

/// The value `range`'s target takes `offset` control units (-kControlFull..kControlFull) from `home`, clamped to the
/// param's own range: home plus the share of `low` or `high` the offset reaches, or for cables home × (1 + offset /
/// kControlFull), never 0 from a home that isn't (the firmware deletes a cable set to 0: ×0 gives ±1).
int32_t targetValue(const TargetRange& range, int32_t home, int32_t offset);

/// A player edit's home (spec §11.6): the home from which `offset` plays `value`, the param's new value, clamped to
/// the param's own range. For a linear target, home = value − the share of the range the offset reaches, whether or
/// not the param's range clipped it, so the param keeps the value it was turned to. For cables, value ÷ the scale; at
/// ×0, where every home plays the same, the new amount itself.
int32_t homeFor(const TargetRange& range, int32_t value, int32_t offset);

} // namespace deluge::generative::perform
