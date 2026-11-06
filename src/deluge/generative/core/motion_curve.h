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

// Motion curves (spec §3.5). Each rate Motion moves has a value at Motion 25, which the genre sets, and a curve that
// scales it: R, the multiple at Motion 50, and the start z, the multiple just above Motion 0. In between, the multiple
// is z + (1 − z)(c^(M/25) − 1)/(c − 1) with c = (R − 1)/(1 − z). Motion 0 is "never" on every curve.
//
// Each curve is a table of multiples per Motion point, built by the compiler from R and the start in integer fixed
// point, so the firmware (built with -ffast-math), Compose and the harness read the same integers (ADR 0004). A
// harness spec checks each table against the formula.

#pragma once

#include "generative/core/recipe.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

struct Fraction {
	int32_t num = 0;
	int32_t den = 1;
};

/// A multiple of 1 in the tables, in fixed point.
constexpr int32_t kCurveOne = 1 << 16;
/// One table entry per Motion point, 0 to 50.
constexpr int32_t kCurvePoints = kMacroPoints + 1;

struct MotionCurve {
	Fraction r;
	Fraction start;
	/// The formula's multiple at each Motion point, in kCurveOne units. Entry 0 holds the start, the value just above
	/// Motion 0; Motion 0 itself is 0 (motionMultiple()).
	int32_t table[kCurvePoints] = {};
};

namespace detail {

// Build-time math in Q40 on int64: values stay below 16, so no step overflows.
constexpr int32_t kQBits = 40;
constexpr int64_t kQOne = int64_t{1} << kQBits;

/// a × b in Q40 for 0 ≤ a, b < 16, without a 128-bit product: each operand splits into 22-bit halves.
consteval int64_t mulQ(int64_t a, int64_t b) {
	constexpr int64_t kLowMask = (int64_t{1} << 22) - 1;
	int64_t ah = a >> 22;
	int64_t al = a & kLowMask;
	int64_t bh = b >> 22;
	int64_t bl = b & kLowMask;
	// a·b = ah·bh·2^44 + (ah·bl + al·bh)·2^22 + al·bl, shifted down by 40.
	return (ah * bh << 4) + ((ah * bl + al * bh) >> 18) + ((al * bl) >> kQBits);
}

/// Integer division rounded to nearest, for any signs.
consteval int64_t divRound(int64_t a, int64_t b) {
	if (b < 0) {
		a = -a;
		b = -b;
	}
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

/// x^25 compared with `target`: stops multiplying once it passes the target, so nothing overflows.
consteval bool pow25Exceeds(int64_t x, int64_t target) {
	int64_t acc = kQOne;
	for (int32_t i = 0; i < 25; i++) {
		acc = mulQ(acc, x);
		if (acc > target) {
			return true;
		}
	}
	return false;
}

/// The 25th root of `c` (Q40), by bisection.
consteval int64_t root25(int64_t c) {
	int64_t low = 0;
	int64_t high = 2 * kQOne; // 2^25 is far above any c a curve uses
	while (high - low > 1) {
		int64_t mid = low + (high - low) / 2;
		if (pow25Exceeds(mid, c)) {
			high = mid;
		}
		else {
			low = mid;
		}
	}
	return low;
}

} // namespace detail

/// A curve's table, built from R (> 1) and the start (0 ≤ z < 1).
consteval MotionCurve makeMotionCurve(Fraction r, Fraction start) {
	using namespace detail;
	if (r.den <= 0 || start.den <= 0 || r.num <= r.den || start.num < 0 || start.num >= start.den) {
		throw "a Motion curve needs R > 1 and a start in [0, 1)";
	}
	MotionCurve curve{r, start};
	const int64_t rn = r.num;
	const int64_t rd = r.den;
	const int64_t zn = start.num;
	const int64_t zd = start.den;
	const int64_t z = divRound(zn * kQOne, zd);
	// c − 1 = (R − 2 + z) / (1 − z), so (1 − z)/(c − 1) = (1 − z)² / (R − 2 + z): the ratio num/den below.
	const int64_t num = (zd - zn) * (zd - zn) * rd;
	const int64_t den = zd * (rn * zd - 2 * rd * zd + zn * rd);
	const int64_t c = divRound((rn - rd) * zd * kQOne, rd * (zd - zn));
	if (c >= 4 * kQOne) {
		throw "c^2, reached at Motion 50, must stay below 16 in the build-time math";
	}
	const int64_t step = den == 0 ? 0 : root25(c); // c^(1/25)
	int64_t power = kQOne;                         // c^(M/25)
	for (int32_t m = 0; m < kCurvePoints; m++) {
		int64_t multiple;
		if (den == 0) {
			multiple = z + divRound((kQOne - z) * m, 25); // c = 1: the formula's limit is a straight line
		}
		else {
			multiple = z + divRound((power - kQOne) * num, den);
		}
		curve.table[m] = static_cast<int32_t>(divRound(multiple, kQOne / kCurveOne));
		power = mulQ(power, step);
	}
	return curve;
}

/// The Generator-wide curves (spec §3.5, tuning).
namespace curves {
constexpr MotionCurve grooveLength = makeMotionCurve({3, 1}, {1, 3});
constexpr MotionCurve fill = makeMotionCurve({5, 2}, {1, 20}); ///< Fill chance and section-end chance.
constexpr MotionCurve phraseAccent = makeMotionCurve({5, 2}, {1, 20});
constexpr MotionCurve loopMutation = makeMotionCurve({3, 1}, {0, 1});
constexpr MotionCurve answers = makeMotionCurve({2, 1}, {1, 4});
constexpr MotionCurve dropOut = makeMotionCurve({2, 1}, {0, 1});
constexpr MotionCurve patternRepick = makeMotionCurve({2, 1}, {0, 1});
constexpr MotionCurve motifReroll = makeMotionCurve({2, 1}, {0, 1});
} // namespace curves

/// Every curve, for the spec that checks them.
inline std::span<const MotionCurve* const> allMotionCurves() {
	static constexpr const MotionCurve* kAll[] = {
	    &curves::grooveLength, &curves::fill,    &curves::phraseAccent,  &curves::loopMutation,
	    &curves::answers,      &curves::dropOut, &curves::patternRepick, &curves::motifReroll,
	};
	return kAll;
}

/// The curve's multiple at `motion` (macro units), in kCurveOne units: 0 at Motion 0, interpolated between points.
constexpr int32_t motionMultiple(const MotionCurve& curve, int32_t motion) {
	if (motion <= 0) {
		return 0;
	}
	if (motion >= kMacroFull) {
		return curve.table[kMacroPoints];
	}
	int32_t point = motion / kMacroUnitsPerPoint;
	int32_t fraction = motion % kMacroUnitsPerPoint;
	int32_t low = curve.table[point];
	int32_t high = curve.table[point + 1];
	return low + (high - low) * fraction / kMacroUnitsPerPoint;
}

/// A chance (per-mille at Motion 25) at `motion`: times the curve's multiple, clamped at 100%.
constexpr int32_t scaleChance(int32_t perMilleAt25, const MotionCurve& curve, int32_t motion) {
	int64_t scaled = (static_cast<int64_t>(perMilleAt25) * motionMultiple(curve, motion) + kCurveOne / 2) / kCurveOne;
	return scaled > 1000 ? 1000 : static_cast<int32_t>(scaled);
}

} // namespace deluge::generative
