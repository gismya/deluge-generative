// Motion curves (spec §3.5): each rate Motion moves is its Motion 25 value times a curve's multiple. The curves are
// integer tables built at compile time, so this spec checks every table against the formula, in floating point.

#include "generative/core/motion_curve.h"

#include "cppspec.hpp"

#include <cmath>
#include <iterator>

using namespace deluge::generative;

namespace {

double value(Fraction f) {
	return static_cast<double>(f.num) / f.den;
}

/// The spec's formula: the multiple at Motion `m` (1..50) for a curve with multiple R at 50 and start z.
double formula(double r, double z, double m) {
	double c = (r - 1) / (1 - z);
	if (std::fabs(c - 1) < 1e-12) {
		return z + (1 - z) * m / 25; // the limit as c → 1: a straight line
	}
	return z + (1 - z) * (std::pow(c, m / 25) - 1) / (c - 1);
}

/// The §3.5 table's curves, in allMotionCurves() order: R and the start.
struct Shape {
	Fraction r;
	Fraction start;
};
constexpr Shape kSpecCurves[] = {
    {{3, 1}, {1, 3}},  // Groove length
    {{5, 2}, {1, 20}}, // Fill chance and section-end chance
    {{5, 2}, {1, 20}}, // Phrase accent chance
    {{3, 1}, {0, 1}},  // Loop mutation
    {{2, 1}, {1, 4}},  // Answers
    {{2, 1}, {0, 1}},  // Drop-out chance
    {{2, 1}, {0, 1}},  // Pattern re-pick chance
    {{2, 1}, {0, 1}},  // Motif re-roll
};

} // namespace

// clang-format off
describe motion_curve("Motion curves", $ {
	it("are the eight curves of the spec's table, with its R and start", _ {
		expect(allMotionCurves().size()).to_equal(std::size(kSpecCurves));
		for (size_t i = 0; i < std::size(kSpecCurves); i++) {
			const MotionCurve& curve = *allMotionCurves()[i];
			expect(value(curve.r)).to_equal(value(kSpecCurves[i].r));
			expect(value(curve.start)).to_equal(value(kSpecCurves[i].start));
		}
	});

	it("match the formula for the spec's R and start at every Motion point, to within one table unit", _ {
		for (size_t i = 0; i < std::size(kSpecCurves) && i < allMotionCurves().size(); i++) {
			const MotionCurve& curve = *allMotionCurves()[i];
			for (int32_t m = 0; m <= kMacroPoints; m++) {
				double expected = formula(value(kSpecCurves[i].r), value(kSpecCurves[i].start), m) * kCurveOne;
				expect(std::fabs(curve.table[m] - expected)).to_be_less_than(1.0);
			}
		}
	});

	it("give 0 at Motion 0, the start just above it, 1 at Motion 25 and R at Motion 50", _ {
		for (const MotionCurve* curve : allMotionCurves()) {
			expect(motionMultiple(*curve, 0)).to_equal(0);
			expect(motionMultiple(*curve, 1)).to_be_between(curve->table[0], curve->table[1]);
			expect(motionMultiple(*curve, macroPoints(25))).to_equal(kCurveOne);
			expect(motionMultiple(*curve, macroPoints(50)))
			    .to_equal(static_cast<int32_t>(int64_t{kCurveOne} * curve->r.num / curve->r.den));
		}
	});

	it("interpolate between Motion points", _ {
		const MotionCurve& curve = curves::grooveLength;
		int32_t low = motionMultiple(curve, macroPoints(10));
		int32_t high = motionMultiple(curve, macroPoints(11));
		int32_t mid = motionMultiple(curve, macroPoints(10) + kMacroUnitsPerPoint / 2);
		expect(mid - (low + high) / 2).to_be_between(-1, 1);
	});

	it("scale Techno's 35% fill chance to 3% at Motion 1 and 88% at Motion 50", _ {
		expect(scaleChance(350, curves::fill, macroPoints(1))).to_be_between(25, 34);
		expect(scaleChance(350, curves::fill, macroPoints(50))).to_be_between(875, 876);
		expect(scaleChance(350, curves::fill, 0)).to_equal(0);
	});

	it("clamp a scaled chance at 100%", _ {
		expect(scaleChance(600, curves::fill, macroPoints(50))).to_equal(1000);
	});

	it("make the pattern re-pick chance a straight line from 0", _ {
		expect(scaleChance(200, curves::patternRepick, macroPoints(10))).to_equal(80);
		expect(scaleChance(200, curves::patternRepick, macroPoints(25))).to_equal(200);
		expect(scaleChance(200, curves::patternRepick, macroPoints(50))).to_equal(400);
	});
});

CPPSPEC_SPEC(motion_curve)
