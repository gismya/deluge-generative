// House (spec §4.5, #132): a genre of its own with six parts (Drums, Bass, Stab, Strings, Lead, FX), four on the floor
// with claps on 2 and 4, an offbeat bass, 7th stabs and Strings over a walking Chord graph, swung hats leaning late on
// the e and the a, and a Lead that Responds to the bass in each bar's second half. Run straight through the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/palette.h"
#include "generative/core/pitch.h"
#include "generative/core/variants.h"
#include "harness.h"
#include "palette_helpers.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
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
/// The variants-2 sets, by their index in House's list.
constexpr size_t kDrumsSet = 4;
constexpr size_t kStabSet = 5;
constexpr size_t kBreakSet = 6;

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

/// `recipe` with the variants-2 sets (drums, stab, break) on the genre as written, so a spec of the rest measures what
/// it did before them; the other sets as drawn.
Recipe beforeV2(Recipe recipe) {
	for (const char* fix : {"drums=classic", "stab=syncopated", "break=stabpad"}) {
		if (const std::optional<VariantFix> pin = parseVariantFix(*recipe.genre, fix)) {
			fixVariant(recipe.variants, *pin);
		}
	}
	return recipe;
}

/// `walked`'s note-ons and note-offs that `keep` takes, as (tick, kind, part, lane, pitch, velocity), sorted: events
/// due on one tick may leave in another order when a lane's notes around them change.
std::vector<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>>
notesWhere(const Walked& walked, const std::function<bool(const Event&)>& keep) {
	std::vector<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>> out;
	for (const Event& e : walked.notes) {
		if (keep(e)) {
			out.emplace_back(e.tick, static_cast<int32_t>(e.kind), e.part, static_cast<int32_t>(e.lane), e.pitch,
			                 e.velocity);
		}
	}
	std::ranges::sort(out);
	return out;
}

/// House as variant `fix` (`set=name`) patches it, the other sets on their first.
struct HouseAs {
	ResolvedGenre copy;
	const Genre& genre;
	explicit HouseAs(const char* fix)
	    : genre(resolveGenre(houseGenre, withVariants(beforeV2(newRecipe(houseGenre, 0)), {fix}).variants, &copy)) {}
};

/// The note-ons of `part` (and `lane`, for Drums and FX) in each Build's last `lastBars` bars and before them, over
/// `pieces` pieces of `recipe`'s variants.
struct BuildNotes {
	int32_t early = 0;
	int32_t late = 0;
};
BuildNotes buildNotes(const char* fix, int32_t part, Lane lane, int32_t lastBars) {
	BuildNotes notes;
	for (uint32_t seed = 1; seed <= 6; seed++) {
		const Walked walked =
		    walk(withVariants(beforeV2(movingRecipe(houseGenre, seed)), {fix}), seed, 128 * kStepsPerBar);
		const auto steps = static_cast<int32_t>(walked.steps.size());
		for (const Event& e : walked.noteOns) {
			const int32_t step = std::min<int32_t>(stepAt(e), steps - 1);
			if (walked.steps[step].section != SectionKind::Build || e.part != part
			    || ((part == kDrumsPart || part == kFxPart) && e.lane != lane)) {
				continue;
			}
			// Steps to the Build's end.
			int32_t end = step;
			while (end < steps && walked.steps[end].section == SectionKind::Build) {
				end++;
			}
			(end - step <= lastBars * kStepsPerBar ? notes.late : notes.early)++;
		}
	}
	return notes;
}

/// Where a bar sits for the hats and percussion (feel-1 F2): a Groove bar after its first phrase plays at home (the
/// Drop lift has decayed); a Drop's first 2 bars and a Build's last 2 are the most intense; anything else is neither.
enum class Intensity : uint8_t { Home, Intense, Other };
Intensity intensityAt(const Walked& walked, int32_t bar) {
	const auto steps = static_cast<int32_t>(walked.steps.size());
	const int32_t n = bar * kStepsPerBar;
	if (n + kStepsPerBar > steps) {
		return Intensity::Other;
	}
	const Walked::Step& step = walked.steps[n];
	if (step.section == SectionKind::Groove) {
		if (step.phraseInSection > 0) {
			return Intensity::Home;
		}
		// A Groove after a Build opens on a Drop; the first after the Intro doesn't, and has no lift.
		const bool dropped = walked.steps[n - step.stepInPhrase].drop;
		return dropped && step.stepInPhrase < 2 * kStepsPerBar ? Intensity::Intense : Intensity::Other;
	}
	if (step.section == SectionKind::Build) {
		const int32_t end = n + 2 * kStepsPerBar;
		return end < steps && walked.steps[end].section != SectionKind::Build ? Intensity::Intense : Intensity::Other;
	}
	return Intensity::Other;
}

/// Each Groove's chords, one a bar for its first phrase, in a walk: what a Drop's loop move changes.
std::vector<std::vector<int32_t>> grooveChords(const Walked& walked) {
	std::vector<std::vector<int32_t>> out;
	const auto steps = static_cast<int32_t>(walked.steps.size());
	for (int32_t n = 0; n + 8 * kStepsPerBar <= steps; n++) {
		if (walked.steps[n].section == SectionKind::Groove && walked.steps[n].sectionStart) {
			out.emplace_back();
			for (int32_t bar = 0; bar < 8; bar++) {
				out.back().push_back(walked.steps[n + bar * kStepsPerBar].chord);
			}
		}
	}
	return out;
}

