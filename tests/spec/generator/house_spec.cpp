// House (spec §4.5, #132): a genre of its own with six parts (Drums, Bass, Stab, Strings, Lead, FX), four on the floor
// with claps on 2 and 4, an offbeat bass, 7th stabs and Strings over a walking Chord graph, swung hats leaning late on
// the e and the a, and a Lead that Responds to the bass in each bar's second half. Run straight through the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/palette.h"
#include "harness.h"
#include "palette_helpers.h"
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

const LaneSpec& laneOf(Lane lane) {
	return harness::laneOf(houseGenre, kDrumsPart, lane);
}

/// The step a note-on was decided for: nothing in House moves a pitched note early (no negative Lean), and swing and
/// the hats' Lean move it at most half a step late.
int32_t stepAt(const Event& e) {
	return e.tick / kTicksPerStep;
}

/// Respond as the spec reads it (§5.5), counted at each half-bar's first step from the Bass's planned onsets at that
/// step's Density: by half-bar index, whether it is against (the busier half, the first on a non-zero tie).
struct AgainstHalves : harness::DecidedHook<AgainstHalves> {
	std::map<int32_t, bool> against;
	void decided(const Generator& g, int32_t step, int32_t density) {
		if (step % kHalfBarSteps == 0) {
			against[step / kHalfBarSteps] = respondHalves(g, {kBassPart}, step, density).against();
		}
	}
};

