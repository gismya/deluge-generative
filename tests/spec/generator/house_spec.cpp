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
		expect(houseGenre.version).to_equal(5);
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

	it("plays 120–126 BPM in Minor, Dorian or Mixolydian, swing +10 to +20, in 8-bar phrases, every macro at 25", _ {
		expect(houseGenre.facts.tempoLowBpm).to_equal(120);
		expect(houseGenre.facts.tempoHighBpm).to_equal(126);
		const std::set<Scale> scales(houseGenre.facts.scales.begin(), houseGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Dorian, Scale::Mixolydian}).to_be_true();
		expect(houseGenre.facts.swingLow).to_equal(int8_t{10});
		expect(houseGenre.facts.swingHigh).to_equal(int8_t{20});
		expect(houseGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(houseGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("loops a Chord graph from home i: i–iv, i–♭VII, iv–♭VII, i–♭VI and ♭VI–♭VII both ways, 2 bars a chord, repeat 8, cadence ♭VII every 4, Extensions 1–2, no guarded chord, a new loop on 1 Drop in 3", _ {
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
		// The loop (rules review S1, round 2's s1-loop): a new one on 1 Drop in 3.
		expect(graph->loopChance).to_equal(int16_t{333});
		expect(graph->halfLoopChance).to_equal(int16_t{333});
		// ♭VII weakened from i and ♭VI (#371), and no vii° or vi° (#367): the guard at 0.
		for (const ChordMove& move : graph->moves) {
			if (move.to == kVII && (move.from == kI || move.from == kVI)) {
				expect(move.weight).to_be_less_than(uint8_t{20});
			}
		}
		expect(houseGenre.harmony.guard.factor).to_equal(0);
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

		it("plays the closed hat's 16ths at the default: a hit on the a after each & in most Groove bars (#373)", _ {
			int32_t grooveBars = 0;
			int32_t as = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				const Walked walked = walk(newRecipe(houseGenre, seed), seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					const int32_t step = stepAt(e);
					if (e.part == kDrumsPart && e.lane == Lane::ClosedHat && step % kStepsPerBeat == 3
					    && step < static_cast<int32_t>(walked.steps.size())
					    && walked.steps[step].section == SectionKind::Groove) {
						as++;
					}
				}
				for (size_t n = 0; n < walked.steps.size(); n += kStepsPerBar) {
					grooveBars += walked.steps[n].section == SectionKind::Groove ? 1 : 0;
				}
			}
			expect(grooveBars).to_be_greater_than(200);
			expect(as).to_be_greater_than(2 * grooveBars); // today's tier-1 patterns: about one in two bars
		});

		it("fills with a Swap on Clap and Snare (the held fill), a Roll on Snare and a Rest on the kick (and the Bass), at 350 per-mille and before every Section, the Crash on Drops, after a phrase-ending fill and at 150 on other phrase starts", _ {
			const FillEntry& held = houseGenre.fills[houseGenre.heldFill];
			expect(held.kind == FillKind::Swap).to_be_true();
			expect(held.mask == PartsAndLanes{.lanes = laneBit(Lane::Clap) | laneBit(Lane::Snare)}).to_be_true();
			expect(laneOf(Lane::Clap).fillPool.empty()).to_be_false();
			expect(laneOf(Lane::Snare).fillPool.empty()).to_be_false();
			bool roll = false;
			bool rest = false;
			for (const FillEntry& fill : houseGenre.fills) {
				if (fill.kind == FillKind::Roll) {
					roll = true;
					expect(fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare)}).to_be_true();
				}
				else if (fill.kind == FillKind::Rest) {
					// The kick drops for 2 to 4 beats (rules review S8), the Bass with it or not.
					rest = true;
					expect(fill.mask.lanes).to_equal(laneBit(Lane::Kick));
					expect((fill.mask.parts & ~partBit(kBassPart)) == 0).to_be_true();
				}
				else {
					expect(fill.kind == FillKind::Swap).to_be_true();
				}
			}
			expect(roll).to_be_true();
			expect(rest).to_be_true();
			expect(houseGenre.fillChance).to_equal(int16_t{350});
			expect(houseGenre.sectionEndChance).to_equal(int16_t{1000});
			expect(houseGenre.arc.accentLane == Lane::Crash).to_be_true();
			expect(houseGenre.arc.accentChance).to_equal(int16_t{150});
		});
	});

	context("its pitched parts (spec §4.5)", _ {
		it("anchors the Bass on the & of every beat, never on a beat, each line's pickups in one half of the bar, a tier-1 line's in the second (#368)", _ {
			const PartSpec& bass = houseGenre.parts[kBassPart];
			int32_t secondHalfLines = 0;
			for (const Pattern& pattern : bass.pool) {
				bool halves[2] = {};
				for (int32_t step = 0; step < pattern.length; step++) {
					const int32_t at = step % kStepsPerBar;
					if (at % 4 == 0) {
						expect(pattern.importance[step]).to_equal(0); // the kick's
					}
					if (at % 4 == 2) {
						expect(pattern.importance[step]).to_equal(kAnchor);
					}
					else if (pattern.importance[step] != 0) {
						halves[halfOf(at)] = true;
					}
				}
				expect(halves[0] && halves[1]).to_be_false(); // one half is the busier, so the Lead answers in the other
				if (halves[1]) {
					secondHalfLines++;
					expect(pattern.tier).to_equal(1);
				}
			}
			expect(secondHalfLines).to_equal(1);
		});

		it("anchors the Stab on the a of 1, then plays only &s and a's: never a beat or an e", _ {
			for (const Pattern& pattern : houseGenre.parts[kStabPart].pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					const int32_t at = step % kStepsPerBar;
					if (at == 3) {
						expect(pattern.importance[step]).to_equal(kAnchor);
					}
					else if (pattern.importance[step] != 0) {
						expect(at % kStepsPerBeat >= 2).to_be_true();
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

		it("gates its parts low so a low Density keeps most of them, the Lead only from Density 30, so it plays into and out of Drops (round 2: S2)", _ {
			expect(houseGenre.parts[kStringsPart].densityGate).to_equal(macroPoints(6));
			expect(houseGenre.parts[kBassPart].densityGate).to_equal(macroPoints(9));
			expect(houseGenre.parts[kStabPart].densityGate).to_equal(macroPoints(12));
			expect(houseGenre.parts[kLeadPart].densityGate).to_equal(macroPoints(30));
			int32_t grooveBars = 0;
			int32_t leadBars = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				Recipe recipe = newRecipe(houseGenre, seed);
				const Walked walked = walk(recipe, seed, 160 * kStepsPerBar);
				std::set<int32_t> withLead;
				for (const Event& e : walked.noteOns) {
					if (e.part == kLeadPart) {
						withLead.insert(stepAt(e) / kStepsPerBar);
					}
				}
				for (size_t n = 0; n < walked.steps.size(); n += kStepsPerBar) {
					if (walked.steps[n].section == SectionKind::Groove) {
						grooveBars++;
						leadBars += withLead.contains(static_cast<int32_t>(n) / kStepsPerBar) ? 1 : 0;
					}
				}
			}
			expect(leadBars).to_be_greater_than(0);
			expect(leadBars * 5).to_be_less_than(grooveBars); // under a fifth of the Groove: the Drops' first bars
		});

		it("re-rolls the Lead's notes at 20, so a riff holds 8–16 bars (#372), and sweeps the Stab's filter over 32 bars", _ {
			expect(houseGenre.parts[kLeadPart].motif.reroll).to_equal(int16_t{20});
			expect(houseGenre.parts[kStabPart].drift.sweepBars).to_equal(uint8_t{32});
		});

		it("lets the Bass leap to the 5th from Complexity 25 (#369)", _ {
			const LoopSpec& loop = houseGenre.parts[kBassPart].loop;
			expect(loop.draws.leap.atZero).to_equal(int16_t{3});
			expect(loop.draws.leap.atFull).to_equal(int16_t{6});
			int32_t notes = 0;
			int32_t offRoot = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				Recipe recipe = movingRecipe(houseGenre, Scale::Minor, seed);
				recipe.keyRoot = 0;
				for (const Event& e : walk(recipe, seed, 64 * kStepsPerBar).noteOns) {
					if (e.part == kBassPart) {
						notes++;
						offRoot += e.pitch % 12 == 0 || e.pitch % 12 == 5 || e.pitch % 12 == 8 || e.pitch % 12 == 10 ? 0 : 1;
					}
				}
			}
			expect(notes).to_be_greater_than(500);
			expect(offRoot).to_be_greater_than(0); // a 5th over some chord root
		});

		it("puts every key's tonic between A1 and G♯2, so F♯, G and G♯ keys sit an octave over the rest's floor no longer (#370)", _ {
			const Register bass = houseGenre.parts[kBassPart].reg;
			for (uint8_t root = 0; root < 12; root++) {
				const int32_t tonic = tonicNote(bass, root);
				expect(tonic).to_be_between(33, 44);
			}
			expect(tonicNote(bass, 6)).to_equal(42);  // F♯2
			expect(tonicNote(bass, 8)).to_equal(44);  // G♯2
			expect(tonicNote(bass, 0)).to_equal(36);  // C2, as before
			expect(tonicNote(bass, 9)).to_equal(33);  // A1, as before
			// The Registers stay stacked an octave apart.
			expect(houseGenre.parts[kStringsPart].reg.anchor - bass.anchor).to_equal(12);
			expect(houseGenre.parts[kStabPart].reg.anchor - bass.anchor).to_equal(24);
			expect(houseGenre.parts[kLeadPart].reg.anchor - bass.anchor).to_equal(36);
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

		it("at full Respond strength plays no Lead note but an anchor (beat 3 or its &) in a half-bar the Bass makes against, over many pieces at every Complexity", _ {
			const HouseWithRespond full(100, 0); // no sensitivity: full at every Complexity
			int32_t leads = 0;
			int32_t firstHalves = 0;
			for (uint32_t seed = 1; seed <= 9; seed++) {
				AgainstHalves halves;
				const int32_t complexity = static_cast<int32_t>(seed % 3) * 25; // 0, 25 and 50
				// Density 40: over the Lead's gate (30), so it plays through the Groove.
				const Walked walked = walk(movingRecipe(full.genre, seed, 40, complexity), seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part != kLeadPart) {
						continue;
					}
					leads++;
					const int32_t half = stepAt(e) / kHalfBarSteps;
					expect(halves.against.contains(half)).to_be_true();
					// Respond never thins an anchor (§5.5): under the Bass line with second-half pickups (#368), the
					// second half is the busier, and the Lead's anchors there still play.
					const int32_t at = stepAt(e) % kStepsPerBar;
					expect(halves.against[half] && at != 8 && at != 10).to_be_false();
				}
				for (const auto& [half, against] : halves.against) {
					firstHalves += half % 2 == 0 && against ? 1 : 0;
				}
			}
			expect(leads).to_be_greater_than(200);
			expect(firstHalves).to_be_greater_than(200); // the offbeat bass makes the first half against
		});

		it("at its own strength (60) plays most Lead notes in the half-bar the Bass leaves quieter, thinning the busier half's lead-ins, mostly the second half", _ {
			int32_t against = 0;
			int32_t free = 0;
			int32_t first = 0;
			int32_t second = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				AgainstHalves halves;
				const Walked walked = walk(movingRecipe(houseGenre, seed, 40), seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part == kLeadPart) {
						(halves.against[stepAt(e) / kHalfBarSteps] ? against : free)++;
						(halfOf(stepAt(e)) == 0 ? first : second)++;
					}
				}
			}
			expect(free).to_be_greater_than(100);
			expect(against).to_be_greater_than(0); // 60 % thins the lead-ins, it doesn't drop them all
			// Most: the anchors play in a busier second half too (#368), and Density 40 opens more lead-ins.
			expect(against * 2).to_be_less_than(free);
			// The Bass's second-half line (#368) moves the answer to the first half now and then.
			expect(first).to_be_greater_than(0);
			expect(second).to_be_greater_than(0);
		});

		it("keeps about its strength's share of the busier half's lead-ins, not all or none in nearly every piece (spec §5.5)", _ {
			// Each lead-in's draw holds while the Lead's pick does, and a pick can hold for a whole Groove, so a line with
			// one lead-in a bar kept all of them or none in a piece. Two or three, each on its own step, average out,
			// but not in every piece since round 2 (a 2-phrase Intro, the Lead's gate): a piece now and then keeps all
			// or none (round 2's Later rounds: more Respond re-picks).
			// The strength is 60, plus 40 per 50 points of Complexity over the default: 40, 60 and 80 % thinned.
			const HouseWithRespond none(0, 0); // no thinning at any Complexity
			for (const auto& [complexity, keptShare] : {std::pair{0, 60}, std::pair{25, 40}, std::pair{50, 20}}) {
				int32_t kept = 0;
				int32_t all = 0;
				int32_t thinned = 0; // pieces that dropped some
				int32_t keptSome = 0;
				for (uint32_t seed = 1; seed <= 16; seed++) {
					// Lead-ins in the half-bars the Bass makes against: mostly the first halves, but where the Bass rests.
					auto firstHalf = [&](const Genre& genre) {
						AgainstHalves halves;
						int32_t n = 0;
						const Walked walked = walk(movingRecipe(genre, seed, 40, complexity), seed, 96 * kStepsPerBar);
						for (const Event& e : walked.noteOns) {
							const int32_t at = stepAt(e) % kStepsPerBar;
							const bool leadIn = at != 8 && at != 10; // anchors aren't thinned (§5.5)
							n += e.part == kLeadPart && leadIn && halves.against[stepAt(e) / kHalfBarSteps] ? 1 : 0;
						}
						return n;
					};
					const int32_t pieceKept = firstHalf(houseGenre);
					const int32_t pieceAll = firstHalf(none.genre);
					expect(pieceKept).to_be_less_than(pieceAll + 1);
					thinned += pieceKept < pieceAll ? 1 : 0;
					keptSome += pieceKept > 0 ? 1 : 0;
					kept += pieceKept;
					all += pieceAll;
				}
				expect(thinned).to_be_greater_than(13); // of 16
				if (complexity < 50) { // at 80 % a pick's two or three lead-ins all go now and then
					expect(keptSome).to_be_greater_than(13);
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
		it("takes its Arc from the brief and round 2: a 2-phrase Intro, Groove 4–8, Break 1–2 keeping Stab, Strings and Closed hat, Builds 4×1 and 8×2 with no cut and a 1-beat Gap", _ {
			const ArcSpec& arc = houseGenre.arc;
			expect(arc.introPhrases).to_equal(2);
			expect(arc.outroPhrases).to_equal(1);
			expect(arc.groovePhrases.low).to_equal(4);
			expect(arc.groovePhrases.high).to_equal(8);
			expect(arc.breakPhrases.low).to_equal(1);
			expect(arc.breakPhrases.high).to_equal(2);
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
		it("gives every synth part at least 3 presets on the card or in the pack, GEN House Stab the Stab's main pick", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(harness::fewestPartPresets(houseGenre)).to_be_greater_than(size_t{2});
			expect(harness::partPresetsMissing(houseGenre, card).empty()).to_be_true();
			expect(harness::mainPick(houseGenre, kStabPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN House Stab.XML x3"));
		});

		it("drops 000 Rich Saw Bass, 074 Electric Piano and 143 Chillout Pad, and keeps 069 FM Bells 2 (round 2: I1–I5)", _ {
			auto paths = [](int32_t part) {
				std::set<std::string> out;
				for (const PaletteEntry& entry : houseGenre.palette->partEntries(houseGenre.parts[part])) {
					out.insert(entry.path);
				}
				return out;
			};
			expect(paths(kBassPart).contains("SYNTHS/000 Rich Saw Bass.XML")).to_be_false();
			expect(paths(kStabPart).contains("SYNTHS/074 Electric Piano.XML")).to_be_false();
			expect(paths(kStringsPart).contains("SYNTHS/143 Chillout Pad.XML")).to_be_false();
			expect(paths(kLeadPart).contains("SYNTHS/069 FM Bells 2.XML")).to_be_true();
			expect(paths(kBassPart).size()).to_equal(size_t{4});
			expect(paths(kStabPart).size()).to_equal(size_t{5});
			expect(paths(kStringsPart).size()).to_equal(size_t{3});
			expect(paths(kLeadPart).size()).to_equal(size_t{4});
		});

		it("rolls the pack's claps and rides, only Risers of 6 s or less, the pack's boom alone as the Impact, the plain kicks, and 3 files or more on every ungated Drums lane", _ {
			FakeCard card = harness::factoryCardWithPack();
			// The lanes' own lists, the kit aside (spec §10.2): with it, the pack's claps never play, as all three
			// machines have a clap.
			auto picks = harness::lanePicks(houseGenre, card, 400, {}, false);
			expect(picks[Lane::Riser] == std::set<std::string>{"SAMPLES/FX/Riser/GEN Riser 2.wav", "SAMPLES/FX/Riser/GEN Riser 3.wav"})
			    .to_be_true();
			const auto picked = [&](Lane lane, const std::string& tag) {
				return std::ranges::any_of(picks[lane], [&](const std::string& path) { return path.find(tag) != std::string::npos; });
			};
			// The Impact: the owner's boom alone (#472, impact round 3's H1).
			expect(picks[Lane::Impact] == std::set<std::string>{"SYNTHS/GENERATIVE/FX/GEN Impact Boom.XML"}).to_be_true();
			// A tag matches any part of a name: "909 Kick" would roll the distorted and dry 909 kicks as kicks.
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
