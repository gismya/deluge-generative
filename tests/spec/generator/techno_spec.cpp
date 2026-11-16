// Acid techno through the Perform driver: what the genre's tables and the macros make (spec §4.5, §3.3, §8.1).

#include "harness.h"

#include "cppspec.hpp"

using namespace harness;

// clang-format off
describe techno("Acid techno through the Perform driver", $ {
	it("plays only the four-on-the-floor kick and the Drone's root at Density 0", _ {
		Run run;
		run.recipe = acidRecipe();
		run.recipe.macros.set(Macro::Density, 0);
		Result result = play(run, 8);

		auto ons = only(result.events, EventKind::NoteOn);
		expect(ons.size()).to_equal(size_t{8 * 4 + 1});
		for (const Event& e : ons) {
			if (e.part == 2) { // the Drone's root, an anchor, held from the first step
				expect(e.tick).to_equal(0);
				expect(e.pitch % 12).to_equal(run.recipe.keyRoot);
				continue;
			}
			expect(e.lane).to_equal(Lane::Kick);
			expect(e.tick % (4 * kTicksPerStep)).to_equal(0);
		}
	});
});

CPPSPEC_SPEC(techno)
