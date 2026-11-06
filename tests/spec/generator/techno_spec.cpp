// Techno through the Perform driver: what the genre's tables and the macros make (spec §4.5, §3.3, §8.1).

#include "harness.h"

#include "cppspec.hpp"

using namespace harness;

// clang-format off
describe techno("Techno through the Perform driver", $ {
	it("plays only the four-on-the-floor kick at Density 0", _ {
		Run run;
		run.recipe = technoRecipe();
		run.recipe.macros.set(Macro::Density, 0);
		Result result = play(run, 8);

		auto ons = only(result.events, EventKind::NoteOn);
		expect(ons.size()).to_equal(size_t{8 * 4});
		for (const Event& e : ons) {
			expect(e.lane).to_equal(Lane::Kick);
			expect(e.tick % (4 * kTicksPerStep)).to_equal(0);
		}
	});
});

CPPSPEC_SPEC(techno)