/// The share, in percent, of Drops whose Groove plays other chords than the one before, over 30 pieces of S1's `fix`,
/// on the graph's loops: the vamp, the chords set's heavier pick (feel-2 G4), often draws the loop it left.
int32_t loopMoveShare(const char* fix) {
	int32_t drops = 0;
	int32_t moves = 0;
	for (uint32_t seed = 1; seed <= 30; seed++) {
		const std::vector<std::vector<int32_t>> grooves = grooveChords(walk(
		    withVariants(beforeV2(movingRecipe(houseGenre, seed)), {fix, "chords=loop"}), seed, 300 * kStepsPerBar));
		for (size_t i = 1; i < grooves.size(); i++) {
			drops++;
			moves += grooves[i] != grooves[i - 1] ? 1 : 0;
		}
	}
	return drops > 60 ? moves * 100 / drops : -1; // -1: too few Drops to measure
}

/// The Bass's notes in `scale`, key C, at `complexity`, over 6 pieces of S6's `fix`: all, and those off the chord
/// root; then the anchors (the & of each beat), and those off the root.
struct BassRoots {
	int32_t notes = 0;
	int32_t offRoot = 0;
	int32_t anchors = 0;
	int32_t offRootAnchors = 0;
};
BassRoots bassRoots(const char* fix, Scale scale, int32_t complexity) {
	BassRoots out;
	for (uint32_t seed = 1; seed <= 6; seed++) {
		Recipe recipe = withVariants(beforeV2(movingRecipe(houseGenre, scale, seed, 25, complexity)), {fix});
		recipe.keyRoot = 0;
		const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
		for (const Event& e : walked.noteOns) {
			const int32_t step = stepAt(e);
			if (e.part != kBassPart || step >= static_cast<int32_t>(walked.steps.size())) {
				continue;
			}
			const bool off = e.pitch % 12 != semitonesOf(walked.steps[step].chord, scale) % 12;
			out.notes++;
			out.offRoot += off ? 1 : 0;
			if (step % 4 == 2) {
				out.anchors++;
				out.offRootAnchors += off ? 1 : 0;
			}
		}
	}
	return out;
}

} // namespace

