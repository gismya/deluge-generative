// Checks every built-in genre's data (spec §4, §5.4, §7.3–§7.5, §9.3).

#include "generative/core/genre.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <cctype>
#include <cstring>
#include <span>
#include <vector>

using namespace deluge::generative;

namespace {

/// Every pool a genre's parts and lanes pick from.
std::vector<std::span<const Pattern>> pools(const Genre& genre) {
	std::vector<std::span<const Pattern>> out;
	for (const PartSpec& part : genre.parts) {
		if (!part.pool.empty()) {
			out.push_back(part.pool);
		}
		for (const LaneSpec& lane : part.lanes) {
			out.push_back(lane.pool);
		}
	}
	return out;
}

/// The importance of the pattern's strongest hit off the beat (an e, & or a), kAnchor for an off-beat anchor, or 0.
int32_t strongestOffBeat(const Pattern& pattern) {
	int32_t strongest = 0;
	for (int32_t i = 0; i < pattern.length; i++) {
		if (i % kStepsPerBeat != 0 && pattern.importance[i] > strongest) {
			strongest = pattern.importance[i];
		}
	}
	return strongest;
}

/// The authoring rule (spec §8.1): an anchor off the beat, or a hit off the beat that ranks in the top half of the
/// pattern's importances, so low Density keeps the pattern's syncopation. Anchors always play, so they are left out of
/// the ranking.
bool keepsSyncopation(const Pattern& pattern) {
	int32_t offBeat = strongestOffBeat(pattern);
	if (offBeat == kAnchor) {
		return true;
	}
	if (offBeat == 0) {
		return false;
	}
	int32_t hits = 0;
	int32_t higher = 0;
	for (int32_t i = 0; i < pattern.length; i++) {
		int32_t importance = pattern.importance[i];
		if (importance > 0 && importance < kAnchor) {
			hits++;
			higher += importance > offBeat ? 1 : 0;
		}
	}
	return 2 * higher < hits;
}

constexpr Pattern kSyncopated[] = {
    {"X...X...X...X.3.", "................", 2}, // its only other hit is off the beat
    {"9.7.5...9.7.5...", "................", 1}, // its off-beat 7s rank below the 9s, in the top half
    {"..X.............", "................", 1}, // an off-beat anchor
};
constexpr Pattern kStraight[] = {
    {"9.3.9.3.9.3.9.3.", "................", 1}, // off-beat hits rank in the bottom half
    {"9.5.9.5.9.5.9.5.", "................", 1}, // and ties don't lift them into the top half
    {"X...X...X...X...", "................", 1}, // nothing off the beat
};

} // namespace

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

	it("gives every pitched role a default Register at least 12 semitones wide", _ {
		for (Role role : {Role::Bass, Role::Chords, Role::Pad, Role::Lead}) {
			Register reg = defaultRegister(role);
			expect(reg.anchor).to_be_greater_than(uint8_t{0});
			expect(reg.spanHigh - reg.spanLow).to_be_greater_than(11);
		}
	});

	it("names a Degree set only on Bass and Lead parts that play Loop, Arp or Motif", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				bool mayNarrow = (part.role == Role::Bass || part.role == Role::Lead)
				                 && (part.flavour == Flavour::Loop || part.flavour == Flavour::Arp
				                     || part.flavour == Flavour::Motif);
				expect(mayNarrow || part.degreeSet == DegreeSet::Full).to_be_true();
			}
		}
	});

	it("keeps its Extension floor at or below its ceiling, and the ceiling at 2 or below", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->harmony.extensionFloor).to_be_less_than(genre->harmony.extensionCeiling + 1);
			expect(genre->harmony.extensionCeiling).to_be_less_than(uint8_t{3});
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

	it("keeps an off-beat hit in the top half of every tier 1+ pattern's importances", _ {
		for (const Pattern& pattern : kSyncopated) {
			expect(keepsSyncopation(pattern)).to_be_true();
		}
		for (const Pattern& pattern : kStraight) {
			expect(keepsSyncopation(pattern)).to_be_false();
		}
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					if (pattern.tier >= 1) {
						expect(keepsSyncopation(pattern)).to_be_true();
					}
				}
			}
		}
	});

	it("plays every tier 1+ pattern's off-beat hit at the genre's default Density, whatever its jitter", _ {
		for (const Genre* genre : allGenres()) {
			int32_t threshold = genre->densityCurve.threshold(genre->defaults[Macro::Density]);
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					int32_t offBeat = strongestOffBeat(pattern);
					if (pattern.tier >= 1 && offBeat != kAnchor) {
						expect(offBeat * 100 - genre->densityJitter).to_be_greater_than(threshold - 1);
					}
				}
			}
		}
	});

	it("lists every pool by tier, lowest first, with every entry weighted above 0", _ {
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (size_t i = 0; i < pool.size(); i++) {
					expect(pool[i].weight).to_be_greater_than(uint8_t{0});
					if (i > 0) {
						expect(pool[i].tier).to_be_greater_than(pool[i - 1].tier - 1);
					}
				}
			}
		}
	});

	it("has a pool entry at or below every band's lowest tier", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				std::vector<std::pair<std::span<const Pattern>, TierBand>> banded;
				if (!part.pool.empty()) {
					banded.emplace_back(part.pool, part.band);
				}
				for (const LaneSpec& lane : part.lanes) {
					if (!lane.pool.empty()) { // FX lanes have no pool: section rules play them (spec §5.4)
						banded.emplace_back(lane.pool, lane.band);
					}
				}
				for (const auto& [pool, band] : banded) {
					expect(band.high).to_be_less_than(kMaxTier + 1);
					bool covered = false;
					for (const Pattern& pattern : pool) {
						covered = covered || pattern.tier <= band.low;
					}
					expect(covered).to_be_true();
				}
			}
		}
	});

	it("thins to the anchors alone at Density 0 and lets every step through at full Density", _ {
		for (const Genre* genre : allGenres()) {
			const DensityCurve& curve = genre->densityCurve;
			expect(genre->densityJitter).to_be_between(int16_t{0}, int16_t{99});
			// The top non-anchor importance, jittered up, stays below the threshold at Density 0.
			expect(curve.threshold(0)).to_be_greater_than(900 + genre->densityJitter);
			// The lowest, jittered down, clears it at full Density.
			expect(curve.threshold(kMacroFull)).to_be_less_than(100 - genre->densityJitter + 1);
			for (int32_t density = 1; density <= kMacroFull; density++) {
				expect(curve.threshold(density)).to_be_less_than(curve.threshold(density - 1) + 1);
			}
		}
	});
});

CPPSPEC_SPEC(genre_data)
