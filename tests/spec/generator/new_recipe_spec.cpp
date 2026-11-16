// A new Recipe from a genre (spec §3.1, §3.2).

#include "generative/core/genre.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <algorithm>

using namespace deluge::generative;

// clang-format off
describe new_recipe("A new Recipe from a genre", $ {
	it("rolls its facts in the genre's ranges and starts from the genre's macros", _ {
		// The ranges from the table, so a retune of them needs no edit here.
		const Facts& facts = genres::technoAcid.facts;
		for (uint32_t entropy = 0; entropy < 50; entropy++) {
			Recipe recipe = newRecipe(genres::technoAcid, entropy);
			expect(recipe.genre == &genres::technoAcid).to_be_true();
			expect(recipe.tempo).to_be_between(facts.tempoLowBpm * 100, facts.tempoHighBpm * 100);
			expect(recipe.tempo % 100).to_equal(0);
			expect(std::find(facts.scales.begin(), facts.scales.end(), recipe.scale) != facts.scales.end()).to_be_true();
			expect(recipe.keyRoot).to_be_less_than(uint8_t{12});
			expect(recipe.macros == genres::technoAcid.defaults).to_be_true();
		}
	});

	it("rolls different facts from different entropy", _ {
		bool differs = false;
		Recipe first = newRecipe(genres::technoAcid, 0);
		for (uint32_t entropy = 1; entropy < 20; entropy++) {
			Recipe recipe = newRecipe(genres::technoAcid, entropy);
			differs = differs || recipe.keyRoot != first.keyRoot || recipe.tempo != first.tempo;
		}
		expect(differs).to_be_true();
	});
});

CPPSPEC_SPEC(new_recipe)
