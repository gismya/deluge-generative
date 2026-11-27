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

/// Deep-close reach below home in menu units at -(kControlFull + kCloseControlExtra) (spec §5.6, §11.2): a handover
/// overlap closes Brightness past -kControlFull so the leaving part's filter or FM modulators close audibly even on a
/// preset whose normal Brightness range is scaled down (e.g. kHalfBrightness). 0 for targets that don't close further.
constexpr int32_t closeMenuUnits(Target target) {
	switch (target) {
	case Target::LpfFrequency:
		return 32;
	case Target::Modulator1Level:
	case Target::Modulator2Level:
		return 30;
	case Target::Treble:
	case Target::TrebleFreq:
		return 25;
	case Target::WaveIndex:
	case Target::PulseWidth:
		return 12;
	default:
		return 0;
	}
}

/// The share of `range`'s end on the offset's side, in param units: low is 0 or below, high 0 or above. Below
/// -kControlFull (a handover's deep Brightness close, spec §5.6), a moving negative target (or a deep-close-only EQ
/// target) extends linearly from `low` to -max(-low, closeMenuUnits(target)) at -(kControlFull + kCloseControlExtra).
/// Worked out with no 64-bit division (the Cortex-A9 divides in software, for every param a step moves): the offset
/// makes hundredths of a menu unit, and kParamUnitsPerMenuUnit is 858993 * 100 + 45.
int64_t share(const TargetRange& range, int32_t offset) {
	static_assert(kControlFull == 100 && kCloseControlExtra == 100 && kParamUnitsPerMenuUnit == 858993 * 100 + 45);
	int32_t menuHundredths = 0;
	if (offset < -kControlFull && (range.low < 0 || isDeepCloseOnly(range.target))) {
		const int32_t extra = -offset - kControlFull;
		const int32_t deep = std::max<int32_t>(-range.low, closeMenuUnits(range.target));
		menuHundredths = range.low * kControlFull - (deep + range.low) * extra;
	}
	else {
		menuHundredths = offset < 0 ? -range.low * offset : range.high * offset;
	}
	return int64_t{menuHundredths} * 858993 + menuHundredths * 45 / 100;
}

/// A cable's scale is a percent of its home amount: this is ×1.
constexpr int32_t kCableHomePercent = 100;
/// A cable's TargetRange counts steps of this many percent of its home, so -50..50 reaches ×0 and ×2.
constexpr int32_t kCableStepPercent = 2;

/// A cable's scale at `offset`, in percent of its home: home plus the share of its range the offset reaches, never
/// below 0.
int32_t cableScale(const TargetRange& range, int32_t offset) {
	const int32_t reach = offset < 0 ? range.low * -offset : range.high * offset;
	return std::max<int32_t>(kCableHomePercent + kCableStepPercent * reach / kControlFull, 0);
}

} // namespace

int32_t targetValue(const TargetRange& range, int32_t home, int32_t offset) {
	if (range.target == Target::LfoCables) {
		offset = std::clamp<int32_t>(offset, -kControlFull, kControlFull);
		const int64_t scaled = int64_t{home} * cableScale(range, offset) / kCableHomePercent;
		if (scaled == 0 && home != 0) {
			// The firmware deletes a cable whose amount is set to 0, so ×0 leaves it one step from 0.
			return home > 0 ? 1 : -1;
		}
		return static_cast<int32_t>(std::clamp<int64_t>(scaled, -kCableAmountFull, kCableAmountFull));
	}
	offset = std::clamp<int32_t>(offset, -(kControlFull + kCloseControlExtra), kControlFull);
	return static_cast<int32_t>(std::clamp<int64_t>(home + share(range, offset), INT32_MIN, INT32_MAX));
}

int32_t throwDelayValue(const TargetRange& range, int32_t home, int32_t offset, int32_t depth, int32_t percent) {
	const int32_t other = targetValue(range, home, offset);
	if (percent <= 0) {
		return other;
	}
	const int64_t cap = int64_t{INT32_MIN} + int64_t{kThrowDelayCap} * kParamUnitsPerMenuUnit;
	const int64_t spike = std::max<int64_t>(home, std::min<int64_t>(home + depth * kParamUnitsPerMenuUnit, cap));
	// While it falls, Space's offset still holds the Throw's own spike, so Space's delay share can reach past the
	// spike (past the cap, or past a home above it): the fall comes down from the spike, never up from it.
	const int64_t below = std::min<int64_t>(other, spike);
	// The level in 128ths, so the blend shifts instead of dividing 64 bits (see share()): 100% is exactly 128.
	const int64_t weight = std::min<int32_t>(percent, 100) * 128 / 100;
	return static_cast<int32_t>(below + ((spike - below) * weight >> 7));
}

int32_t homeFor(const TargetRange& range, int32_t value, int32_t offset) {
	if (range.target == Target::LfoCables) {
		offset = std::clamp<int32_t>(offset, -kControlFull, kControlFull);
		const int32_t scale = cableScale(range, offset);
		if (scale == 0) {
			return value;
		}
		const int64_t home = int64_t{value} * kCableHomePercent / scale;
		return static_cast<int32_t>(std::clamp<int64_t>(home, -kCableAmountFull, kCableAmountFull));
	}
	offset = std::clamp<int32_t>(offset, -(kControlFull + kCloseControlExtra), kControlFull);
	return static_cast<int32_t>(std::clamp<int64_t>(value - share(range, offset), INT32_MIN, INT32_MAX));
}

int32_t throwDelayHomeFor(const TargetRange& range, int32_t value, int32_t offset, int32_t rest, int32_t percent) {
	return homeFor(range, value, percent > 0 ? rest : offset);
}

} // namespace deluge::generative::perform
