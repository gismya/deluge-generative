// A new Recipe from a genre (spec §3.1, §3.2).

#include "generative/core/genre.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <cstring>

using namespace deluge::generative;

// clang-format off
describe new_recipe("A new Recipe from a genre", $ {
	it("rolls its facts in the genre's ranges and starts from the genre's macros and first sounds", _ {
		for (uint32_t entropy = 0; entropy < 50; entropy++) {
			Recipe recipe = newRecipe(genres::techno, entropy);
			expect(recipe.genre == &genres::techno).to_be_true();
			expect(recipe.tempo).to_be_between(12500, 13500);
			expect(recipe.tempo % 100).to_equal(0);
			expect(recipe.scale == Scale::Minor || recipe.scale == Scale::Phrygian).to_be_true();
			expect(recipe.keyRoot).to_be_less_than(uint8_t{12});
			expect(recipe.macros == genres::techno.defaults).to_be_true();
			expect(std::strcmp(recipe.sounds.part[1], "SYNTHS/165 Acid Arp.XML")).to_equal(0);
			expect(recipe.sounds.lane[static_cast<int32_t>(Lane::Kick)] != nullptr).to_be_true();
		}
	});

	it("rolls different facts from different entropy", _ {
		bool differs = false;
		Recipe first = newRecipe(genres::techno, 0);
		for (uint32_t entropy = 1; entropy < 20; entropy++) {
			Recipe recipe = newRecipe(genres::techno, entropy);
			differs = differs || recipe.keyRoot != first.keyRoot || recipe.tempo != first.tempo;
		}
		expect(differs).to_be_true();
	});
});

CPPSPEC_SPEC(new_recipe)
