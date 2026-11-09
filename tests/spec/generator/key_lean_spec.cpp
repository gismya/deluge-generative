// The key lean of a genre change or Regenerate (spec §9.7): the scale is rolled first, then the related roots of the
// playing key share 65% of the root roll and the rest is spread over all 12 roots.

#include "generative/core/genre.h"
#include "generative/core/pitch.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <array>

using namespace deluge::generative;

namespace {

constexpr uint8_t C = 0, D = 2, E = 4, A = 9, B = 11;

uint16_t bits(std::initializer_list<uint8_t> roots) {
	uint16_t mask = 0;
	for (uint8_t root : roots) {
		mask |= static_cast<uint16_t>(1U << root);
	}
	return mask;
}

} // namespace

// clang-format off
describe key_lean("The key lean of a Regenerate", $ {
	it("names the roots whose key shares 6 or 7 notes with the playing key", _ {
		// The spec's example: A Minor into a Minor-only genre favours A, D and E Minor.
		expect(relatedRoots({A, Scale::Minor}, Scale::Minor)).to_equal(bits({A, D, E}));
		// E Phrygian has A Minor's notes; A Phrygian (one flat more) and B Phrygian (one sharp more) share 6.
		expect(relatedRoots({A, Scale::Minor}, Scale::Phrygian)).to_equal(bits({E, A, B}));
		// C Major is A Minor's relative: the same 7 notes.
		expect(relatedRoots({A, Scale::Minor}, Scale::Major) & bits({C})).to_equal(bits({C}));
	});

	it("gives the related roots 65% of the roll, split evenly, and the rest to all 12", _ {
		std::array<int32_t, 12> minorRoots{};
		int32_t minorRolls = 0;
		const Key playing{A, Scale::Minor};
		for (uint32_t entropy = 0; entropy < 24000; entropy++) {
			const Recipe recipe = newRecipe(genres::techno, entropy, playing);
			if (recipe.scale == Scale::Minor) {
				minorRoots[recipe.keyRoot]++;
				minorRolls++;
			}
		}
		// Each of A, D and E: 65% / 3 + 35% / 12 = 24.58%; every other root 35% / 12 = 2.92%.
		for (int32_t root = 0; root < 12; root++) {
			const int32_t perMille = minorRoots[root] * 1000 / minorRolls;
			if (root == A || root == D || root == E) {
				expect(perMille).to_be_between(230, 262);
			}
			else {
				expect(perMille).to_be_between(20, 39);
			}
		}
	});

	it("rolls every root evenly with no playing key", _ {
		std::array<int32_t, 12> roots{};
		for (uint32_t entropy = 0; entropy < 12000; entropy++) {
			roots[newRecipe(genres::techno, entropy).keyRoot]++;
		}
		for (int32_t count : roots) {
			expect(count).to_be_between(880, 1120);
		}
	});
});

CPPSPEC_SPEC(key_lean)
