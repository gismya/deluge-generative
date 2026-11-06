// The Recipe a tuning tool plays (spec §21.5): the rough render and the delugemu capture both take genre, Recipe
// overrides and seed, so the same arguments give the same Recipe in both.

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/recipe_overrides.h"

#include "cppspec.hpp"

using namespace deluge::generative;

// clang-format off
describe recipe_overrides("Recipe overrides", $ {
	it("with none set, plays the genre's new Recipe from entropy 0 at seed 1", _ {
		RolledRecipe rolled = rollWithOverrides(genres::techno, RecipeOverrides{});
		Recipe fresh = newRecipe(genres::techno, 0);
		expect(rolled.recipe.genre == &genres::techno).to_be_true();
		expect(rolled.recipe.tempo).to_equal(fresh.tempo);
		expect(rolled.recipe.keyRoot).to_equal(fresh.keyRoot);
		expect(rolled.recipe.scale == fresh.scale).to_be_true();
		expect(rolled.recipe.macros == genres::techno.defaults).to_be_true();
		expect(rolled.seed).to_equal(1U);
	});

	it("sets the seed, the facts and each macro it names, in OLED points", _ {
		RecipeOverrides overrides{
		    .seed = 77,
		    .macroPoints = {40, -1, 10, -1},
		    .tempo = 13250,
		    .keyRoot = 2,
		    .scale = static_cast<int8_t>(Scale::Phrygian),
		    .swing = 12,
		};
		RolledRecipe rolled = rollWithOverrides(genres::techno, overrides);
		expect(rolled.seed).to_equal(77U);
		expect(rolled.recipe.macros[Macro::Energy]).to_equal(40 * 64);
		expect(rolled.recipe.macros[Macro::Density]).to_equal(genres::techno.defaults[Macro::Density]);
		expect(rolled.recipe.macros[Macro::Complexity]).to_equal(10 * 64);
		expect(rolled.recipe.macros[Macro::Motion]).to_equal(genres::techno.defaults[Macro::Motion]);
		expect(rolled.recipe.tempo).to_equal(13250);
		expect(rolled.recipe.keyRoot).to_equal(uint8_t{2});
		expect(rolled.recipe.scale == Scale::Phrygian).to_be_true();
		expect(rolled.recipe.swing).to_equal(int8_t{12});
	});

	it("rolls the facts from the entropy it names", _ {
		RolledRecipe rolled = rollWithOverrides(genres::techno, RecipeOverrides{.entropy = 5});
		Recipe fresh = newRecipe(genres::techno, 5);
		expect(rolled.recipe.tempo).to_equal(fresh.tempo);
		expect(rolled.recipe.keyRoot).to_equal(fresh.keyRoot);
	});

	it("plays the genre it names by stable name, and the fallback genre for a name it doesn't know", _ {
		expect(rollWithOverrides(genres::techno, RecipeOverrides{.genre = "techno"}).recipe.genre == &genres::techno)
		    .to_be_true();
		expect(rollWithOverrides(genres::techno, RecipeOverrides{.genre = "polka"}).recipe.genre == &genres::techno)
		    .to_be_true();
	});
});

CPPSPEC_SPEC(recipe_overrides)
