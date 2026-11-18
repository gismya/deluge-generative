// House (spec §4.5, #132): a genre of its own with six parts (Drums, Bass, Stab, Strings, Lead, FX), four on the floor
// with claps on 2 and 4, an offbeat bass, 7th stabs and Strings over a walking Chord graph, swung hats leaning late on
// the e and the a, and a Lead that Responds to the bass in each bar's second half. Run straight through the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/palette.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace harness;

namespace {

const Genre& houseGenre = genres::house;

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kStabPart = 2;
constexpr int32_t kStringsPart = 3;
constexpr int32_t kLeadPart = 4;
constexpr int32_t kFxPart = 5;

/// Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec §9.3).
Recipe movingRecipe(const Genre& genre, uint32_t entropy, int32_t density = 25, int32_t complexity = 25) {
	Recipe recipe = newRecipe(genre, entropy);
	recipe.keyRoot = static_cast<uint8_t>(entropy % 12);
	recipe.macros.set(Macro::Motion, macroPoints(50));
	recipe.macros.set(Macro::Density, macroPoints(density));
	recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	return recipe;
}

const LaneSpec& laneOf(Lane lane) {
	for (const LaneSpec& spec : houseGenre.parts[kDrumsPart].lanes) {
		if (spec.lane == lane) {
			return spec;
		}
	}
	throw "no such lane";
}

/// The step a note-on was decided for: nothing in House moves a pitched note early (no negative Lean), and swing and
/// the hats' Lean move it at most half a step late.
int32_t stepAt(const Event& e) {
	return e.tick / kTicksPerStep;
}

/// Respond as the spec reads it (§5.5), counted at each half-bar's first step from the Bass's planned onsets at that
/// step's Density: by half-bar index, whether it is against (the busier half, the first on a non-zero tie).
struct AgainstHalves {
	std::map<int32_t, bool> against;
	static inline AgainstHalves* current = nullptr;
	AgainstHalves() {
		current = this;
		Generator::testDecided_ = [](const Generator& g, int32_t step, int32_t stepDensity) {
			if (step % kHalfBarSteps != 0) {
				return;
			}
			const int32_t bar = step - step % kStepsPerBar;
			auto count = [&](int32_t from) {
				int32_t n = 0;
				for (int32_t s = from; s < from + kHalfBarSteps; s++) {
					n += g.testTargetPlanned({kBassPart}, s, stepDensity) ? 1 : 0;
				}
				return n;
			};
			const int32_t first = count(bar);
			const int32_t second = count(bar + kHalfBarSteps);
			const bool inFirst = halfOf(step) == 0;
			const int32_t mine = inFirst ? first : second;
			const int32_t other = inFirst ? second : first;
			current->against[step / kHalfBarSteps] = mine > other || (inFirst && mine == other && mine > 0);
		};
	}
	~AgainstHalves() {
		Generator::testDecided_ = nullptr;
		current = nullptr;
	}
	AgainstHalves(const AgainstHalves&) = delete;
	AgainstHalves& operator=(const AgainstHalves&) = delete;
};

/// House with its Lead's Respond rule at `strength` (a test edit of the data).
struct HouseWithRespond {
	InterplayRule rule[1];
	PartSpec parts[6];
	Genre genre;
	explicit HouseWithRespond(int8_t strength) : rule{houseGenre.parts[kLeadPart].rules[0]}, genre(houseGenre) {
		rule[0].strength = strength;
		std::copy(houseGenre.parts.begin(), houseGenre.parts.end(), parts);
		parts[kLeadPart].rules = rule;
		genre.parts = parts;
	}
};

} // namespace

