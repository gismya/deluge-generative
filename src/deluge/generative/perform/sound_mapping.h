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

/// The targets `control` moves on `engine`, from the spec §11.2 table. Empty where a control does nothing there.
std::span<const TargetRange> controlTargets(SoundControl control, Engine engine);

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
