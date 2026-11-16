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

// Tempo and swing glides (spec §12.7, §13.3): the Glide setting, and a value moving along a line over Generator ticks.

#pragma once

#include "generative/core/types.h"
#include <algorithm>
#include <cstdint>

namespace deluge::generative::perform {

/// The Perform Glide setting: how long tempo and swing glides take (spec §13.3). Kept until power-off.
enum class GlideLength : uint8_t { Instant, Beat, Bar, TwoBars, FourBars, Phrase };
constexpr GlideLength kDefaultGlide = GlideLength::Bar;

/// A Glide setting's length in Generator ticks, for a genre whose phrase is `phraseSteps` long.
constexpr int32_t glideTicks(GlideLength length, int32_t phraseSteps) {
	constexpr int32_t kBar = kStepsPerBar * kTicksPerStep;
	switch (length) {
	case GlideLength::Instant:
		return 0;
	case GlideLength::Beat:
		return kBar / 4;
	case GlideLength::Bar:
		return kBar;
	case GlideLength::TwoBars:
		return 2 * kBar;
	case GlideLength::FourBars:
		return 4 * kBar;
	case GlideLength::Phrase:
		return phraseSteps * kTicksPerStep;
	}
	return kBar;
}

/// A value moving along a line from where it is to a target, over Generator ticks that played. Integer math.
class Glide {
public:
	/// Lands at once on `value`.
	void set(int32_t value) {
		from_ = to_ = value;
		elapsed_ = length_ = 0;
		running_ = false;
	}
	/// Starts from the current value towards `to` over `ticks` (0 lands at the next update).
	void start(int32_t to, int32_t ticks) {
		from_ = value();
		to_ = to;
		length_ = ticks > 0 ? ticks : 0;
		elapsed_ = 0;
		running_ = true;
	}
	/// Moves on by `ticks` that played; it stops running once it reaches the target.
	void advance(int32_t ticks) {
		elapsed_ = elapsed_ + ticks < length_ ? elapsed_ + ticks : length_;
		running_ = elapsed_ < length_;
	}
	/// Lands on the target.
	void finish() { set(to_); }
	[[nodiscard]] bool running() const { return running_; }
	[[nodiscard]] int32_t target() const { return to_; }
	[[nodiscard]] int32_t value() const {
		// 32 bits hold it: a tempo difference under 10^5 hundredths times a glide under 10^4 ticks.
		return length_ == 0 ? to_ : from_ + (to_ - from_) * elapsed_ / length_;
	}
	/// The value `ticks` from now, as it runs on.
	[[nodiscard]] int32_t valueIn(int32_t ticks) const {
		const int32_t elapsed = std::min(elapsed_ + std::max<int32_t>(ticks, 0), length_);
		return length_ == 0 ? to_ : from_ + (to_ - from_) * elapsed / length_;
	}

private:
	int32_t from_ = 0;
	int32_t to_ = 0;
	int32_t length_ = 0;
	int32_t elapsed_ = 0;
	bool running_ = false;
};

} // namespace deluge::generative::perform
