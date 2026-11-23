// A new Recipe from a genre (spec §3.1, §3.2).

#include "generative/core/draw.h"
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

	// Swing is a fact range, rolled with tempo (ADR 0008, #498).
	it("rolls swing in each genre's range, reaching both ends, and keeps every range in the song's -49..49", _ {
		for (const Genre* genre : allGenres()) {
			const Facts& facts = genre->facts;
			expect(facts.swingLow).to_be_between(int8_t{-kMaxSwing}, facts.swingHigh);
			expect(facts.swingHigh).to_be_between(facts.swingLow, int8_t{kMaxSwing});
			bool low = false;
			bool high = false;
			for (uint32_t entropy = 0; entropy < 1000; entropy++) {
				const Recipe recipe = newRecipe(*genre, entropy);
				expect(recipe.swing).to_be_between(facts.swingLow, facts.swingHigh);
				low = low || recipe.swing == facts.swingLow;
				high = high || recipe.swing == facts.swingHigh;
			}
			expect(low && high).to_be_true();
		}
	});

	it("draws swing fourth from the entropy, after tempo, root and scale, so they roll as before", _ {
		for (const Genre* genre : allGenres()) {
			const Facts& facts = genre->facts;
			for (uint32_t entropy = 0; entropy < 100; entropy++) {
				EntropyStream rolls(entropy);
				const int32_t tempoSpan = facts.tempoHighBpm - facts.tempoLowBpm + 1;
				const int32_t tempo = (facts.tempoLowBpm + rolls.below(tempoSpan)) * 100;
				const uint32_t root = rolls.next();
				rolls.next(); // the scale
				const int32_t swing = facts.swingLow + rolls.below(facts.swingHigh - facts.swingLow + 1);
				const Recipe recipe = newRecipe(*genre, entropy);
				expect(recipe.tempo).to_equal(tempo);
				expect(int32_t{recipe.keyRoot}).to_equal(scaleBelow(root, 12));
				expect(int32_t{recipe.swing}).to_equal(swing);
				// A lean moves only the key: the swing is the same roll.
				const Recipe leaned = newRecipe(*genre, entropy, Key{2, Scale::Minor});
				expect(leaned.swing).to_equal(recipe.swing);
			}
		}
	});
});

CPPSPEC_SPEC(new_recipe)
