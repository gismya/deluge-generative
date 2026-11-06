// Checks every built-in genre's data (spec §4, §5.4, §7.3, §9.3).

#include "generative/core/genre.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <cctype>
#include <cstring>

using namespace deluge::generative;

// clang-format off
describe genre_data("Every built-in genre", $ {
	it("has a stable lowercase name it can be found by", _ {
		expect(allGenres().empty()).to_be_false();
		for (const Genre* genre : allGenres()) {
			expect(std::strlen(genre->name)).to_be_greater_than(size_t{0});
			for (const char* c = genre->name; *c; c++) {
				expect(std::islower(static_cast<unsigned char>(*c)) != 0).to_be_true();
			}
			expect(findGenre(genre->name) == genre).to_be_true();
		}
		expect(findGenre("polka") == nullptr).to_be_true();
	});

	it("has 1–6 parts, lanes in the fixed lane order, and a pool for every patterned part and Drums lane", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->parts.size()).to_be_between(size_t{1}, size_t{kMaxParts});
			int32_t drumsParts = 0;
			for (const PartSpec& part : genre->parts) {
				drumsParts += part.role == Role::Drums ? 1 : 0;
				int32_t previous = -1;
				for (const LaneSpec& lane : part.lanes) {
					expect(part.role == Role::Drums || part.role == Role::FX).to_be_true();
					expect(isFxLane(lane.lane)).to_equal(part.role == Role::FX);
					expect(static_cast<int32_t>(lane.lane)).to_be_greater_than(previous);
					previous = static_cast<int32_t>(lane.lane);
					expect(lane.pool.empty()).to_equal(isFxLane(lane.lane)); // section rules play FX lanes (§5.4)
					expect(lane.band.low).to_be_less_than(lane.band.high + 1);
				}
				if (part.flavour == Flavour::Loop) {
					expect(part.pool.empty()).to_be_false();
					expect(part.loop.degrees.empty()).to_be_false();
				}
			}
			expect(drumsParts).to_be_less_than(2);
		}
	});

	it("gives every pitched part a Register at least 12 semitones wide", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				if (part.lanes.empty()) {
					Register reg = part.registerOrDefault();
					expect(reg.spanHigh - reg.spanLow).to_be_greater_than(11);
				}
			}
		}
	});

	it("only has Build lengths that land the Drop on a phrase line", _ {
		for (const Genre* genre : allGenres()) {
			for (const WeightedBars& build : genre->arc.buildBars) {
				// Shorter Builds take the end of the phrase before; until that lands, whole phrases only.
				expect(build.bars % genre->phraseBars).to_equal(0);
				expect(build.weight).to_be_greater_than(uint8_t{0});
			}
			expect(genre->arc.groovePhrases.low).to_be_greater_than(uint8_t{0});
			expect(genre->arc.breakPhrases.low).to_be_greater_than(uint8_t{0});
		}
	});
});

CPPSPEC_SPEC(genre_data)
