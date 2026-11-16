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

/// The share of `range`'s end on the offset's side, in param units: low is 0 or below, high 0 or above. Worked out
/// with no 64-bit division (the Cortex-A9 divides in software, for every param a step moves): the offset makes
/// hundredths of a menu unit, and kParamUnitsPerMenuUnit is 858993 * 100 + 45.
int64_t share(const TargetRange& range, int32_t offset) {
	static_assert(kControlFull == 100 && kParamUnitsPerMenuUnit == 858993 * 100 + 45);
	const int32_t menuHundredths = offset < 0 ? -range.low * offset : range.high * offset;
	return int64_t{menuHundredths} * 858993 + menuHundredths * 45 / 100;
}

} // namespace

int32_t targetValue(const TargetRange& range, int32_t home, int32_t offset) {
	offset = std::clamp<int32_t>(offset, -kControlFull, kControlFull);
	if (range.target == Target::LfoCables) {
		const int64_t scaled = int64_t{home} * (kControlFull + offset) / kControlFull;
		if (scaled == 0 && home != 0) {
			// The firmware deletes a cable whose amount is set to 0, so ×0 leaves it one step from 0.
			return home > 0 ? 1 : -1;
		}
		return static_cast<int32_t>(std::clamp<int64_t>(scaled, -kCableAmountFull, kCableAmountFull));
	}
	return static_cast<int32_t>(std::clamp<int64_t>(home + share(range, offset), INT32_MIN, INT32_MAX));
}

int32_t homeFor(const TargetRange& range, int32_t value, int32_t offset) {
	offset = std::clamp<int32_t>(offset, -kControlFull, kControlFull);
	if (range.target == Target::LfoCables) {
		if (offset == -kControlFull) {
			return value;
		}
		const int64_t home = int64_t{value} * kControlFull / (kControlFull + offset);
		return static_cast<int32_t>(std::clamp<int64_t>(home, -kCableAmountFull, kCableAmountFull));
	}
	return static_cast<int32_t>(std::clamp<int64_t>(value - share(range, offset), INT32_MIN, INT32_MAX));
}

} // namespace deluge::generative::perform
