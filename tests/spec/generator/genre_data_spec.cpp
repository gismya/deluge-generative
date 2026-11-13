// Checks every built-in genre's data (spec §4, §5.4, §7.3–§7.5, §9.3).

#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/motion_curve.h"
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

/// Every pool a genre's parts and lanes pick from, and every lane's fill pool (spec §8.7).
std::vector<std::span<const Pattern>> poolsAndFillPools(const Genre& genre) {
	std::vector<std::span<const Pattern>> out = pools(genre);
	for (const PartSpec& part : genre.parts) {
		for (const LaneSpec& lane : part.lanes) {
			out.push_back(lane.fillPool);
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
					// Section rules play FX lanes (§5.4). A Drums lane with no pool plays only the roll, the Phrase accent
					// or a Swap, so a rule or a fill pool must name it.
					bool ruled = lane.lane == genre->arc.roll.lane || lane.lane == genre->arc.accentLane;
					if (isFxLane(lane.lane)) {
						expect(lane.pool.empty() && lane.fillPool.empty()).to_be_true();
					}
					else {
						expect(!lane.pool.empty() || !lane.fillPool.empty() || ruled).to_be_true();
					}
					expect(lane.band.low).to_be_less_than(lane.band.high + 1);
				}
				if (part.flavour == Flavour::Loop) {
					expect(part.pool.empty()).to_be_false();
					expect(part.loop.degrees.empty()).to_be_false();
					// A Loop that answers does so every N bars, N at least 1, with a weight on some transform.
					if (!part.loop.answers.empty()) {
						expect(part.loop.answerBars).to_be_greater_than(uint8_t{0});
						int32_t weights = 0;
						for (const Answer& answer : part.loop.answers) {
							weights += answer.weight;
							expect(answer.kind == AnswerKind::None).to_be_false();
						}
						expect(weights).to_be_greater_than(0);
					}
					expect(part.loop.leap.atZero >= 0 && part.loop.leap.atFull >= 0).to_be_true();
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

	it("fits its roster in the Generator block: its notes, step events and control sets", _ {
		for (const Genre* genre : allGenres()) {
			std::vector<Role> roster;
			for (int32_t p = 0; p < partCount(*genre); p++) {
				roster.push_back(genre->parts[p].role);
			}
			expect(soundingOf(roster)).to_be_less_than(SoundingNotes::kTotal + 1);
			for (bool barLine : {true, false}) {
				expect(stepEventsOf(roster, barLine)).to_be_less_than(stepEventsOf(kLargestRoster, barLine) + 1);
			}
			expect(controlSets(*genre)).to_be_less_than(kMaxControlSets + 1);
			expect(controlEventsOf(*genre)).to_be_less_than(kMaxControlEventsPerStep + 1);
		}
	});

	it("weighs the macro layer within ×0..×2", _ {
		for (const Genre* genre : allGenres()) {
			for (const auto& row : genre->soundWeight) {
				for (int16_t weight : row) {
					expect(weight >= 0 && weight <= 2000).to_be_true();
				}
			}
		}
	});

	it("gives each part gesture depths and a drift weight within ×0..×2, and Throw a hold", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->throwHoldSteps).to_be_greater_than(0);
			for (const PartSpec& part : genre->parts) {
				for (int16_t depth : part.gestures.depth) {
					expect(depth >= 0 && depth <= 2000).to_be_true();
				}
				expect(part.drift.weight >= 0 && part.drift.weight <= 2000).to_be_true();
			}
		}
	});

	it("gives every Sweep a period that divides the phrase or is a whole multiple of it (spec §11.5)", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				const int32_t bars = part.drift.sweepBars;
				expect(bars == 0 || genre->phraseBars % bars == 0 || bars % genre->phraseBars == 0).to_be_true();
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
		// 1, 2, 4, 8, 16 or 32 bars, each dividing the phrase or a whole number of phrases (spec §9.3).
		for (const Genre* genre : allGenres()) {
			for (const WeightedBars& build : genre->arc.buildBars) {
				bool allowed = false;
				for (int32_t bars : {1, 2, 4, 8, 16, 32}) {
					allowed = allowed || build.bars == bars;
				}
				expect(allowed).to_be_true();
				expect(genre->phraseBars % build.bars == 0 || build.bars % genre->phraseBars == 0).to_be_true();
				expect(build.weight).to_be_greater_than(uint8_t{0});
			}
			expect(genre->arc.groovePhrases.low).to_be_greater_than(uint8_t{0});
			expect(genre->arc.breakPhrases.low).to_be_greater_than(uint8_t{0});
		}
	});

	it("names section-rule lanes its roster has, and keeps the roll lane out of the Build cut", _ {
		for (const Genre* genre : allGenres()) {
			auto inRoster = [&](Lane lane) {
				for (const PartSpec& part : genre->parts) {
					for (const LaneSpec& spec : part.lanes) {
						if (spec.lane == lane) {
							return true;
						}
					}
				}
				return false;
			};
			const ArcSpec& arc = genre->arc;
			expect(arc.roll.lane == Lane::None || inRoster(arc.roll.lane)).to_be_true();
			expect(arc.accentLane == Lane::None || inRoster(arc.accentLane)).to_be_true();
			expect(arc.roll.top == RollStage::Sixteenth || arc.roll.top == RollStage::ThirtySecond
			       || arc.roll.top == RollStage::SixtyFourth)
			    .to_be_true();
			if (arc.roll.lane != Lane::None && arc.buildCutBars > 0) {
				for (int32_t p = 0; p < static_cast<int32_t>(genre->parts.size()); p++) {
					for (const LaneSpec& lane : genre->parts[p].lanes) {
						expect(lane.lane == arc.roll.lane && arc.buildCut.has(p, lane.lane)).to_be_false();
					}
				}
			}
		}
	});

	it("only has fill entries a fill can play: lengths, weights, masks and a held-fill entry (spec §8.7)", _ {
		for (const Genre* genre : allGenres()) {
			const auto& fills = genre->fills;
			expect(fills.empty() || genre->heldFill < fills.size()).to_be_true();
			expect(genre->fillChance).to_be_between(int16_t{0}, int16_t{1000});
			expect(genre->sectionEndChance == kSameAsFillChance
			       || (genre->sectionEndChance >= 0 && genre->sectionEndChance <= 1000))
			    .to_be_true();
			for (const FillEntry& fill : fills) {
				expect(fill.beats == 1 || fill.beats == 2 || fill.beats == 4).to_be_true();
				expect(fill.weight).to_be_greater_than(uint8_t{0});
				// Shorter than a phrase, so it never covers the phrase's first step.
				expect(fill.beats * kStepsPerBeat).to_be_less_than(genre->phraseSteps());
				expect(fill.mask.parts != 0 || fill.mask.lanes != 0).to_be_true();
				for (int32_t p = 0; p < kMaxParts; p++) {
					const bool named = (fill.mask.parts & (1U << p)) != 0;
					// Whole parts only in a Rest, and only pitched ones: Bass, Chords, Pad or Lead.
					expect(named && (fill.kind != FillKind::Rest || p >= static_cast<int32_t>(genre->parts.size())
					                 || !genre->parts[p].lanes.empty()))
					    .to_be_false();
				}
				for (int32_t l = 0; l < kNumLanes; l++) {
					const auto lane = static_cast<Lane>(l);
					if ((fill.mask.lanes & laneBit(lane)) == 0) {
						continue;
					}
					expect(isFxLane(lane)).to_be_false(); // section rules own FX lanes
					// A masked lane is in the roster, and a Swap's has fill patterns to play.
					const LaneSpec* found = nullptr;
					for (const PartSpec& part : genre->parts) {
						for (const LaneSpec& spec : part.lanes) {
							found = spec.lane == lane ? &spec : found;
						}
					}
					expect(found != nullptr).to_be_true();
					expect(fill.kind == FillKind::Swap && found != nullptr && found->fillPool.empty()).to_be_false();
				}
			}
		}
	});

	it("fits its Drop decay in its shortest Groove at Motion 50", _ {
		for (const Genre* genre : allGenres()) {
			// The first phrase line where phrases × the Groove multiple at Motion 50 reaches the shortest base, less
			// the bars a Build shorter than a phrase takes from it after Groove → Build (spec §9.3).
			int64_t multiple = motionMultiple(curves::grooveLength, kMacroFull);
			int32_t phrases = 1;
			while (phrases * multiple < int64_t{genre->arc.groovePhrases.low} * kCurveOne) {
				phrases++;
			}
			int32_t shortestBars = phrases * genre->phraseBars;
			if (genre->arc.grooveToBuild > 0) {
				int32_t taken = 0;
				for (const WeightedBars& build : genre->arc.buildBars) {
					taken = build.bars < genre->phraseBars && build.bars > taken ? build.bars : taken;
				}
				shortestBars -= taken;
			}
			expect(genre->arc.dropDecayPhrases * genre->phraseBars).to_be_less_than(shortestBars + 1);
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

	it("plays the off-beat hit that carries the authoring rule above ghost level", _ {
		// Ghosts are low-importance steps (spec §8.1): the hit that keeps a pattern's syncopation isn't one.
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					int32_t offBeat = strongestOffBeat(pattern);
					if (pattern.tier == 0 || offBeat == 0) {
						continue;
					}
					bool audible = false;
					for (int32_t i = 0; i < pattern.length; i++) {
						audible = audible
						          || (i % kStepsPerBeat != 0 && pattern.importance[i] == offBeat
						              && pattern.level[i] != VelocityLevel::Ghost);
					}
					expect(audible).to_be_true();
				}
			}
		}
	});

	it("lists every pool and fill pool by tier, lowest first, with every entry weighted above 0", _ {
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : poolsAndFillPools(*genre)) {
				for (size_t i = 0; i < pool.size(); i++) {
					expect(pool[i].weight).to_be_greater_than(uint8_t{0});
					if (i > 0) {
						expect(pool[i].tier).to_be_greater_than(pool[i - 1].tier - 1);
					}
				}
			}
		}
	});

	it("has a pool and fill pool entry at or below every band's lowest tier", _ {
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
					if (!lane.fillPool.empty()) {
						banded.emplace_back(lane.fillPool, lane.band);
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

	it("keeps Lean within ±12 ticks, flam gaps at 2–6 ticks and Ornament rates within 0–1000, floor to ceiling", _ {
		for (const Genre* genre : allGenres()) {
			std::vector<const Expression*> expressions;
			for (const PartSpec& part : genre->parts) {
				expressions.push_back(&part.expression);
				for (const LaneSpec& lane : part.lanes) {
					expressions.push_back(&lane.expression);
				}
			}
			for (const Expression* expression : expressions) {
				for (int8_t lean : expression->lean) {
					expect(static_cast<int32_t>(lean)).to_be_between(-kMaxLean, kMaxLean);
				}
				const Ornaments& ornaments = expression->ornaments;
				expect(ornaments.flamGap).to_be_between(uint8_t{2}, uint8_t{6});
				expect(ornaments.rateFloor).to_be_between(int16_t{0}, ornaments.rateCeiling);
				expect(ornaments.rateCeiling).to_be_less_than(int16_t{1001});
			}
		}
	});

	it("only has interplay rules that read earlier parts and lanes", _ {
		for (const Genre* genre : allGenres()) {
			expect(interplayRulesValid(*genre)).to_be_true();
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