// clang-format off
describe house("House", $ {
	it("is a genre of its own with six parts: Drums, Bass, Stab, Strings, Lead and FX (spec §4.5)", _ {
		expect(std::string(houseGenre.name)).to_equal(std::string("house"));
		expect(std::string(houseGenre.oledName)).to_equal(std::string("HOUSE"));
		expect(houseGenre.parent == nullptr).to_be_true();
		expect(houseGenre.version).to_equal(8);
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

	it("plays 120–126 BPM in Minor, Dorian or Mixolydian, swing +10 to +14, in 8-bar phrases, every macro at 25", _ {
		expect(houseGenre.facts.tempoLowBpm).to_equal(120);
		expect(houseGenre.facts.tempoHighBpm).to_equal(126);
		const std::set<Scale> scales(houseGenre.facts.scales.begin(), houseGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Dorian, Scale::Mixolydian}).to_be_true();
		expect(houseGenre.facts.swingLow).to_equal(int8_t{10});
		expect(houseGenre.facts.swingHigh).to_equal(int8_t{14}); // feel-1 F6: no gallop past a triplet (#518)
		expect(houseGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(houseGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("loops a Chord graph from home i: i–iv, i–♭VII, iv–♭VII, i–♭VI and ♭VI–♭VII both ways, 2 bars a chord, repeat 8, cadence ♭VII every 4, Extensions 1–2, no guarded chord, a new loop on 1 Drop in 3, one move in 3 going back to an earlier loop", _ {
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
		expect(graph->returnChance).to_equal(int16_t{333}); // the owner's round 2 note (#496)
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
				Recipe recipe = beforeV2(movingRecipe(plain, seed));
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
				const Walked leaned = walk(beforeV2(movingRecipe(houseGenre, seed, 40)), seed, 32 * kStepsPerBar);
				const Walked plain = walk(beforeV2(movingRecipe(unleaned, seed, 40)), seed, 32 * kStepsPerBar);
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
				const Walked walked = walk(beforeV2(newRecipe(houseGenre, seed)), seed, 96 * kStepsPerBar);
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

		it("plays one open hat a bar at home, on the & of 4, and the other &s only above home, behind more Density (feel-1 F2)", _ {
			const std::span<const Pattern> pool = laneOf(Lane::OpenHat).pool;
			expect(pool.size()).to_equal(size_t{1});
			for (int32_t step = 0; step < kStepsPerBar; step++) {
				expect(int32_t{pool[0].importance[step]}).to_equal(step == 14 ? 7 : step % kStepsPerBeat == 2 ? 5 : 0);
			}
			// Under House's jitter (±45) an importance 5 never meets the home threshold and always meets Density 31's.
			const int32_t lowest = 500 - houseGenre.densityJitter;
			const int32_t highest = 500 + houseGenre.densityJitter;
			expect(highest).to_be_less_than(houseGenre.densityCurve.threshold(macroPoints(25)));
			expect(lowest).to_be_greater_than(houseGenre.densityCurve.threshold(macroPoints(31)));
		});

		it("gates the shaker's 16ths at 28 and the Ride and Congas at 29: none in a Groove at home (feel-1 F2, F6)", _ {
			expect(laneOf(Lane::Perc1).densityGate).to_equal(macroPoints(28));
			expect(laneOf(Lane::Ride).densityGate).to_equal(macroPoints(29));
			expect(laneOf(Lane::Perc2).densityGate).to_equal(macroPoints(29));
			for (const Pattern& pattern : laneOf(Lane::Perc1).pool) {
				for (int32_t step = 0; step < pattern.length; step++) {
					expect(pattern.importance[step]).to_be_greater_than(uint8_t{6}); // every 16th, once the gate opens
				}
			}
		});

		it("plays the open hat on every &, the shaker, the Ride and the Congas in a Drop's first bars and a Build's last, and only one open hat a bar and none of them at home", _ {
			std::map<Intensity, int32_t> bars;
			std::map<Intensity, int32_t> fourHats;  // bars with an open hat on each &
			std::map<Intensity, int32_t> extraHats; // bars with an open hat off the & of 4
			std::map<Intensity, std::map<Lane, int32_t>> percBars;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(beforeV2(newRecipe(houseGenre, seed)), seed, 200 * kStepsPerBar);
				std::map<int32_t, std::set<int32_t>> hats; // by bar, the steps in it
				std::map<Lane, std::set<int32_t>> perc;    // by lane, the bars
				for (const Event& e : walked.noteOns) {
					if (e.part != kDrumsPart) {
						continue;
					}
					const int32_t step = stepAt(e);
					if (e.lane == Lane::OpenHat) {
						hats[step / kStepsPerBar].insert(step % kStepsPerBar);
					}
					if (e.lane == Lane::Perc1 || e.lane == Lane::Ride || e.lane == Lane::Perc2) {
						perc[e.lane].insert(step / kStepsPerBar);
					}
				}
				for (int32_t bar = 0; bar * kStepsPerBar < static_cast<int32_t>(walked.steps.size()); bar++) {
					const Intensity at = intensityAt(walked, bar);
					bars[at]++;
					const std::set<int32_t>& steps = hats[bar];
					fourHats[at] += steps == std::set<int32_t>{2, 6, 10, 14} ? 1 : 0;
					extraHats[at] += std::ranges::any_of(steps, [](int32_t s) { return s != 14; }) ? 1 : 0;
					for (const auto& [lane, laneBars] : perc) {
						percBars[at][lane] += laneBars.contains(bar) ? 1 : 0;
					}
				}
			}
			expect(bars[Intensity::Home]).to_be_greater_than(300);
			expect(bars[Intensity::Intense]).to_be_greater_than(20);
			expect(extraHats[Intensity::Home]).to_equal(0);
			expect(fourHats[Intensity::Intense] * 10).to_be_greater_than(bars[Intensity::Intense] * 7);
			for (const Lane lane : {Lane::Perc1, Lane::Ride, Lane::Perc2}) {
				expect(percBars[Intensity::Home][lane]).to_equal(0);
				expect(percBars[Intensity::Intense][lane] * 10).to_be_greater_than(bars[Intensity::Intense] * 8);
			}
		});

		it("lets the Stab, the Strings, both, the kick or the hats leave a Groove for 4 or 8 bars on 4-bar lines (feel-1 F3)", _ {
			expect(houseGenre.dropOuts.size()).to_equal(size_t{6});
			expect(houseGenre.dropOutChance).to_equal(int16_t{500});
			expect(houseGenre.dropOutGapBars).to_equal(uint8_t{4});
			expect(houseGenre.dropOutGridBars).to_equal(uint8_t{4});
			const PartsAndLanes hats{.lanes = static_cast<uint16_t>(laneBit(Lane::ClosedHat) | laneBit(Lane::OpenHat))};
			const std::vector<std::pair<PartsAndLanes, int32_t>> entries = {
			    {{.parts = partBit(kStabPart)}, 4},
			    {{.parts = partBit(kStabPart)}, 8},
			    {{.parts = partBit(kStringsPart)}, 8},
			    {{.parts = static_cast<uint8_t>(partBit(kStabPart) | partBit(kStringsPart))}, 8},
			    {{.lanes = laneBit(Lane::Kick)}, 4},
			    {hats, 4}};
			for (size_t i = 0; i < entries.size(); i++) {
				expect(houseGenre.dropOuts[i].mask == entries[i].first).to_be_true();
				expect(int32_t{houseGenre.dropOuts[i].bars}).to_equal(entries[i].second);
			}
			// Played: whole Groove bars with no Stab, and whole Groove bars with no kick.
			int32_t noStab = 0;
			int32_t noKick = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(beforeV2(newRecipe(houseGenre, seed)), seed, 160 * kStepsPerBar);
				std::set<int32_t> stab;
				std::set<int32_t> kick;
				for (const Event& e : walked.noteOns) {
					if (e.part == kStabPart) {
						stab.insert(stepAt(e) / kStepsPerBar);
					}
					if (e.part == kDrumsPart && e.lane == Lane::Kick) {
						kick.insert(stepAt(e) / kStepsPerBar);
					}
				}
				for (int32_t bar = 0; (bar + 1) * kStepsPerBar <= static_cast<int32_t>(walked.steps.size()); bar++) {
					const Walked::Step& step = walked.steps[bar * kStepsPerBar];
					if (step.section == SectionKind::Groove && step.phraseInSection > 0) {
						noStab += stab.contains(bar) ? 0 : 1;
						noKick += kick.contains(bar) ? 0 : 1;
					}
				}
			}
			expect(noStab).to_be_greater_than(8);
			expect(noKick).to_be_greater_than(3);
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
		it("plays the 3-3-2 riff: the 1 with the kick, the a of 1, the & of 2, the & of 3 and the e of 4, 60-tick notes that fit between onsets 3 16ths apart, busier in the first half (feel-1 F1)", _ {
			const PartSpec& bass = houseGenre.parts[kBassPart];
			expect(bass.pool.size()).to_equal(size_t{1});
			const Pattern& riff = bass.pool[0];
			std::vector<int32_t> onsets;
			for (int32_t step = 0; step < riff.length; step++) {
				if (riff.importance[step] != 0) {
					onsets.push_back(step);
				}
			}
			expect(onsets).to_equal(std::vector<int32_t>{0, 3, 6, 10, 13});
			expect(riff.importance[0]).to_equal(kAnchor);
			expect(riff.importance[6]).to_equal(kAnchor);
			expect(int32_t{bass.loop.gateTicks}).to_equal(60);
			expect(int32_t{bass.loop.gateTicks}).to_be_less_than(3 * kTicksPerStep);
			expect(bass.loop.rootOnAnchors).to_be_true();
		});

		it("with the offbeat line anchors the Bass on the & of every beat, never on a beat, each line's pickups in one half of the bar, a tier-1 line's in the second (#368)", _ {
			const HouseAs offbeat("bass=fifth");
			const PartSpec& bass = offbeat.genre.parts[kBassPart];
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

		it("meets the kick only on the 1 with the riff, and never with the offbeat line", _ {
			for (const auto& [complexity, seed] : {std::pair{0, 1u}, std::pair{25, 2u}, std::pair{50, 3u}, std::pair{50, 4u}}) {
				const Walked riff =
				    walk(withVariants(beforeV2(movingRecipe(houseGenre, seed, 40, complexity)), {"bass=riff"}), seed, 64 * kStepsPerBar);
				std::set<int32_t> kicks;
				for (const Event& e : riff.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::Kick) {
						kicks.insert(stepAt(e));
					}
				}
				int32_t ones = 0;
				for (const Event& e : riff.noteOns) {
					if (e.part == kBassPart && kicks.contains(stepAt(e))) {
						expect(stepAt(e) % kStepsPerBar).to_equal(0);
						ones++;
					}
				}
				expect(ones).to_be_greater_than(16);
			}
			for (const auto& [complexity, seed] : {std::pair{0, 1u}, std::pair{25, 2u}, std::pair{50, 3u}, std::pair{50, 4u}}) {
				const Walked walked =
				    walk(withVariants(beforeV2(movingRecipe(houseGenre, seed, 40, complexity)), {"bass=fifth"}), seed, 64 * kStepsPerBar);
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
				const Walked walked = walk(beforeV2(movingRecipe(houseGenre, seed, 25, complexity)), seed, 64 * kStepsPerBar);
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
				Recipe recipe = beforeV2(newRecipe(houseGenre, seed));
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

		it("moves the riff between the root, the 5th and the 7th below, the anchors on the root, now and then an octave up (feel-1 F1)", _ {
			const LoopSpec& loop = houseGenre.parts[kBassPart].loop;
			expect(loop.draws.leap.atZero).to_equal(int16_t{4});
			expect(loop.draws.leap.atFull).to_equal(int16_t{6});
			expect(loop.draws.octaveUp.atZero).to_equal(int16_t{150});
			std::map<int32_t, int32_t> degrees;
			for (const LineWeight& degree : loop.degrees) {
				degrees[degree.value] = degree.weight;
			}
			expect(degrees == std::map<int32_t, int32_t>{{0, 5}, {4, 3}, {6, 2}}).to_be_true();
			const BassRoots roots = bassRoots("bass=riff", Scale::Minor, 25);
			expect(roots.notes).to_be_greater_than(500);
			expect(roots.offRoot).to_be_greater_than(0);
		});

		it("lets the offbeat line's Bass leap to the 5th from Complexity 25 (#369)", _ {
			const LoopSpec& loop = HouseAs("bass=fifth").genre.parts[kBassPart].loop;
			expect(loop.draws.leap.atZero).to_equal(int16_t{3});
			expect(loop.draws.leap.atFull).to_equal(int16_t{6});
			int32_t notes = 0;
			int32_t offRoot = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				Recipe recipe = withVariants(beforeV2(movingRecipe(houseGenre, Scale::Minor, seed)), {"bass=fifth"});
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
				const Walked walked = walk(beforeV2(movingRecipe(houseGenre, seed)), seed, 128 * kStepsPerBar);
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
				const Walked walked = walk(beforeV2(movingRecipe(full.genre, seed, 40, complexity)), seed, 96 * kStepsPerBar);
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
				const Walked walked = walk(beforeV2(movingRecipe(houseGenre, seed, 40)), seed, 96 * kStepsPerBar);
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
						const Walked walked = walk(beforeV2(movingRecipe(genre, seed, 40, complexity)), seed, 96 * kStepsPerBar);
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
					Recipe recipe = beforeV2(movingRecipe(houseGenre, root + 1u, 40, 50));
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
		it("takes its Arc from the brief, round 2 and feel-1: a 2-phrase Intro, Groove 4–8, Break 1–2 keeping Stab, Strings and Closed hat, 8-bar Builds with no cut, a 1-beat Gap and a snare roll up to 16ths", _ {
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
			expect(builds == std::map<int32_t, int32_t>{{8, 1}}).to_be_true(); // no Build takes half a 1-phrase Break
			expect(arc.buildCutBars).to_equal(0);
			expect(arc.gap == GapLength::Beat).to_be_true();
			expect(arc.roll.lane == Lane::Snare).to_be_true();
			expect(arc.roll.top == RollStage::Sixteenth).to_be_true();
			expect(arc.buildCutLeads).to_be_false();
			expect(arc.buildRiser).to_be_true();
			expect(arc.dropDecayPhrases).to_equal(1);
			expect(cutAndGapUnderBuild(houseGenre)).to_be_true();
		});

		it("recedes to the Stab, the Strings and the closed hat in a Break, and Builds with everything", _ {
			int32_t buildSteps = 0;
			int32_t breakNotes = 0;
			std::set<int32_t> buildParts;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				// S4a on today's Build (#496): its other variants leave the Lead or the kick and Bass out.
				const Walked walked = walk(withVariants(beforeV2(movingRecipe(houseGenre, seed)), {"build=full"}), seed, 96 * kStepsPerBar);
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

	context("its variant sets (#461, #496, ADR 0008)", _ {
		it("lists S1's loop changes (live), the Build, the Bass, the chords, the drums (live, changing at a Drop), the stab "
		   "(live) and the Break, the genre as written first in each",
		   _ {
			const std::span<const VariantSet> sets = houseGenre.variantSets;
			expect(sets.size()).to_equal(size_t{7}); // one slot left for a LEAD set (variants-2's Later rounds)
			std::vector<std::vector<std::string>> names;
			std::vector<std::vector<int32_t>> weights;
			std::vector<bool> live;
			for (const VariantSet& set : sets) {
				names.push_back({set.name, set.oledName});
				weights.emplace_back();
				for (const Variant& variant : set.variants) {
					names.back().emplace_back(variant.name);
					weights.back().push_back(variant.weight);
				}
				expect(set.variants[0].apply == nullptr).to_be_true(); // the genre as written
				live.push_back(set.live);
			}
			expect(names).to_equal(std::vector<std::vector<std::string>>{
			    {"loops", "LOOPS", "third", "half", "every", "never"},
			    {"build", "BUILD", "full", "nolead", "nokick", "filter"},
			    {"bass", "BASS", "riff", "fifth", "root", "pickups"},
			    {"chords", "CHORDS", "loop", "vamp"},
			    {"drums", "DRUMS", "classic", "deep", "jackin", "tribal"},
			    {"stab", "STAB", "syncopated", "offbeat", "clave"},
			    {"break", "BREAK", "stabpad", "pads", "piano", "bassline", "lead"}});
			// The filter Build 5 to the three roll Builds' 4, the riff 3 to 1 over the offbeat lines (feel-1), the vamp 2
			// to 1 over the loop (feel-2 G4). variants-2: the owner's "often", "sometimes" and "rarely" as 3, 2 and 1.
			expect(weights).to_equal(std::vector<std::vector<int32_t>>{
			    {1, 6, 1, 1}, {2, 1, 1, 5}, {9, 1, 1, 1}, {1, 2}, {3, 2, 2, 2}, {3, 1, 2}, {2, 1, 2, 1, 1}});
			// The Build is read ahead of its line, the Bass pools differ in size, a live set may not move the chord
			// lines (ADR 0008), and the Arc reads the Break ahead.
			expect(live).to_equal(std::vector<bool>{true, false, false, false, true, true, false});
			for (size_t s = 0; s < sets.size(); s++) {
				expect(sets[s].changeAt == (s == kDrumsSet ? ChangeAt::Drop : ChangeAt::Never)).to_be_true();
			}
			expect(sets[kDrumsSet].changeChance).to_equal(int16_t{300}); // "turns tribal at the Drop": often
		});

		it("S1: a new Recipe draws a new loop on 1 Drop in 2 about two thirds of the time (the owner, #472 round 3)", _ {
			int32_t half = 0;
			constexpr uint32_t kPieces = 900;
			for (uint32_t entropy = 1; entropy <= kPieces; entropy++) {
				half += beforeV2(newRecipe(houseGenre, entropy)).variants.pick[0] == 1 ? 1 : 0;
			}
			expect(half * 100 / static_cast<int32_t>(kPieces)).to_be_between(60, 73); // 6 of 9
		});

		it("S1: a new loop on 1 Drop in 3, 1 in 2, every Drop or never, each going back to an earlier loop at 333", _ {
			std::vector<int32_t> chances;
			for (const char* fix : {"loops=third", "loops=half", "loops=every", "loops=never"}) {
				const HouseAs house(fix);
				const ChordGraph& graph = *house.genre.harmony.graph;
				chances.push_back(graph.loopChance);
				expect(graph.returnChance).to_equal(int16_t{333});
				expect(graph.halfLoopChance).to_equal(houseGenre.harmony.graph->halfLoopChance);
				expect(graph.barsPerChord).to_equal(houseGenre.harmony.graph->barsPerChord);
				expect(graph.moves.data() == houseGenre.harmony.graph->moves.data()).to_be_true();
			}
			expect(chances).to_equal(std::vector<int32_t>{333, 500, 1000, 0});
			// Played: how often a Drop's Groove plays other chords. A new loop can draw the old one's chords, and a
			// 1000 move can go back to an earlier loop, so the shares sit under the chances.
			std::vector<int32_t> shares;
			for (const char* fix : {"loops=never", "loops=third", "loops=half", "loops=every"}) {
				shares.push_back(loopMoveShare(fix));
			}
			expect(shares[0]).to_equal(0);
			expect(shares[1]).to_be_between(15, 40);
			expect(shares[2]).to_be_greater_than(shares[1]);
			expect(shares[3]).to_be_greater_than(shares[2]);
			expect(shares[3]).to_be_greater_than(65);
		});

		it("S1 is live: a switch to every Drop before the first Drop moves the loop on that Drop, as a piece drawn with it", _ {
			int32_t differs = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				// The loop's graph: the vamp, the chords set's heavier pick (feel-2 G4), often draws the loop it left.
				const Recipe never = withVariants(beforeV2(movingRecipe(houseGenre, seed)), {"loops=never", "chords=loop"});
				const Walked switched = walk(never, seed, 160 * kStepsPerBar, [](int32_t n, const Generator&, SteeringQueue& queue) {
					if (n == 0) {
						queue.push({CommandKind::SetVariantMask, 0, 0b0100}); // loops: every
					}
				});
				const Walked every = walk(withVariants(never, {"loops=every"}), seed, 160 * kStepsPerBar);
				const Walked stays = walk(never, seed, 160 * kStepsPerBar);
				expect(switched.generator->variants().pick[0]).to_equal(uint8_t{2});
				expect(grooveChords(switched).size()).to_be_greater_than(size_t{1});
				expect(grooveChords(switched) == grooveChords(every)).to_be_true();
				differs += grooveChords(switched) != grooveChords(stays) ? 1 : 0;
			}
			expect(differs).to_be_greater_than(0);
		});

		it("S4a: the Build with everything, with no Lead, or with no kick (feel-2 G2 dropped the 2-bar cut)", _ {
			const BuildNotes full = buildNotes("build=full", kDrumsPart, Lane::Kick, 2);
			expect(full.early).to_be_greater_than(0);
			expect(full.late).to_be_greater_than(0);
			expect(buildNotes("build=nolead", kLeadPart, Lane::None, 0).early).to_equal(0);
			expect(buildNotes("build=full", kLeadPart, Lane::None, 0).early).to_be_greater_than(0);
			const BuildNotes kick = buildNotes("build=nokick", kDrumsPart, Lane::Kick, 2);
			expect(kick.early + kick.late).to_equal(0);
			expect(buildNotes("build=nokick", kStabPart, Lane::None, 0).early).to_be_greater_than(0);
		});

		it("nokick brings the Bass in on the Build's last bar, as filter does, and keeps the roll and the Riser (#518 R1)", _ {
			const ArcSpec& arc = HouseAs("build=nokick").genre.arc;
			expect(arc.buildCutLeads).to_be_true();
			expect(int32_t{arc.buildCutBars}).to_equal(1);
			expect(arc.buildCut == PartsAndLanes{.parts = partBit(kBassPart)}).to_be_true();
			const BuildNotes bass = buildNotes("build=nokick", kBassPart, Lane::None, 1);
			expect(bass.early).to_equal(0);
			expect(bass.late).to_be_greater_than(10);
			expect(buildNotes("build=nokick", kDrumsPart, Lane::Snare, 0).early).to_be_greater_than(0);
			expect(buildNotes("build=nokick", kFxPart, Lane::Riser, 0).early).to_be_greater_than(0);
		});

		it("feel-1 F4: the filter Build has no roll, Gap or Riser, no kick, and the Bass only in its last bar, leading into the Drop (#518)", _ {
			const ArcSpec& arc = HouseAs("build=filter").genre.arc;
			expect(arc.roll.lane == Lane::None).to_be_true();
			expect(arc.gap == GapLength::None).to_be_true();
			expect(arc.buildRiser).to_be_false();
			expect(arc.buildCutLeads).to_be_true();
			expect(int32_t{arc.buildCutBars}).to_equal(1);
			const BuildNotes kick = buildNotes("build=filter", kDrumsPart, Lane::Kick, 1);
			expect(kick.early + kick.late).to_equal(0);
			for (const Lane lane : {Lane::Snare, Lane::Riser}) {
				const BuildNotes none = buildNotes("build=filter", lane == Lane::Riser ? kFxPart : kDrumsPart, lane, 1);
				expect(none.early + none.late).to_equal(0);
			}
			const BuildNotes bass = buildNotes("build=filter", kBassPart, Lane::None, 1);
			expect(bass.early).to_equal(0);
			expect(bass.late).to_be_greater_than(10);
			expect(buildNotes("build=filter", kStabPart, Lane::None, 0).early).to_be_greater_than(0);
			// The roll Builds keep their Riser.
			expect(buildNotes("build=full", kFxPart, Lane::Riser, 0).early).to_be_greater_than(0);
		});

		it("S6: the offbeat line with the 5th from Complexity 25, the root alone below 34, or pickups on the 5th and the 7th below with five more rhythms", _ {
			const LoopSpec& fifth = HouseAs("bass=fifth").genre.parts[kBassPart].loop;
			expect(fifth.draws.leap.atZero).to_equal(int16_t{3});
			const HouseAs root("bass=root");
			expect(root.genre.parts[kBassPart].loop.draws.leap.atZero).to_equal(int16_t{0});
			expect(root.genre.parts[kBassPart].loop.draws.leap.atFull).to_equal(int16_t{6});
			const HouseAs pickups("bass=pickups");
			const PartSpec& bass = pickups.genre.parts[kBassPart];
			expect(bass.loop.rootOnAnchors).to_be_true();
			expect(bass.loop.draws.leap.atZero).to_equal(int16_t{4});
			expect(bass.loop.draws.leap.atFull).to_equal(int16_t{8});
			expect(bass.pool.size()).to_equal(HouseAs("bass=fifth").genre.parts[kBassPart].pool.size() + 5);
			expect(int32_t{bass.loop.gateTicks}).to_equal(20); // the pickups' adjacent 16ths
		});

		it("S6 played: the 5th from Complexity 25 with fifth, the root alone there with root, and the 5th at 50 in both", _ {
			// The leap is a cap in scale steps (the 5th is 4): fifth's 3–6 reaches it from Complexity 25, root's 0–6
			// only from 34.
			for (const char* fix : {"bass=root", "bass=fifth"}) {
				expect(bassRoots(fix, Scale::Minor, 0).offRoot).to_equal(0);
				expect(bassRoots(fix, Scale::Minor, 50).offRoot).to_be_greater_than(0);
			}
			const BassRoots root = bassRoots("bass=root", Scale::Minor, 25);
			expect(root.notes).to_be_greater_than(300);
			expect(root.offRoot).to_equal(0);
			expect(bassRoots("bass=fifth", Scale::Minor, 25).offRoot).to_be_greater_than(0);
		});

		it("S6 played: pickups keep every anchor (the &s) on the chord root and move pickups off it, in every scale", _ {
			for (const Scale scale : houseGenre.facts.scales) {
				for (const int32_t complexity : {25, 50}) {
					const BassRoots roots = bassRoots("bass=pickups", scale, complexity);
					expect(roots.anchors).to_be_greater_than(300);
					expect(roots.offRootAnchors).to_equal(0);
					expect(roots.offRoot).to_be_greater_than(0);
				}
			}
		});
	});

	context("its chords set (feel-1 F5, #518)", _ {
		it("vamps i–iv, 2 bars each, every loop two chords, keeping the loop chance the loops set drew", _ {
			for (const auto& [loops, chance] : {std::pair{"loops=third", 333}, std::pair{"loops=half", 500},
			                                    std::pair{"loops=every", 1000}, std::pair{"loops=never", 0}}) {
				ResolvedGenre copy;
				const Genre& genre =
				    resolveGenre(houseGenre, withVariants(beforeV2(newRecipe(houseGenre, 0)), {loops, "chords=vamp"}).variants, &copy);
				const ChordGraph& graph = *genre.harmony.graph;
				expect(int32_t{graph.loopChance}).to_equal(chance);
				expect(graph.moves.size()).to_equal(size_t{1});
				expect(graph.moves[0].from).to_equal(kI);
				expect(graph.moves[0].to).to_equal(kIV);
				expect(graph.repeatWeight).to_equal(0);
				expect(graph.cadenceTarget).to_equal(kIV);
				expect(graph.halfLoopChance).to_equal(int16_t{1000});
				expect(graph.barsPerChord).to_equal(2);
			}
			ResolvedGenre copy;
			const Genre& loop =
			    resolveGenre(houseGenre, withVariants(beforeV2(newRecipe(houseGenre, 0)), {"loops=third", "chords=loop"}).variants, &copy);
			expect(loop.harmony.graph == houseGenre.harmony.graph).to_be_true();
		});

		it("played: the vamp's chords are i and iv alone, each 2 bars; the loop's reach ♭VI or ♭VII", _ {
			std::set<int32_t> vampChords;
			std::set<int32_t> loopChords;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				Recipe recipe = beforeV2(movingRecipe(houseGenre, Scale::Minor, seed));
				recipe.keyRoot = 0;
				const Walked vamp = walk(withVariants(recipe, {"chords=vamp"}), seed, 128 * kStepsPerBar);
				for (const Walked::Step& step : vamp.steps) {
					vampChords.insert(step.chord);
				}
				for (const int32_t change : vamp.changes()) {
					expect(change % (2 * kStepsPerBar)).to_equal(0);
				}
				for (const Walked::Step& step : walk(withVariants(recipe, {"chords=loop"}), seed, 128 * kStepsPerBar).steps) {
					loopChords.insert(step.chord);
				}
			}
			expect(vampChords == std::set<int32_t>{kI, kIV}).to_be_true();
			expect(loopChords.contains(kVI) || loopChords.contains(kVII)).to_be_true();
		});

		it("played across Drops: each Drop's new loop stays i–iv in Minor and Dorian; Mixolydian's colour move adds ♭VII", _ {
			// The Generator-wide colour row (chord_walk.h) adds I–♭VII to whichever graph plays in Mixolydian, so its
			// vamp's loops are I–IV or I–♭VII, each two chords from home.
			for (const Scale scale : {Scale::Minor, Scale::Dorian, Scale::Mixolydian}) {
				std::set<int32_t> chords;
				size_t grooves = 0;
				for (uint32_t seed = 1; seed <= 6; seed++) {
					Recipe recipe = beforeV2(movingRecipe(houseGenre, scale, seed));
					recipe.keyRoot = 0;
					const Walked walked = walk(withVariants(recipe, {"chords=vamp", "loops=every"}), seed, 300 * kStepsPerBar);
					for (const std::vector<int32_t>& groove : grooveChords(walked)) {
						grooves++;
						const std::set<int32_t> played(groove.begin(), groove.end());
						expect(played.size()).to_equal(size_t{2});
						expect(played.contains(kI)).to_be_true();
						chords.insert(groove.begin(), groove.end());
					}
				}
				expect(grooves).to_be_greater_than(size_t{30});
				const std::set<int32_t> expected =
				    scale == Scale::Mixolydian ? std::set<int32_t>{kI, kIV, kVII} : std::set<int32_t>{kI, kIV};
				expect(chords == expected).to_be_true();
			}
		});
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		const harness::Lost lost = harness::lostAtEveryCorner(houseGenre);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its variants-2 sets: drums, stab and break (#133)", _ {
		it("plays each drum style in its own lanes only: deep the hats and the shaker, jackin the rim and the hats, "
		   "tribal the closed hat, the shaker and the congas",
		   _ {
			struct Claim {
				const char* fix;
				std::vector<Lane> lanes;
			};
			const Claim claims[] = {{"drums=deep", {Lane::ClosedHat, Lane::OpenHat, Lane::Perc1}},
			                        {"drums=jackin", {Lane::Rim, Lane::ClosedHat, Lane::OpenHat}},
			                        {"drums=tribal", {Lane::ClosedHat, Lane::Perc1, Lane::Perc2}}};
			for (const Claim& claim : claims) {
				std::map<Lane, int32_t> moved;
				for (uint32_t seed = 1; seed <= 4; seed++) {
					const Recipe written = beforeV2(movingRecipe(houseGenre, seed));
					const Walked as = walk(written, seed, 96 * kStepsPerBar);
					const Walked styled = walk(withVariants(written, {claim.fix}), seed, 96 * kStepsPerBar);
					const auto claimed = [&](const Event& e) {
						return e.part == kDrumsPart && std::ranges::count(claim.lanes, e.lane) > 0;
					};
					expect(notesWhere(styled, std::not_fn(claimed)) == notesWhere(as, std::not_fn(claimed))).to_be_true();
					for (Lane lane : claim.lanes) {
						const auto inLane = [&](const Event& e) { return e.part == kDrumsPart && e.lane == lane; };
						moved[lane] += notesWhere(styled, inLane) != notesWhere(as, inLane) ? 1 : 0;
					}
				}
				for (Lane lane : claim.lanes) {
					expect(moved[lane]).to_be_greater_than(0);
				}
			}
		});

		it("plays each stab rhythm in the Stab only: offbeat on the &s, clave never on a beat", _ {
			std::map<std::string, std::pair<int32_t, int32_t>> onAnd;  // onsets on an &, all onsets
			std::map<std::string, int32_t> onBeat;
			for (const char* fix : {"stab=syncopated", "stab=offbeat", "stab=clave"}) {
				for (uint32_t seed = 1; seed <= 4; seed++) {
					const Recipe written = beforeV2(movingRecipe(houseGenre, seed));
					const Walked as = walk(written, seed, 96 * kStepsPerBar);
					const Walked played = walk(withVariants(written, {fix}), seed, 96 * kStepsPerBar);
					const auto notStab = [](const Event& e) { return e.part != kStabPart; };
					expect(notesWhere(played, notStab) == notesWhere(as, notStab)).to_be_true();
					std::set<int32_t> onsets;
					for (const Event& e : played.noteOns) {
						if (e.part == kStabPart) {
							onsets.insert(stepAt(e));
						}
					}
					for (int32_t step : onsets) {
						onAnd[fix].first += step % 4 == 2 ? 1 : 0;
						onAnd[fix].second++;
						onBeat[fix] += step % 4 == 0 ? 1 : 0;
					}
				}
			}
			const auto share = [&](const char* fix) { return onAnd[fix].first * 100 / std::max(1, onAnd[fix].second); };
			expect(share("stab=offbeat")).to_be_greater_than(80);
			expect(share("stab=syncopated")).to_be_less_than(60);
			expect(onBeat["stab=clave"]).to_equal(0);
			expect(onBeat["stab=syncopated"]).to_equal(0);
		});

		it("keeps each Break's parts and lanes alone in the Break, and plays the rest of the piece as written, but for the "
		   "lead Break's Build, which rises from its higher Density",
		   _ {
			struct Claim {
				const char* fix;
				std::set<int32_t> parts;
				std::set<Lane> lanes;
				bool buildMoves = false; // a Build ramps from where the Section before it left Density (spec §9.4)
			};
			const Claim claims[] = {
			    {"break=stabpad", {kStabPart, kStringsPart}, {Lane::ClosedHat}},
			    {"break=pads", {kStringsPart}, {Lane::ClosedHat}},
			    {"break=piano", {kStabPart}, {}},
			    {"break=bassline", {kBassPart}, {Lane::ClosedHat, Lane::OpenHat}},
			    {"break=lead", {kStringsPart, kLeadPart}, {}, true},
			};
			for (const Claim& claim : claims) {
				std::set<int32_t> parts;
				std::set<Lane> lanes;
				for (uint32_t seed = 1; seed <= 6; seed++) {
					const Recipe written = beforeV2(movingRecipe(houseGenre, seed));
					const Walked as = walk(written, seed, 128 * kStepsPerBar);
					const Walked played = walk(withVariants(written, {claim.fix}), seed, 128 * kStepsPerBar);
					const auto steps = static_cast<int32_t>(played.steps.size());
					const auto sectionOf = [&](const Event& e) { return played.steps[std::min(stepAt(e), steps - 1)].section; };
					const auto inBreak = [&](const Event& e) { return sectionOf(e) == SectionKind::Break; };
					// Note-ons: a part the Break leaves out has no note to end on the Build's first tick.
					const auto playsOutside = [&](const Event& e) {
						return e.kind == EventKind::NoteOn && !inBreak(e)
						       && !(claim.buildMoves && sectionOf(e) == SectionKind::Build);
					};
					expect(notesWhere(played, playsOutside) == notesWhere(as, playsOutside)).to_be_true();
					for (const Event& e : played.noteOns) {
						if (inBreak(e) && !e.muted && e.part != kFxPart) { // the FX play their own Break
							(e.part == kDrumsPart ? lanes.insert(e.lane).second : parts.insert(e.part).second);
						}
					}
				}
				expect(parts == claim.parts).to_be_true();
				expect(lanes == claim.lanes).to_be_true();
			}
		});
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
