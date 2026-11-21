// Part bias's curve (spec §3.3): the 50/50 blend of the linear curve (clamped) and the headroom-proportional one. The
// expected values are worked by hand from the spec's formula.

#include "generative/core/part_bias.h"

#include "cppspec.hpp"

#include <cstdint>

using namespace deluge::generative;

namespace {

constexpr int32_t pts(int32_t points) {
	return macroPoints(points);
}

} // namespace

// clang-format off
describe part_bias_curve("Part bias curve", $ {
	it("is the identity at bias 0", _ {
		for (int32_t v = 0; v <= kMacroFull; v++) {
			expect(partBiased(v, 0)).to_equal(v);
		}
	});

	it("is the mean of the linear and the headroom-proportional curves", _ {
		expect(partBiased(pts(25), pts(8))).to_equal(pts(31));              // (33 + 29) / 2
		expect(partBiased(pts(25), -pts(8))).to_equal(pts(19));             // (17 + 21) / 2
		expect(partBiased(pts(20), pts(15))).to_equal(pts(32));             // (35 + 29) / 2
		expect(partBiased(pts(10), -pts(5))).to_equal(pts(7));              // (5 + 9) / 2
	});

	it("eases into the end the bias heads for", _ {
		expect(partBiased(pts(45), pts(5))).to_equal(pts(47) + 3 * pts(1) / 4); // (50 + 45.5) / 2
		expect(partBiased(pts(5), -pts(5))).to_equal(pts(2) + pts(1) / 4);      // (0 + 4.5) / 2
		expect(partBiased(pts(40), pts(20))).to_equal(pts(47));                 // (50 + 44) / 2
		expect(partBiased(pts(30), pts(30))).to_equal(pts(46));                 // (50 + 42) / 2
	});

	it("reaches each end with the shared value, and stays within 0..50", _ {
		for (int32_t o = -kMacroFull; o <= kMacroFull; o += 16) {
			expect(partBiased(0, o)).to_be_between(int32_t{0}, kMacroFull);
			expect(partBiased(kMacroFull, o)).to_be_between(int32_t{0}, kMacroFull);
			if (o > 0) {
				expect(partBiased(kMacroFull, o)).to_equal(kMacroFull);
			}
			if (o < 0) {
				expect(partBiased(0, o)).to_equal(0);
			}
		}
		expect(partBiased(0, kMacroFull)).to_equal(kMacroFull);
		expect(partBiased(kMacroFull, -kMacroFull)).to_equal(0);
	});

	it("clamps a bias past ±50 to ±50", _ {
		expect(partBiased(pts(10), 2 * kMacroFull)).to_equal(partBiased(pts(10), kMacroFull));
		expect(partBiased(pts(10), -2 * kMacroFull)).to_equal(partBiased(pts(10), -kMacroFull));
	});

	it("is monotonic in the shared value and in the bias", _ {
		for (int32_t o = -kMacroFull; o <= kMacroFull; o += 32) {
			for (int32_t v = 1; v <= kMacroFull; v++) {
				expect(partBiased(v, o) >= partBiased(v - 1, o)).to_be_true();
			}
		}
		for (int32_t v = 0; v <= kMacroFull; v += 32) {
			for (int32_t o = -kMacroFull + 1; o <= kMacroFull; o++) {
				expect(partBiased(v, o) >= partBiased(v, o - 1)).to_be_true();
			}
		}
	});

	it("moves by one unit at most for one unit of the shared value", _ {
		for (int32_t o = 1; o <= kMacroFull; o += 7) {
			for (int32_t v = 1; v <= kMacroFull; v++) {
				const int32_t up = partBiased(v, o) - partBiased(v - 1, o);
				const int32_t down = partBiased(v, -o) - partBiased(v - 1, -o);
				expect(up >= 0 && up <= 1).to_be_true();
				expect(down >= 0 && down <= 1).to_be_true();
			}
		}
	});

	context("biasFor()", _ {
		it("is about 4/3 of the difference through the middle", _ {
			expect(biasFor(pts(25), pts(33))).to_equal(683);  // 3/4 of 683 is 512.25: 8 points
			expect(biasFor(pts(25), pts(17))).to_equal(-682);
			expect(biasFor(pts(25), pts(25))).to_equal(0);
		});

		it("finds the smallest bias that puts the part there, near the ends too", _ {
			for (int32_t centre = 0; centre <= kMacroFull; centre += pts(1)) {
				for (int32_t target = 0; target <= kMacroFull; target += pts(1)) {
					const int32_t bias = biasFor(centre, target);
					if (target >= centre) {
						expect(partBiased(centre, bias) >= target).to_be_true();
						expect(bias == 0 || partBiased(centre, bias - 1) < target).to_be_true();
					}
					else {
						expect(partBiased(centre, bias) <= target).to_be_true();
						expect(partBiased(centre, bias + 1) > target).to_be_true();
					}
				}
			}
			expect(biasFor(pts(45), kMacroFull)).to_equal(kMacroFull); // only the whole bias reaches 50 from 45
			expect(biasFor(pts(25), kMacroFull)).to_equal(kMacroFull); // and from 25
		});
	});
});

CPPSPEC_SPEC(part_bias_curve)