/// House with its Lead's Respond rule at `strength` (a test edit of the data).
struct HouseWithRespond {
	InterplayRule rule[1];
	PartSpec parts[6];
	Genre genre;
	explicit HouseWithRespond(int8_t strength, int8_t sensitivity = 40)
	    : rule{houseGenre.parts[kLeadPart].rules[0]}, genre(houseGenre) {
		rule[0].strength = strength;
		rule[0].sensitivity = sensitivity;
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
		expect(houseGenre.version).to_equal(2);
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

	it("walks a Chord graph from home i: i–iv, i–♭VII, iv–♭VII, i–♭VI and ♭VI–♭VII both ways, 2 bars a chord, repeat 8, cadence ♭VII every 4, Extensions 1–2", _ {
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
		expect(graph->repeatWeight).to_equal(8);
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

		it("plays the Rim only on the a's, off the open hat's & of 4", _ {
			for (const Pattern& pattern : laneOf(Lane::Rim).pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					if (pattern.importance[step] != 0) {
						expect(step % kStepsPerBeat).to_equal(3);
					}
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

		it("plays the closed hat and Perc 1 3 ticks late on the e and the a, on time on the beat and the &, as the Ride would", _ {
			// House with no Lean on its drums (a test edit of the data): the same hits, each where the swing alone puts it.
			LaneSpec lanes[kNumLanes];
			std::ranges::copy(houseGenre.parts[kDrumsPart].lanes, lanes);
			for (LaneSpec& lane : lanes) {
				lane.expression.lean[1] = lane.expression.lean[3] = 0;
			}
			PartSpec parts[6];
			std::ranges::copy(houseGenre.parts, parts);
			parts[kDrumsPart].lanes = std::span(lanes, houseGenre.parts[kDrumsPart].lanes.size());
			Genre unleaned = houseGenre;
			unleaned.parts = parts;
			std::map<Lane, int32_t> late;
			for (uint32_t seed = 1; seed <= 2; seed++) {
				// Density 40 opens the Ride's and Perc 1's gate.
				const Walked leaned = walk(movingRecipe(houseGenre, seed, 40), seed, 32 * kStepsPerBar);
				const Walked plain = walk(movingRecipe(unleaned, seed, 40), seed, 32 * kStepsPerBar);
				for (Lane lane : {Lane::ClosedHat, Lane::Ride, Lane::Perc1}) {
					// Each step's first hit: Lean moves the step's onset, and a ratchet's later hits may sit elsewhere.
					auto ticks = [&](const Walked& walked) {
						std::map<int32_t, int32_t> out; // by step
						for (const Event& e : walked.noteOns) {
							if (e.part == kDrumsPart && e.lane == lane) {
								const auto [at, fresh] = out.try_emplace(stepAt(e), e.tick);
								at->second = std::min(at->second, e.tick);
							}
						}
						return out;
					};
					const std::map<int32_t, int32_t> mine = ticks(leaned);
					const std::map<int32_t, int32_t> unmoved = ticks(plain);
					expect(mine.size()).to_equal(unmoved.size());
					for (const auto& [step, tick] : unmoved) {
						const bool eOrA = step % 2 == 1;
						expect(mine.contains(step)).to_be_true();
						expect(mine.contains(step) ? mine.at(step) - tick : -1).to_equal(eOrA ? 3 : 0);
						late[lane] += eOrA ? 1 : 0;
					}
				}
			}
			// The Ride's patterns play no e or a: its Lean shows nowhere yet.
			for (Lane lane : {Lane::ClosedHat, Lane::Perc1}) {
				expect(late[lane]).to_be_greater_than(0);
			}
		});

		it("fills with a Swap on Clap and Snare (the held fill) and a Roll on Snare, at 350 per-mille, the Crash on Drops, after a phrase-ending fill and at 150 on other phrase starts", _ {
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

		it("anchors the Stab on the a of 1, then plays only the & of 2 or 3 and the a's: never a beat or an e", _ {
			for (const Pattern& pattern : houseGenre.parts[kStabPart].pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					const int32_t at = step % kStepsPerBar;
					if (at == 3) {
						expect(pattern.importance[step]).to_equal(kAnchor);
					}
					else if (pattern.importance[step] != 0) {
						expect(at % kStepsPerBeat == 3 || at == 6 || at == 10).to_be_true();
					}
				}
			}
		});

		it("plays no Bass note on a kick", _ {
			for (const auto& [complexity, seed] : {std::pair{0, 1u}, std::pair{25, 2u}, std::pair{50, 3u}, std::pair{50, 4u}}) {
				const Walked walked = walk(movingRecipe(houseGenre, seed, 40, complexity), seed, 64 * kStepsPerBar);
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

		it("stabs every chord with an Extension: 4 pitch classes or more in each stab", _ {
			int32_t stabs = 0;
			for (const auto& [complexity, seed] : {std::pair{0, 1u}, std::pair{25, 2u}, std::pair{50, 3u}}) {
				const Walked walked = walk(movingRecipe(houseGenre, seed, 25, complexity), seed, 64 * kStepsPerBar);
				std::map<int32_t, std::set<int32_t>> chords; // tick -> pitch classes
				for (const Event& e : walked.noteOns) {
					if (e.part == kStabPart) {
						chords[e.tick].insert(e.pitch % 12);
					}
				}
				for (const auto& [tick, classes] : chords) {
					expect(classes.size()).to_be_greater_than(size_t{3});
				}
				stabs += static_cast<int32_t>(chords.size());
			}
			expect(stabs).to_be_greater_than(300);
		});

		it("sets the Stab at 80 % of its preset's volume and the Strings, which anchor their Extension, at 40 %, and gives the Lead a Motif that Responds to the Bass", _ {
			expect(houseGenre.parts[kStabPart].stab.gateTicks).to_be_greater_than(uint8_t{0});
			expect(houseGenre.parts[kStringsPart].sustain.anchorExtension).to_be_true();
			// The knobs only: the levels they give come from delugemu captures of each part alone (§4.5), which no spec
			// runs.
			expect(houseGenre.parts[kStringsPart].volume).to_equal(uint8_t{40});
			expect(houseGenre.parts[kStabPart].volume).to_equal(uint8_t{80});
			const PartSpec& lead = houseGenre.parts[kLeadPart];
			expect(lead.rules.size()).to_equal(size_t{1});
			expect(lead.rules[0].kind == RuleKind::Respond).to_be_true();
			expect(lead.rules[0].target.part).to_equal(uint8_t{kBassPart});
			expect(lead.rules[0].strength).to_equal(int8_t{60});
			expect(lead.rules[0].sensitivity).to_be_greater_than(int8_t{0});
			expect(lead.motif.positions.empty()).to_be_false();
		});

		it("holds a chord 2 or 4 bars, now and then longer: under 1 bar in 10 in a hold over 4 bars (spec §4.5)", _ {
			// A repeat weight with no cap weighs most where the guard thins the other ways on: at 15, 15 % of bars.
			std::map<int32_t, int32_t> holds; // bars held -> bars in such holds
			for (uint32_t seed = 1; seed <= 12; seed++) {
				const Walked walked = walk(movingRecipe(houseGenre, seed), seed, 128 * kStepsPerBar);
				const std::vector<int32_t> changes = walked.changes();
				for (size_t n = 1; n < changes.size(); n++) {
					const int32_t bars = (changes[n] - changes[n - 1]) / kStepsPerBar;
					holds[bars] += bars;
				}
			}
			int32_t all = 0;
			int32_t longer = 0;
			for (const auto& [bars, count] : holds) {
				all += count;
				longer += bars > 4 ? count : 0;
			}
			expect(all).to_be_greater_than(1000);
			expect(longer * 10).to_be_less_than(all);
		});

		it("anchors the Lead only in a bar's second half", _ {
			for (const Pattern& pattern : houseGenre.parts[kLeadPart].pool) {
				bool anchored = false;
				for (int32_t step = 0; step < pattern.length; step++) {
					if (pattern.importance[step] == kAnchor) {
						expect(halfOf(step)).to_equal(1);
						anchored = true;
					}
				}
				expect(anchored).to_be_true();
			}
		});

		it("at full Respond strength plays no Lead note in a half-bar the Bass makes against, over many pieces at every Complexity", _ {
			const HouseWithRespond full(100, 0); // no sensitivity: full at every Complexity
			int32_t leads = 0;
			int32_t firstHalves = 0;
			for (uint32_t seed = 1; seed <= 9; seed++) {
				AgainstHalves halves;
				const int32_t complexity = static_cast<int32_t>(seed % 3) * 25; // 0, 25 and 50
				const Walked walked = walk(movingRecipe(full.genre, seed, 25, complexity), seed, 96 * kStepsPerBar);
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
						(halfOf(stepAt(e)) == 0 ? first : second)++;
					}
				}
			}
			expect(second).to_be_greater_than(100);
			expect(first).to_be_greater_than(0); // 60 % thins the lead-ins, it doesn't drop them all
			expect(first * 3).to_be_less_than(second); // three in four or more
		});

		it("keeps about its strength's share of the first half's lead-ins in every piece, not all or none (spec §5.5)", _ {
			// Each lead-in's draw holds while the Lead's pick does, and a pick can hold for a whole Groove, so a line with
			// one lead-in a bar kept all of them or none in a piece. Two or three, each on its own step, average out.
			// The strength is 60, plus 40 per 50 points of Complexity over the default: 40, 60 and 80 % thinned.
			const HouseWithRespond none(0, 0); // no thinning at any Complexity
			for (const auto& [complexity, keptShare] : {std::pair{0, 60}, std::pair{25, 40}, std::pair{50, 20}}) {
				int32_t kept = 0;
				int32_t all = 0;
				for (uint32_t seed = 1; seed <= 16; seed++) {
					// Lead notes in the half-bars the Bass makes against: the first halves, but where the Bass rests.
					auto firstHalf = [&](const Genre& genre) {
						AgainstHalves halves;
						int32_t n = 0;
						const Walked walked = walk(movingRecipe(genre, seed, 25, complexity), seed, 96 * kStepsPerBar);
						for (const Event& e : walked.noteOns) {
							n += e.part == kLeadPart && halves.against[stepAt(e) / kHalfBarSteps] ? 1 : 0;
						}
						return n;
					};
					const int32_t pieceKept = firstHalf(houseGenre);
					const int32_t pieceAll = firstHalf(none.genre);
					expect(pieceKept).to_be_less_than(pieceAll);
					if (complexity < 50) { // at 80 % a pick's two or three lead-ins all go now and then
						expect(pieceKept).to_be_greater_than(0);
					}
					kept += pieceKept;
					all += pieceAll;
				}
				expect(kept * 100).to_be_greater_than(all * (keptShare - 15));
				expect(kept * 100).to_be_less_than(all * (keptShare + 15));
			}
		});

		it("plays every pitched part, its range clear of the others, in every key and scale", _ {
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
					for (int32_t p : {kBassPart, kStabPart, kStringsPart, kLeadPart}) {
						expect(ranges[p].high).to_be_greater_than(-1); // it played
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
		const harness::Lost lost = harness::lostAtEveryCorner(houseGenre);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its palette (spec §10.1, §10.5, §18.2, §18.3)", _ {
		it("gives every synth part at least 4 presets on the card or in the pack, GEN House Stab the Stab's main pick", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(harness::fewestPartPresets(houseGenre)).to_be_greater_than(size_t{3});
			expect(harness::partPresetsMissing(houseGenre, card).empty()).to_be_true();
			expect(harness::mainPick(houseGenre, kStabPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN House Stab.XML x3"));
		});

		it("rolls the pack's claps and rides, only Risers of 6 s or less, the 909 crash or the descending kick as Impacts, the plain kicks, and 3 files or more on every ungated Drums lane", _ {
			FakeCard card = harness::factoryCardWithPack();
			// The lanes' own lists, the kit aside (spec §10.2): with it, the pack's claps never play, as all three
			// machines have a clap.
			auto picks = harness::lanePicks(houseGenre, card, 400, {}, false);
			expect(picks[Lane::Riser] == std::set<std::string>{"SAMPLES/FX/Riser/GEN Riser 2.wav", "SAMPLES/FX/Riser/GEN Riser 3.wav"})
			    .to_be_true();
			const auto picked = [&](Lane lane, const std::string& tag) {
				return std::ranges::any_of(picks[lane], [&](const std::string& path) { return path.find(tag) != std::string::npos; });
			};
			// A tag matches any part of a name: "909" alone would roll the 909 kicks as Impacts, "909 Kick" its
			// distorted and dry ones as kicks.
			expect(picks[Lane::Impact] == std::set<std::string>{"SAMPLES/DRUMS/Crash/909 Crash.wav",
			                                                    "SAMPLES/DRUMS/Kick/Deep Sky Kick descending.wav"})
			    .to_be_true();
			expect(picks[Lane::Kick] == std::set<std::string>{"SAMPLES/DRUMS/Kick/909 Kick.wav", "SAMPLES/DRUMS/Kick/808 Kick.wav",
			                                                  "SAMPLES/DRUMS/Kick/Linndrum Kick.wav"})
			    .to_be_true();
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