// clang-format off
describe house("House", $ {
	it("is a genre of its own with six parts: Drums, Bass, Stab, Strings, Lead and FX (spec §4.5)", _ {
		expect(std::string(houseGenre.name)).to_equal(std::string("house"));
		expect(std::string(houseGenre.oledName)).to_equal(std::string("HOUSE"));
		expect(houseGenre.parent == nullptr).to_be_true();
		expect(houseGenre.version).to_equal(1);
		expect(findGenre("house") == &houseGenre).to_be_true();
		expect(partCount(houseGenre)).to_equal(6);
		const std::vector<std::tuple<Role, Flavour, std::string>> roster = {
		    {Role::Drums, Flavour::None, "DRUMS"},    {Role::Bass, Flavour::Loop, "BASS"},
		    {Role::Chords, Flavour::Stab, "STAB"},    {Role::Pad, Flavour::Sustain, "STRINGS"},
		    {Role::Lead, Flavour::Motif, "LEAD"},     {Role::FX, Flavour::None, "FX"}};
		for (size_t p = 0; p < roster.size(); p++) {
			const PartSpec& part = houseGenre.parts[p];
			expect(part.role == std::get<0>(roster[p])).to_be_true();
			expect(part.flavour == std::get<1>(roster[p])).to_be_true();
			expect(std::string(part.name)).to_equal(std::get<2>(roster[p]));
		}
		expect(rosterFits(houseGenre)).to_be_true();
		expect(genreValid(houseGenre)).to_be_true();
	});

	it("plays 120–126 BPM in Minor, Dorian or Major, swing +15, in 8-bar phrases, every macro at 25", _ {
		expect(houseGenre.facts.tempoLowBpm).to_equal(120);
		expect(houseGenre.facts.tempoHighBpm).to_equal(126);
		const std::set<Scale> scales(houseGenre.facts.scales.begin(), houseGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Dorian, Scale::Major}).to_be_true();
		expect(houseGenre.facts.swing).to_equal(15);
		expect(houseGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(houseGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("walks a Chord graph from home i: i–iv, i–♭VII, iv–♭VII, i–♭VI and ♭VI–♭VII both ways, 2 bars a chord, repeat 15, cadence ♭VII every 4, Extensions 1–2", _ {
		const ChordGraph* graph = houseGenre.harmony.graph;
		expect(graph != nullptr).to_be_true();
		std::set<std::pair<int32_t, int32_t>> moves;
		for (const ChordMove& move : graph->moves) {
			moves.insert({move.from, move.to});
		}
		// Each brief move both ways: a move back to i is the home pull's.
		const std::pair<int32_t, int32_t> brief[] = {{kI, kIV}, {kI, kVII}, {kIV, kVII}, {kI, kVI}, {kVI, kVII}};
		for (const auto& [from, to] : brief) {
			expect(moves.contains({from, to})).to_be_true();
			expect(moves.contains({to, from}) || from == kI).to_be_true();
		}
		expect(moves.size()).to_equal(size_t{7});
		expect(graph->homePull).to_be_greater_than(uint8_t{0});
		expect(graph->barsPerChord).to_equal(2);
		expect(graph->repeatWeight).to_equal(15);
		expect(graph->cadenceTarget).to_equal(kVII);
		expect(graph->cadencePeriod).to_equal(4);
		expect(houseGenre.harmony.extensionFloor).to_equal(1);
		expect(houseGenre.harmony.extensionCeiling).to_equal(2);
	});

	context("its drums (spec §4.5)", _ {
		it("keeps the kick on the four beats and the claps on 2 and 4, anchored, at every Complexity", _ {
			for (const Pattern& pattern : laneOf(Lane::Kick).pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					expect(pattern.importance[step]).to_equal(step % 4 == 0 ? kAnchor : 0);
				}
			}
			for (const Pattern& pattern : laneOf(Lane::Clap).pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					expect(pattern.importance[step]).to_equal(step % kStepsPerBar == 4 || step % kStepsPerBar == 12 ? kAnchor : 0);
				}
			}
		});

		it("plays the claps on 2 and 4 of every Groove bar a fill doesn't touch", _ {
			int32_t bars = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				Genre plain = houseGenre;
				plain.fills = {};
				Recipe recipe = movingRecipe(plain, seed);
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				std::set<int32_t> claps;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::Clap) {
						claps.insert(stepAt(e));
					}
				}
				for (int32_t bar = 0; (bar + 1) * kStepsPerBar <= static_cast<int32_t>(walked.steps.size()); bar++) {
					if (walked.steps[bar * kStepsPerBar].section != SectionKind::Groove) {
						continue;
					}
					bars++;
					expect(claps.contains(bar * kStepsPerBar + 4)).to_be_true();
					expect(claps.contains(bar * kStepsPerBar + 12)).to_be_true();
				}
			}
			expect(bars).to_be_greater_than(32);
		});

		it("leans the hats late on the e and the a (0, +3, 0, +3)", _ {
			for (Lane lane : {Lane::ClosedHat, Lane::Ride, Lane::Perc1}) {
				const Expression& feel = laneOf(lane).expression;
				expect(feel.lean[0]).to_equal(0);
				expect(feel.lean[1]).to_equal(3);
				expect(feel.lean[2]).to_equal(0);
				expect(feel.lean[3]).to_equal(3);
			}
		});

		it("fills with a Swap on Clap and Snare (the held fill) and a Roll on Snare, at 350 per-mille, the Crash on Drops and at 150", _ {
			const FillEntry& held = houseGenre.fills[houseGenre.heldFill];
			expect(held.kind == FillKind::Swap).to_be_true();
			expect(held.mask == PartsAndLanes{.lanes = laneBit(Lane::Clap) | laneBit(Lane::Snare)}).to_be_true();
			expect(laneOf(Lane::Clap).fillPool.empty()).to_be_false();
			expect(laneOf(Lane::Snare).fillPool.empty()).to_be_false();
			bool roll = false;
			for (const FillEntry& fill : houseGenre.fills) {
				expect(fill.kind == FillKind::Swap || fill.kind == FillKind::Roll).to_be_true();
				if (fill.kind == FillKind::Roll) {
					roll = true;
					expect(fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare)}).to_be_true();
				}
			}
			expect(roll).to_be_true();
			expect(houseGenre.fillChance).to_equal(int16_t{350});
			expect(houseGenre.arc.accentLane == Lane::Crash).to_be_true();
			expect(houseGenre.arc.accentChance).to_equal(int16_t{150});
		});
	});

	context("its pitched parts (spec §4.5)", _ {
		it("anchors the Bass on the & of every beat, never on a beat, its pickups only in a bar's first half", _ {
			const PartSpec& bass = houseGenre.parts[kBassPart];
			for (const Pattern& pattern : bass.pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					const int32_t at = step % kStepsPerBar;
					if (at % 4 == 0) {
						expect(pattern.importance[step]).to_equal(0); // the kick's
					}
					if (at % 4 == 2) {
						expect(pattern.importance[step]).to_equal(kAnchor);
					}
					else if (halfOf(at) == 1) {
						expect(pattern.importance[step]).to_equal(0); // the second half is never the busier
					}
				}
			}
		});

		it("plays no Bass note on a kick", _ {
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Walked walked = walk(movingRecipe(houseGenre, seed, 40, 50), seed, 64 * kStepsPerBar);
				std::set<int32_t> kicks;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::Kick) {
						kicks.insert(stepAt(e));
					}
				}
				for (const Event& e : walked.noteOns) {
					if (e.part == kBassPart) {
						expect(kicks.contains(stepAt(e))).to_be_false();
					}
				}
				expect(kicks.empty()).to_be_false();
			}
		});

		it("stabs every chord with an Extension, over Strings that anchor theirs, and gives the Lead a Motif that Responds to the Bass", _ {
			expect(houseGenre.parts[kStabPart].stab.gateTicks).to_be_greater_than(uint8_t{0});
			expect(houseGenre.parts[kStringsPart].sustain.anchorExtension).to_be_true();
			const PartSpec& lead = houseGenre.parts[kLeadPart];
			expect(lead.rules.size()).to_equal(size_t{1});
			expect(lead.rules[0].kind == RuleKind::Respond).to_be_true();
			expect(lead.rules[0].target.part).to_equal(uint8_t{kBassPart});
			expect(lead.rules[0].strength).to_equal(int8_t{60});
			expect(lead.rules[0].sensitivity).to_be_greater_than(int8_t{0});
			expect(lead.motif.positions.empty()).to_be_false();
		});

		it("anchors the Lead only in a bar's second half", _ {
			for (const Pattern& pattern : houseGenre.parts[kLeadPart].pool) {
				bool anchored = false;
				for (int32_t step = 0; step < pattern.length; step++) {
					if (pattern.importance[step] == kAnchor) {
						expect(halfOf(step % kStepsPerBar)).to_equal(1);
						anchored = true;
					}
				}
				expect(anchored).to_be_true();
			}
		});

		it("at full Respond strength plays no Lead note in a half-bar the Bass makes against, over many pieces", _ {
			const HouseWithRespond full(100);
			int32_t leads = 0;
			int32_t firstHalves = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				AgainstHalves halves;
				const Walked walked = walk(movingRecipe(full.genre, seed), seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part != kLeadPart) {
						continue;
					}
					leads++;
					const int32_t half = stepAt(e) / kHalfBarSteps;
					expect(halves.against.contains(half)).to_be_true();
					expect(halves.against[half]).to_be_false();
				}
				for (const auto& [half, against] : halves.against) {
					firstHalves += half % 2 == 0 && against ? 1 : 0;
				}
			}
			expect(leads).to_be_greater_than(200);
			expect(firstHalves).to_be_greater_than(200); // the offbeat bass makes the first half against
		});

		it("at its own strength (60) plays most Lead notes in a bar's second half, thinning the first half's lead-ins", _ {
			int32_t first = 0;
			int32_t second = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(movingRecipe(houseGenre, seed), seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part == kLeadPart) {
						(halfOf(stepAt(e) % kStepsPerBar) == 0 ? first : second)++;
					}
				}
			}
			expect(second).to_be_greater_than(100);
			expect(first).to_be_greater_than(0); // 60 % thins the lead-ins, it doesn't drop them all
			expect(first * 4).to_be_less_than(second);
		});

		it("keeps every pitched part's range clear of the others in every key and scale", _ {
			struct Range {
				int32_t low = 128;
				int32_t high = -1;
			};
			for (Scale scale : houseGenre.facts.scales) {
				for (uint8_t root = 0; root < 12; root++) {
					Recipe recipe = movingRecipe(houseGenre, root + 1u, 40, 50);
					recipe.keyRoot = root;
					recipe.scale = scale;
					const Walked walked = walk(recipe, root + 1u, 48 * kStepsPerBar);
					std::map<int32_t, Range> ranges;
					for (const Event& e : walked.noteOns) {
						if (e.part == kBassPart || e.part == kStabPart || e.part == kStringsPart || e.part == kLeadPart) {
							Range& range = ranges[e.part];
							range.low = std::min<int32_t>(range.low, e.pitch);
							range.high = std::max<int32_t>(range.high, e.pitch);
						}
					}
					// Low to high: Bass, Strings, Stab, Lead.
					expect(ranges[kBassPart].high).to_be_less_than(ranges[kStringsPart].low);
					expect(ranges[kStringsPart].high).to_be_less_than(ranges[kStabPart].low);
					expect(ranges[kStabPart].high).to_be_less_than(ranges[kLeadPart].low);
				}
			}
		});
	});

	context("its Arc (spec §4.5, §9)", _ {
		it("takes its Arc from the brief: Groove 4–8, Break 1 keeping Stab, Strings and Closed hat, Builds 4×1 and 8×2 with no cut and a 1-beat Gap", _ {
			const ArcSpec& arc = houseGenre.arc;
			expect(arc.introPhrases).to_equal(1);
			expect(arc.outroPhrases).to_equal(1);
			expect(arc.groovePhrases.low).to_equal(4);
			expect(arc.groovePhrases.high).to_equal(8);
			expect(arc.breakPhrases.low).to_equal(1);
			expect(arc.breakPhrases.high).to_equal(1);
			expect(arc.breakKeeps == PartsAndLanes{.parts = partBit(kStabPart) | partBit(kStringsPart),
			                                       .lanes = laneBit(Lane::ClosedHat)})
			    .to_be_true();
			std::map<int32_t, int32_t> builds;
			for (const WeightedBars& bars : arc.buildBars) {
				builds[bars.bars] = bars.weight;
			}
			expect(builds == std::map<int32_t, int32_t>{{4, 1}, {8, 2}}).to_be_true();
			expect(arc.buildCutBars).to_equal(0);
			expect(arc.gap == GapLength::Beat).to_be_true();
			expect(arc.roll.lane == Lane::Snare).to_be_true();
			expect(arc.roll.top == RollStage::ThirtySecond).to_be_true();
			expect(arc.dropDecayPhrases).to_equal(1);
			expect(cutAndGapUnderBuild(houseGenre)).to_be_true();
		});

		it("recedes to the Stab, the Strings and the closed hat in a Break, and Builds with everything", _ {
			int32_t buildSteps = 0;
			int32_t breakNotes = 0;
			std::set<int32_t> buildParts;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(movingRecipe(houseGenre, seed), seed, 96 * kStepsPerBar);
				for (const Walked::Step& step : walked.steps) {
					buildSteps += step.section == SectionKind::Build ? 1 : 0;
				}
				for (const Event& e : walked.noteOns) {
					const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
					if (walked.steps[step].section == SectionKind::Build) {
						buildParts.insert(e.part);
					}
					if (walked.steps[step].section != SectionKind::Break || e.part == kFxPart) {
						continue;
					}
					breakNotes++;
					const bool kept = e.part == kStabPart || e.part == kStringsPart
					                  || (e.part == kDrumsPart && e.lane == Lane::ClosedHat);
					expect(kept).to_be_true();
				}
			}
			expect(buildSteps).to_be_greater_than(4 * kStepsPerBar);
			expect(breakNotes).to_be_greater_than(0);
			for (int32_t p : {kDrumsPart, kBassPart, kStabPart, kLeadPart}) {
				expect(buildParts.contains(p)).to_be_true();
			}
		});
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		for (int32_t complexity : {0, 25, 50}) {
			for (int32_t density : {0, 25, 50}) {
				const uint32_t entropy = static_cast<uint32_t>(complexity + density + 1);
				const Walked walked = walk(movingRecipe(houseGenre, entropy, density, complexity), 2, 128 * kStepsPerBar);
				expect(walked.dropped).to_equal(0U);
				expect(walked.evicted).to_equal(0U);
			}
		}
	});

	context("its palette (spec §10.1, §10.5, §18.2, §18.3)", _ {
		it("gives every synth part at least 4 presets on the card or in the pack, GEN House Stab the Stab's main pick", _ {
			FakeCard card = harness::factoryCardWithPack();
			for (int32_t p = kBassPart; p <= kLeadPart; p++) {
				auto entries = houseGenre.palette->partEntries(houseGenre.parts[p]);
				expect(entries.size()).to_be_greater_than(size_t{3});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
				}
			}
			const auto stab = houseGenre.palette->partEntries(houseGenre.parts[kStabPart]);
			expect(std::string(stab[0].path)).to_equal(std::string("SYNTHS/GENERATIVE/GEN House Stab.XML"));
			expect(stab[0].weight).to_equal(3);
		});

		it("rolls the pack's claps and rides, only Risers of 6 s or less, and 3 files or more on every ungated Drums lane", _ {
			FakeCard card = harness::factoryCardWithPack();
			std::map<Lane, std::set<std::string>> picks;
			for (uint32_t entropy = 0; entropy < 400; entropy++) {
				SoundPicks sounds;
				rollSounds(houseGenre, *houseGenre.palette, sounds, entropy, card);
				for (int32_t l = 0; l < kNumLanes; l++) {
					if (!sounds.lane[l].empty()) {
						picks[static_cast<Lane>(l)].insert(sounds.lane[l].c_str());
					}
				}
			}
			expect(picks[Lane::Riser] == std::set<std::string>{"SAMPLES/FX/Riser/GEN Riser 2.wav", "SAMPLES/FX/Riser/GEN Riser 3.wav"})
			    .to_be_true();
			const auto picked = [&](Lane lane, const std::string& tag) {
				return std::ranges::any_of(picks[lane], [&](const std::string& path) { return path.find(tag) != std::string::npos; });
			};
			expect(picked(Lane::Clap, "GEN Clap")).to_be_true();
			expect(picked(Lane::Ride, "GEN Ride")).to_be_true();
			for (const LaneSpec& lane : houseGenre.parts[kDrumsPart].lanes) {
				if (lane.densityGate == 0) {
					expect(picks[lane.lane].size()).to_be_greater_than(size_t{2});
				}
				expect(picks[lane.lane].empty()).to_be_false();
			}
		});
	});
});

CPPSPEC_SPEC(house)
