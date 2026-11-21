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

// Capture's timing (spec §17.6): the tempo and swing a replayed piece moves through, from its log's glides, and a
// note's swing difference, built into its tick as Lean is. Portable and integer-only (ADR 0004), so the harness checks
// it.

#pragma once

#include "generative/core/types.h"
#include "generative/perform/glide.h"
#include <cstdint>

namespace deluge::generative::compose {

/// A value gliding over Generator ticks from where it was to a target, as a logged glide moves it: the tempo or the
/// swing amount. Integer math, as perform::Glide's.
class GlidePlan {
public:
	/// Lands at once on `value`, from tick 0.
	void set(int32_t value) {
		from_ = to_ = value;
		start_ = length_ = 0;
	}
	/// A glide to `to` over `length` ticks starts at `tick` (0: at once), from the value there.
	void glide(int32_t tick, int32_t to, int32_t length) {
		from_ = at(tick);
		to_ = to;
		start_ = tick;
		length_ = length > 0 ? length : 0;
	}
	/// The value at `tick`, at or after the last glide's start.
	[[nodiscard]] int32_t at(int32_t tick) const {
		if (length_ == 0 || tick >= start_ + length_) {
			return to_;
		}
		return tick <= start_ ? from_ : perform::lerpOver(from_, to_, tick - start_, length_);
	}
	/// Whether a glide still moves the value at `tick`.
	[[nodiscard]] bool moving(int32_t tick) const { return length_ > 0 && tick > start_ && tick < start_ + length_; }
	[[nodiscard]] int32_t target() const { return to_; }
	/// Where the last glide ends.
	[[nodiscard]] int32_t endTick() const { return start_ + length_; }

private:
	int32_t from_ = 0;
	int32_t to_ = 0;
	int32_t start_ = 0;
	int32_t length_ = 0;
};

/// The ticks a swing pair spans, for a swing interval (a note value: 0 the 16th, each step doubling): two of its notes.
constexpr int32_t swingPairTicks(int32_t interval) {
	return (2 * kTicksPerStep) << interval;
}

/// Where swung tick `tick` sounds, at swing `amount` on `interval`, in fiftieths of an unswung tick: the first note of
/// each pair is stretched by (50 + amount) / 50 and the second squeezed, as the song's clock swings them.
constexpr int64_t swingTime50(int32_t tick, int32_t amount, int32_t interval) {
	const int32_t pair = swingPairTicks(interval);
	const int32_t in = tick % pair;
	const int64_t start = int64_t{tick - in} * 50;
	if (in <= pair / 2) {
		return start + int64_t{in} * (50 + amount);
	}
	return start + int64_t{pair} * 50 - int64_t{pair - in} * (50 - amount);
}

/// The song's tick, at swing `songAmount` on `songInterval`, that sounds where `tick` did at `amount` on `interval`,
/// rounded to the nearest: a note played at another swing gets the difference built into its tick (spec §17.6).
constexpr int32_t swingShifted(int32_t tick, int32_t amount, int32_t interval, int32_t songAmount,
                               int32_t songInterval) {
	if (tick < 0 || (amount == songAmount && interval == songInterval)) {
		return tick;
	}
	const int64_t time = swingTime50(tick, amount, interval);
	const int32_t pair = swingPairTicks(songInterval);
	const int64_t pairTime = int64_t{pair} * 50;
	const int64_t start = time / pairTime * pair;
	const int64_t in = time - start * 50;
	const int64_t firstHalf = int64_t{pair / 2} * (50 + songAmount);
	int64_t shifted = 0;
	if (in <= firstHalf) {
		const int32_t stretch = 50 + songAmount;
		shifted = (in + stretch / 2) / stretch;
	}
	else {
		const int32_t squeeze = 50 - songAmount;
		shifted = pair - (pairTime - in + squeeze / 2) / squeeze;
	}
	return static_cast<int32_t>(start + shifted);
}

} // namespace deluge::generative::compose
