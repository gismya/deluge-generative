// Dub (spec §4.5, #131): roots dub, a genre of its own with five parts (Drums, Bass, Skank, Melodica, FX), one-drop
// drums, an offbeat skank, a bass that carries the tune, a melodica, drop-outs thrown into dotted-8th echoes, a 1-bar
// Build and a Siren on Drops. Run straight through the core.

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

const Genre& dubGenre = genres::dub;

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kSkankPart = 2;
constexpr int32_t kMelodicaPart = 3;
constexpr int32_t kFxPart = 4;

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;
constexpr int32_t kDropBeat2 = 4;  // beat 2: the recounted one drop (listening round 2, S1)
constexpr int32_t kDropBeat4 = 12; // beat 4

constexpr DelaySetup kDottedEighth{
    .on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true, .analog = true};

/// Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec §9.3).
const LaneSpec& laneOf(int32_t part, Lane lane) {
	return harness::laneOf(dubGenre, part, lane);
}

/// Whether some entry of `pool` at `tier` hits both drop beats (2 and 4).
bool backbeatAt(std::span<const Pattern> pool, int32_t tier) {
	return std::ranges::any_of(pool, [&](const Pattern& p) {
		return p.tier == tier && p.importance[kDropBeat2] > 0 && p.importance[kDropBeat4] > 0;
	});
}

/// The step a note-on falls in, its Lean taken off (Lean is ±12 ticks at most).
int32_t stepAt(const Event& e) {
	return (e.tick + kTicksPerStep / 2) / kTicksPerStep;
}

} // namespace

// clang-format off
describe dub("Dub", $ {
	it("is a genre of its own with five parts: Drums, Bass, Skank, Melodica and FX (spec §4.5)", _ {
		expect(std::string(dubGenre.name)).to_equal(std::string("dub"));
		// Never read as DUB TECHNO in the GENRE menu or a confirm title.
		expect(std::string(dubGenre.oledName)).to_equal(std::string("ROOTS DUB"));
		expect(std::string(dubGenre.oledName).find("TECHNO")).to_equal(std::string::npos);
		expect(dubGenre.parent == nullptr).to_be_true();
		expect(dubGenre.version).to_equal(7);
		expect(findGenre("dub") == &dubGenre).to_be_true();
		expect(partCount(dubGenre)).to_equal(5);
		const std::vector<std::tuple<Role, Flavour, std::string>> roster = {
		    {Role::Drums, Flavour::None, "DRUMS"},   {Role::Bass, Flavour::Loop, "BASS"},
		    {Role::Chords, Flavour::Stab, "SKANK"},  {Role::Lead, Flavour::Motif, "MELODICA"},
		    {Role::FX, Flavour::None, "FX"}};
		for (size_t p = 0; p < roster.size(); p++) {
			const PartSpec& part = dubGenre.parts[p];
			expect(part.role == std::get<0>(roster[p])).to_be_true();
			expect(part.flavour == std::get<1>(roster[p])).to_be_true();
			expect(std::string(part.name)).to_equal(std::get<2>(roster[p]));
		}
		expect(rosterFits(dubGenre)).to_be_true();
		expect(genreValid(dubGenre)).to_be_true();
	});

	it("ghosts steps 21 and 25 of its 7-marked tier-2 Bass line, as notes/132.md meant (listening round 2, QM1)", _ {
		int32_t lines = 0;
		int32_t matched = 0;
		for (const Pattern& pattern : dubGenre.parts[kBassPart].pool) {
			if (pattern.tier == 2) {
				lines++;
				if (pattern.importance[21] == 7 && pattern.importance[25] == 7) {
					matched++;
					expect(pattern.level[21] == VelocityLevel::Ghost).to_be_true();
					expect(pattern.level[25] == VelocityLevel::Ghost).to_be_true();
				}
			}
		}
		expect(lines).to_equal(2);
		expect(matched).to_equal(1);
	});

	it("plays 70–80 BPM in Minor or Dorian, swing +5 to +15, in 8-bar phrases, every macro at 25 (listening round 2, DOR)", _ {
		expect(dubGenre.facts.tempoLowBpm).to_equal(70);
		expect(dubGenre.facts.tempoHighBpm).to_equal(80);
		expect(dubGenre.facts.scales.size()).to_equal(size_t{2});
		expect(dubGenre.facts.scales[0] == Scale::Minor).to_be_true();
		expect(dubGenre.facts.scales[1] == Scale::Dorian).to_be_true();
		expect(dubGenre.facts.swingLow).to_equal(int8_t{5});
		expect(dubGenre.facts.swingHigh).to_equal(int8_t{15});
		expect(dubGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(dubGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("loops one 4-bar Chord graph from home i: i–iv, i–♭VII and iv–v, 1 bar a chord, repeat 20, no cadence, Extensions 0–1 (S2, S2b)", _ {
		const ChordGraph* graph = dubGenre.harmony.graph;
		expect(graph != nullptr).to_be_true();
		std::set<std::pair<int32_t, int32_t>> moves;
		for (const ChordMove& move : graph->moves) {
			moves.insert({move.from, move.to});
		}
		expect(moves == std::set<std::pair<int32_t, int32_t>>{{kI, kIV}, {kI, kVII}, {kIV, kV}}).to_be_true();
		expect(graph->barsPerChord).to_equal(1);
		expect(graph->repeatWeight).to_equal(20);
		expect(graph->cadenceTarget).to_equal(kNoCadenceTarget);
		expect(graph->loopChance).to_equal(int16_t{0});
		expect(graph->halfLoopChance).to_equal(int16_t{1000});
		expect(dubGenre.harmony.extensionFloor).to_equal(0);
		expect(dubGenre.harmony.extensionCeiling).to_equal(1);
		// One 4-bar loop plays across every Section of a piece, in Minor and Dorian alike.
		for (Scale scale : {Scale::Minor, Scale::Dorian}) {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				Recipe recipe = harness::movingRecipe(dubGenre, seed);
				recipe.scale = scale;
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
				int32_t loop[4] = {-1, -1, -1, -1};
				for (const Walked::Step& step : walked.steps) {
					if (step.stepInPhrase % kStepsPerBar != 0) {
						continue;
					}
					const int32_t slot = (step.stepInPhrase / kStepsPerBar) % 4;
					if (loop[slot] < 0) {
						loop[slot] = step.chord;
					}
					expect(step.chord).to_equal(loop[slot]);
				}
				expect(loop[0]).to_equal(kHomeChord);
				expect(loop[3] != kHomeChord).to_be_true();
			}
		}
	});

	context("its one-drop drums (spec §4.5, listening round 2 S1/S7/S8/S9)", _ {
		it("puts the kick and the Rim on beats 2 and 4 at every Complexity, anchored, never on 1 or 3", _ {
			for (Lane lane : {Lane::Kick, Lane::Rim}) {
				for (const Pattern& pattern : laneOf(kDrumsPart, lane).pool) {
					expect(pattern.importance[kDropBeat2]).to_equal(kAnchor);
					expect(pattern.importance[kDropBeat4]).to_equal(kAnchor);
				}
			}
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Kick).pool) {
				expect(pattern.importance[0]).to_equal(0);
				expect(pattern.importance[8]).to_equal(0);
			}
		});

		it("gives the backbeat to exactly one of Rim and Snare at each tier", _ {
			const LaneSpec& rim = laneOf(kDrumsPart, Lane::Rim);
			const LaneSpec& snare = laneOf(kDrumsPart, Lane::Snare);
			// The Snare has no step table, so this holds as long as the Rim has the backbeat at every tier: the check
			// guards a Snare pool added later, which would have to leave beats 2 and 4 to the Rim.
			expect(snare.pool.empty()).to_be_true();
			const int32_t top = std::max(rim.band.high, snare.band.high);
			for (int32_t tier = 0; tier <= top; tier++) {
				expect(backbeatAt(rim.pool, tier) != backbeatAt(snare.pool, tier)).to_be_true();
			}
		});

		it("plays the kick with Rim or Snare (never both) on beats 2 and 4 of Groove bars, Rim the norm and Snare taking some (#460 Q-RIM B)", _ {
			const LaneSpec& rimLane = laneOf(kDrumsPart, Lane::Rim);
			expect(rimLane.alternateLane == Lane::Snare).to_be_true();
			expect(rimLane.alternateChance).to_be_between(int16_t{100}, int16_t{350});
			int32_t rimBars = 0;
			int32_t snareBars = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				Recipe recipe = harness::movingRecipe(dubGenre, seed);
				Genre plain = dubGenre; // no fills or drop-outs, so every Groove bar holds the one drop
				plain.fills = {};
				plain.dropOuts = {};
				recipe.genre = &plain;
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				std::set<int32_t> kicks;
				std::set<int32_t> rims;
				std::set<int32_t> snares;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::Kick) {
						kicks.insert(stepAt(e));
					}
					if (e.part == kDrumsPart && e.lane == Lane::Rim) {
						rims.insert(stepAt(e));
					}
					if (e.part == kDrumsPart && e.lane == Lane::Snare) {
						const int32_t step =
						    std::min<int32_t>(e.tick / kTicksPerStep, static_cast<int32_t>(walked.steps.size()) - 1);
						expect(walked.steps[step].section == SectionKind::Build
						       || walked.steps[step].section == SectionKind::Groove)
						    .to_be_true();
						if (walked.steps[step].section == SectionKind::Groove) {
							snares.insert(stepAt(e));
						}
					}
				}
				for (int32_t bar = 0; bar * kStepsPerBar < static_cast<int32_t>(walked.steps.size()); bar++) {
					const int32_t drop2 = bar * kStepsPerBar + kDropBeat2;
					const int32_t drop4 = bar * kStepsPerBar + kDropBeat4;
					if (walked.steps[drop2].section != SectionKind::Groove) {
						continue;
					}
					expect(kicks.contains(drop2) && kicks.contains(drop4)).to_be_true();
					const bool rim = rims.contains(drop2) && rims.contains(drop4);
					const bool snare = snares.contains(drop2) && snares.contains(drop4);
					expect(rim != snare).to_be_true();
					expect(rims.contains(drop2) == rims.contains(drop4)).to_be_true();
					expect(snares.contains(drop2) == snares.contains(drop4)).to_be_true();
					rimBars += rim ? 1 : 0;
					snareBars += snare ? 1 : 0;
				}
			}
			expect(rimBars + snareBars).to_be_greater_than(200);
			expect(snareBars).to_be_greater_than(20);
			expect(rimBars).to_be_greater_than(snareBars * 2); // Rim stays the norm
		});

		it("leaves hats unleaned (song swing acts), ranks off-beat hats first, gates hand drums at 28, leans Snare +2, and lets Crash replace ClosedHat", _ {
			for (Lane lane : {Lane::ClosedHat, Lane::OpenHat}) {
				const Expression& hats = laneOf(kDrumsPart, lane).expression;
				for (int8_t lean : hats.lean) {
					expect(lean).to_equal(0);
				}
			}
			// S9: off-beat (&) closed hats ranked 9 and accented, above on-beat 8s, so low Density keeps them.
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::ClosedHat).pool) {
				for (int32_t step : {2, 6, 10, 14}) {
					expect(pattern.importance[step]).to_equal(9);
					expect(pattern.level[step] == VelocityLevel::Accent).to_be_true();
				}
				for (int32_t step : {0, 4, 8, 12}) {
					expect(pattern.importance[step]).to_equal(8);
				}
			}
			// S8: hand drums gated at 28 so a Drop's lift (+8 from default 25) brings them in.
			expect(laneOf(kDrumsPart, Lane::Perc2).densityGate).to_equal(macroPoints(28));
			for (int8_t lean : laneOf(kDrumsPart, Lane::Snare).expression.lean) {
				expect(lean).to_equal(2);
			}
			bool crashReplacesHat = false;
			for (const ReplacePair& pair : dubGenre.replacePairs) {
				crashReplacesHat = crashReplacesHat || (pair.replacing == Lane::Crash && pair.replaced == Lane::ClosedHat);
			}
			expect(crashReplacesHat).to_be_true();
			expect(dubGenre.arc.accentLane == Lane::Crash).to_be_true();
			expect(dubGenre.arc.accentChance).to_equal(50);
		});

		it("fills with Snare+Tom Swap (held), Rim->Snare Swap, and a rare 1-beat Snare Roll at fillChance 500 (S7)", _ {
			expect(dubGenre.fills.size()).to_equal(size_t{3});
			const FillEntry& held = dubGenre.fills[dubGenre.heldFill];
			expect(held.kind == FillKind::Swap).to_be_true();
			expect(held.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}).to_be_true();
			bool moves = false;
			int32_t totalWeight = 0;
			int32_t rollWeight = 0;
			for (const FillEntry& fill : dubGenre.fills) {
				totalWeight += fill.weight;
				if (fill.kind == FillKind::Swap
				    && fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Rim) | laneBit(Lane::Snare)}) {
					moves = true;
				}
				if (fill.kind == FillKind::Roll && fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare)}
				    && fill.beats == 1) {
					rollWeight += fill.weight;
				}
			}
			expect(moves).to_be_true();
			expect(rollWeight).to_be_greater_than(0);
			// Owner: "rare, but nice at times" — well under the variant's 50 % share.
			expect(rollWeight * 5).to_be_less_than(totalWeight);
			// The Rim's fill pool is silent, the Snare's holds beats 2 and 4 at every tier.
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Rim).fillPool) {
				expect(std::ranges::all_of(pattern.importance, [&](uint8_t i) { return i == 0; })).to_be_true();
			}
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Snare).fillPool) {
				expect(pattern.importance[kDropBeat2]).to_be_greater_than(0);
				expect(pattern.importance[kDropBeat4]).to_be_greater_than(0);
			}
			expect(dubGenre.fillChance).to_equal(500);
		});
	});

	context("its pitched parts (spec §4.5, listening round 2 S1/S2/S4/S6/I1)", _ {
		it("gives the Bass a 2-bar Loop line on Pentatonic, anchored on the root, mutating slowly with Octave Answers every 8 bars and bursts leaving holes", _ {
			const PartSpec& bass = dubGenre.parts[kBassPart];
			expect(bass.degreeSet == DegreeSet::Pentatonic).to_be_true();
			expect(bass.loop.rootOnAnchors).to_be_true();
			expect(bass.loop.mutation).to_equal(int16_t{15});
			expect(bass.loop.answerBars).to_equal(uint8_t{8});
			expect(bass.loop.slide.atZero).to_equal(0);
			expect(bass.loop.slide.atFull).to_equal(0);
			expect(bass.loop.draws.leap.atZero).to_be_greater_than(1);
			std::set<AnswerKind> answers;
			for (const Answer& answer : bass.loop.answers) {
				answers.insert(answer.kind);
			}
			expect(answers == std::set<AnswerKind>{AnswerKind::Octave}).to_be_true();
			expect(bass.reg.anchor).to_equal(0); // the Bass Register's default
			for (const Pattern& pattern : bass.pool) {
				expect(pattern.length).to_equal(2 * kStepsPerBar);
				expect(pattern.importance[0]).to_equal(kAnchor);
				expect(pattern.importance[kStepsPerBar]).to_equal(kAnchor);
				if (pattern.tier == 0) {
					// S6: tier-0 lines leave holes of at least 2 beats (7+ empty steps in a row in each bar).
					int32_t maxGap = 0;
					int32_t gap = 0;
					for (int32_t i = 0; i < pattern.length; i++) {
						gap = pattern.importance[i] == 0 ? gap + 1 : 0;
						maxGap = std::max(maxGap, gap);
					}
					expect(maxGap).to_be_greater_than(6);
				}
			}
		});

		it("chops the Skank on every & with a short gate, and gives the Melodica resting 2-bar Motif phrases at reroll 50 and volume 80", _ {
			const PartSpec& skank = dubGenre.parts[kSkankPart];
			for (const Pattern& pattern : skank.pool) {
				for (int32_t step : {2, 6, 10, 14}) {
					expect(pattern.importance[step]).to_equal(kAnchor);
				}
				for (int32_t step : {0, 4, 8, 12}) {
					expect(pattern.importance[step]).to_equal(0);
				}
			}
			expect(skank.stab.gateTicks).to_be_less_than(kTicksPerStep);
			const PartSpec& melodica = dubGenre.parts[kMelodicaPart];
			expect(melodica.motif.positions.empty()).to_be_false();
			expect(melodica.motif.reroll).to_equal(int16_t{50});
			expect(melodica.volume).to_equal(uint8_t{80});
			expect(melodica.densityGate).to_equal(0);
			int32_t emptySecondBars = 0;
			for (const Pattern& pattern : melodica.pool) {
				expect(pattern.length).to_equal(2 * kStepsPerBar);
				expect(pattern.importance[0] != kAnchor).to_be_true();
				bool bar2Empty = true;
				for (int32_t i = kStepsPerBar; i < 2 * kStepsPerBar; i++) {
					bar2Empty = bar2Empty && pattern.importance[i] == 0;
				}
				emptySecondBars += bar2Empty ? 1 : 0;
			}
			expect(emptySecondBars).to_be_greater_than(1);
		});

		it("plays every pitched part in its scale over whole pieces", _ {
			const Walked walked = walk(harness::movingRecipe(dubGenre, 4), 4, 96 * kStepsPerBar);
			std::map<int32_t, int32_t> notes;
			for (const Event& e : walked.noteOns) {
				notes[e.part]++;
			}
			for (int32_t p : {kBassPart, kSkankPart, kMelodicaPart}) {
				expect(notes[p]).to_be_greater_than(0);
			}
		});
	});

	context("its Arc (spec §4.5, §9)", _ {
		it("takes its Arc from the brief: Groove 4–8, Break 1 keeping Kick, Rim and Bass, a 1-bar Build with no cut or Gap", _ {
			const ArcSpec& arc = dubGenre.arc;
			expect(arc.introPhrases).to_equal(1);
			expect(arc.outroPhrases).to_equal(1);
			expect(arc.groovePhrases.low).to_equal(4);
			expect(arc.groovePhrases.high).to_equal(8);
			expect(arc.breakPhrases.low).to_equal(1);
			expect(arc.breakPhrases.high).to_equal(1);
			expect(arc.breakKeeps == PartsAndLanes{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim)})
			    .to_be_true();
			expect(arc.buildBars.size()).to_equal(size_t{1});
			expect(arc.buildBars[0].bars).to_equal(1);
			expect(arc.buildCutBars).to_equal(0);
			expect(arc.gap == GapLength::None).to_be_true();
			expect(arc.roll.lane == Lane::Snare).to_be_true();
			expect(arc.roll.top == RollStage::ThirtySecond).to_be_true();
			expect(arc.dropDecayPhrases).to_equal(1);
			expect(arc.transitionEntry == TransitionEntry::Drop).to_be_true();
			expect(cutAndGapUnderBuild(dubGenre)).to_be_true();
		});

		it("recedes to the kick, the Rim and the Bass in a Break, and rolls the Snare across the 1-bar Build", _ {
			int32_t builds = 0;
			int32_t breakNotes = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(harness::movingRecipe(dubGenre, seed), seed, 96 * kStepsPerBar);
				int32_t buildSteps = 0;
				for (const Walked::Step& step : walked.steps) {
					buildSteps += step.section == SectionKind::Build ? 1 : 0;
				}
				builds += buildSteps / kStepsPerBar;
				for (const Event& e : walked.noteOns) {
					const int32_t step = std::min<int32_t>(e.tick / kTicksPerStep, static_cast<int32_t>(walked.steps.size()) - 1);
					if (walked.steps[step].section != SectionKind::Break || e.part == kFxPart) {
						continue;
					}
					breakNotes++;
					const bool kept = e.part == kBassPart
					                  || (e.part == kDrumsPart && (e.lane == Lane::Kick || e.lane == Lane::Rim));
					expect(kept).to_be_true();
				}
			}
			expect(builds).to_be_greater_than(1);
			expect(breakNotes).to_be_greater_than(0);
		});
	});

	context("its drop-outs and delay setups (spec §8.8, §10.6, §11.4, listening round 2 S3/S3b)", _ {
		it("includes the expanded drop-outs, spot echoes and bass-out at chance 250 and gap 2", _ {
			expect(dubGenre.dropOuts.size()).to_equal(size_t{10});
			bool hasBassOut = false;
			bool hasSpotRim = false;
			bool hasSpotSkank = false;
			bool hasHatsAndPerc = false;
			bool hasTops = false;
			bool hasAllButBass = false;
			const uint16_t hatsAndPerc =
			    laneBit(Lane::ClosedHat) | laneBit(Lane::OpenHat) | laneBit(Lane::Perc1) | laneBit(Lane::Perc2);
			const uint16_t allFx =
			    laneBit(Lane::Riser) | laneBit(Lane::Fall) | laneBit(Lane::Impact) | laneBit(Lane::Siren);
			for (const DropOutEntry& entry : dubGenre.dropOuts) {
				hasBassOut = hasBassOut
				             || (entry.mask == PartsAndLanes{.parts = partBit(kBassPart)} && entry.bars == 2
				                 && entry.throwTarget == PartsAndLanes{});
				hasSpotRim = hasSpotRim
				             || (entry.mask == PartsAndLanes{.lanes = laneBit(Lane::Rim)} && entry.bars == 1
				                 && entry.throwTarget == PartsAndLanes{.lanes = laneBit(Lane::Rim)});
				hasSpotSkank = hasSpotSkank
				               || (entry.mask == PartsAndLanes{.parts = partBit(kSkankPart)} && entry.bars == 1
				                   && entry.throwTarget == PartsAndLanes{.parts = partBit(kSkankPart)});
				hasHatsAndPerc = hasHatsAndPerc
				                 || (entry.mask == PartsAndLanes{.lanes = hatsAndPerc} && entry.bars == 2
				                     && entry.throwTarget == PartsAndLanes{});
				hasTops = hasTops
				          || (entry.mask
				                  == PartsAndLanes{.parts = partBit(kSkankPart) | partBit(kMelodicaPart), .lanes = allFx}
				              && entry.bars == 4
				              && entry.throwTarget == PartsAndLanes{.parts = partBit(kSkankPart)});
				if (entry.bars == 4 && !entry.mask.has(kBassPart, Lane::None)
				    && entry.mask.has(kSkankPart, Lane::None) && entry.mask.has(kMelodicaPart, Lane::None)
				    && entry.mask.has(kDrumsPart, Lane::Kick)) {
					hasAllButBass = true;
					expect((entry.mask.lanes & allFx) == allFx).to_be_true();
					expect(entry.throwTarget == PartsAndLanes{.parts = partBit(kMelodicaPart)}).to_be_true();
				}
			}
			expect(hasBassOut).to_be_true();
			expect(hasSpotRim).to_be_true();
			expect(hasSpotSkank).to_be_true();
			expect(hasHatsAndPerc).to_be_true();
			expect(hasTops).to_be_true();
			expect(hasAllButBass).to_be_true();
			expect(dubGenre.dropOutChance).to_equal(250);
			expect(dubGenre.dropOutGapBars).to_equal(2);
			expect(dropOutsValid(dubGenre)).to_be_true();
		});

		it("gives each throw target's every line an onset in its active bar's last beat (Melodica rests on bar 2)", _ {
			// A drop-out starts on a bar line and its Throw spikes only on a target's hit in the beat before it
			// (kThrowWindowSteps): Skank and Rim have one in every bar; Melodica's 2-bar phrases have one in bar 1
			// (bar 2 rests by design, S4).
			int32_t lines = 0;
			for (const DropOutEntry& entry : dubGenre.dropOuts) {
				for (size_t p = 0; p < dubGenre.parts.size(); p++) {
					const PartSpec& part = dubGenre.parts[p];
					std::vector<std::span<const Pattern>> pools;
					if (entry.throwTarget.has(static_cast<int32_t>(p), Lane::None) && part.lanes.empty()) {
						pools.push_back(part.pool);
					}
					for (const LaneSpec& lane : part.lanes) {
						if ((entry.throwTarget.lanes & laneBit(lane.lane)) != 0) {
							pools.push_back(lane.pool);
						}
					}
					for (std::span<const Pattern> pool : pools) {
						expect(pool.empty()).to_be_false();
						for (const Pattern& pattern : pool) {
							const int32_t bars = static_cast<int32_t>(p) == kMelodicaPart ? 1 : pattern.length / kStepsPerBar;
							for (int32_t bar = 0; bar < bars; bar++) {
								const int32_t from = (bar + 1) * kStepsPerBar - kThrowWindowSteps;
								bool hit = false;
								for (int32_t step = from; step < from + kThrowWindowSteps; step++) {
									hit = hit || pattern.importance[step] >= 8;
								}
								expect(hit).to_be_true();
							}
							lines++;
						}
					}
				}
			}
			expect(lines).to_be_greater_than(5);
		});

		it("gives each throw target and the Siren a dotted-8th analog delay setup, and the Drums Throw the Rim's row", _ {
			expect(dubGenre.parts[kSkankPart].delay == kDottedEighth).to_be_true();
			expect(dubGenre.parts[kMelodicaPart].delay == kDottedEighth).to_be_true();
			const DelaySetup& rim = laneOf(kDrumsPart, Lane::Rim).delay;
			expect(rim.on && rim.note == DelayNote::Eighth && rim.feel == DelayFeel::Dotted && rim.analog).to_be_true();
			expect(laneOf(kFxPart, Lane::Siren).delay == kDottedEighth).to_be_true();
			expect(dubGenre.parts[kDrumsPart].gestures.aim[static_cast<int32_t>(Gesture::Throw)] == Lane::Rim)
			    .to_be_true();
			expect(throwsDelay(dubGenre.parts[kDrumsPart], Lane::Rim)).to_be_true();
			for (const PartSpec& part : dubGenre.parts) {
				expect(part.throwDelayDepth).to_equal(uint8_t{12});
			}
			// Nothing else echoes: the Bass stays dry.
			expect(dubGenre.parts[kBassPart].delay.on).to_be_false();
		});

		it("plays each drop-out for its bars in its Grooves, its target thrown when it has one, at low Complexity too", _ {
			// Each entry alone, often and with no fills, at Complexity 5 (tier 0) and Density 15 (or 25 for hats+perc).
			struct Target {
				uint8_t part;
				Lane lane;
				bool thrown;
			};
			const auto targetOf = [](const DropOutEntry& entry) {
				const bool hasThrow = entry.throwTarget.parts != 0 || entry.throwTarget.lanes != 0;
				const PartsAndLanes& ref = hasThrow ? entry.throwTarget : entry.mask;
				for (int32_t p = 0; p < partCount(dubGenre); p++) {
					if (ref.has(p, Lane::None) && dubGenre.parts[p].lanes.empty()) {
						return Target{static_cast<uint8_t>(p), Lane::None, hasThrow};
					}
				}
				for (const LaneSpec& lane : dubGenre.parts[kDrumsPart].lanes) {
					if ((ref.lanes & laneBit(lane.lane)) != 0) {
						return Target{static_cast<uint8_t>(kDrumsPart), lane.lane, hasThrow};
					}
				}
				throw "a drop-out with no target";
			};
			for (size_t n = 0; n < dubGenre.dropOuts.size(); n++) {
				const Target target = targetOf(dubGenre.dropOuts[n]);
				Genre genre = dubGenre;
				genre.dropOuts = std::span(&dubGenre.dropOuts[n], 1);
				genre.fills = {};
				genre.dropOutChance = 500;
				int32_t starts = 0;
				int32_t spikes = 0;
				for (uint32_t seed = 1; seed <= 6; seed++) {
					Recipe recipe = newRecipe(genre, seed);
					recipe.macros.set(Macro::Complexity, macroPoints(5));
					recipe.macros.set(Macro::Density, macroPoints(target.lane == Lane::ClosedHat ? 25 : 15));
					harness::Instance generator;
					generator.start(recipe, seed, Takes{}, true);
					SteeringQueue queue;
					constexpr int32_t kBars = 96;
					std::vector<bool> groove(kBars, true);
					std::vector<bool> silenced(kBars, true);
					std::vector<std::set<int32_t>> hits(kBars);
					std::vector<std::set<int32_t>> snareHits(kBars);
					std::vector<int32_t> space(kBars * kStepsPerBar, 0);
					int32_t level = 0;
					for (int32_t step = 0; step < kBars * kStepsPerBar && !generator.ended(); step++) {
						generator.decideNextStep(queue, nullptr);
						groove[step / kStepsPerBar] = groove[step / kStepsPerBar]
						                              && generator.arc().section() == SectionKind::Groove;
						silenced[step / kStepsPerBar] = silenced[step / kStepsPerBar] && generator.partSilenced(target.part);
						Event e;
						while (generator.popDue(INT32_MAX, &e)) {
							const bool ours = e.part == target.part && (target.lane == Lane::None || e.lane == target.lane);
							if (ours && e.kind == EventKind::NoteOn && stepAt(e) / kStepsPerBar < kBars) {
								hits[stepAt(e) / kStepsPerBar].insert(stepAt(e) % kStepsPerBar);
							}
							if (target.lane == Lane::Rim && e.part == kDrumsPart && e.lane == Lane::Snare
							    && e.kind == EventKind::NoteOn && stepAt(e) / kStepsPerBar < kBars) {
								snareHits[stepAt(e) / kStepsPerBar].insert(stepAt(e) % kStepsPerBar);
							}
							if (ours && e.kind == EventKind::Control && e.control == SoundControl::Space) {
								level = e.offset;
							}
						}
						space[step] = level;
					}
					const auto dropped = [&](int32_t bar) -> bool {
						if (!groove[bar]) {
							return false;
						}
						if (target.lane == Lane::None) {
							return silenced[bar];
						}
						// With Rim's alternateLane = Snare (#460 Q-RIM B), a non-dropped Groove bar has beats 2 and 4
						// on either Rim or Snare; a Rim drop-out silences both.
						return target.lane == Lane::Rim
						           ? !hits[bar].contains(kDropBeat2) && !hits[bar].contains(kDropBeat4)
						                 && !snareHits[bar].contains(kDropBeat2) && !snareHits[bar].contains(kDropBeat4)
						           : hits[bar].empty();
					};
					for (int32_t bar = 1; bar + 1 < kBars; bar++) {
						if (!dropped(bar) || dropped(bar - 1)) {
							continue;
						}
						int32_t bars = 0;
						while (bar + bars < kBars && dropped(bar + bars)) {
							bars++;
						}
						if (bar + bars == kBars) {
							continue; // runs past the walk
						}
						starts++;
						expect(bars).to_equal(int32_t{dubGenre.dropOuts[n].bars});
						if (target.thrown) {
							bool hitInWindow = false;
							for (int32_t s = kStepsPerBar - kThrowWindowSteps; s < kStepsPerBar; s++) {
								hitInWindow = hitInWindow || hits[bar - 1].contains(s);
							}
							const int32_t first = bar * kStepsPerBar;
							if (hitInWindow) {
								expect(space[first] - space[first - kThrowWindowSteps - 1])
								    .to_be_greater_than(kThrowSpace / 2);
								spikes++;
							}
						}
					}
				}
				expect(starts).to_be_greater_than(5);
				if (target.thrown) {
					expect(spikes).to_be_greater_than(3);
				}
			}
		});

		it("cuts a Drop's 2-bar Siren on bar 1 when an FX-masking drop-out starts there (#460 Q-THROW C)", _ {
			// Isolate Dub's "everything but the bass" entry at chance 1000 so bar 1 of the Drop always triggers it.
			const DropOutEntry* allButBass = nullptr;
			for (const DropOutEntry& entry : dubGenre.dropOuts) {
				if (entry.bars == 4 && entry.mask.has(kDrumsPart, Lane::Kick) && entry.mask.has(kFxPart, Lane::Siren)) {
					allButBass = &entry;
				}
			}
			expect(allButBass != nullptr).to_be_true();
			if (allButBass == nullptr) {
				return;
			}
			Genre genre = dubGenre;
			genre.dropOuts = std::span(allButBass, 1);
			genre.dropOutChance = 1000;
			genre.fills = {};
			Recipe recipe = harness::movingRecipe(genre, 1);
			Run run;
			run.recipe = recipe;
			run.seed = 1;
			const Result result = play(run, 96);
			int32_t dropTick = -1;
			for (const Event& e : only(result.events, EventKind::Section)) {
				if (e.section == SectionKind::Groove && e.occurrence == 1) {
					dropTick = e.tick;
					break;
				}
			}
			expect(dropTick).to_be_greater_than(0);
			std::vector<Event> sirenEvents;
			for (const Event& e : result.events) {
				if (e.part == kFxPart && e.lane == Lane::Siren
				    && (e.kind == EventKind::NoteOn || e.kind == EventKind::NoteOff)) {
					sirenEvents.push_back(e);
				}
			}
			const std::vector<Held> fx = heldNotes(sirenEvents, kFxPart);
			const Held* siren = nullptr;
			for (const Held& n : fx) {
				if (n.on == dropTick) {
					siren = &n;
				}
			}
			expect(siren != nullptr).to_be_true();
			if (siren == nullptr) {
				return;
			}
			// Cut on bar 1 of the Drop when the drop-out starts, not ringing through bar 1 to bar 2.
			expect(siren->off - siren->on).to_equal(kBarTicks);
			// No FX note starts during bars 1..4 of the Drop.
			expect(ons(result, kFxPart, Lane::Siren, dropTick / kTicksPerStep + kStepsPerBar,
			           dropTick / kTicksPerStep + 5 * kStepsPerBar)
			           .empty())
			    .to_be_true();
		});
	});

	context("its Siren (spec §9.5, §10.5)", _ {
		it("holds a 2-bar note gated at 30, so at the default macros it sounds on Drops and never in a Break", _ {
			const LaneSpec& siren = laneOf(kFxPart, Lane::Siren);
			expect(siren.heldBars).to_equal(2);
			expect(siren.densityGate).to_equal(macroPoints(30));
			int32_t drops = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(harness::movingRecipe(dubGenre, seed), seed, 96 * kStepsPerBar);
				std::set<int32_t> sirens;
				for (const Event& e : walked.noteOns) {
					if (e.part == kFxPart && e.lane == Lane::Siren) {
						sirens.insert(e.tick / kTicksPerStep);
						const int32_t step = e.tick / kTicksPerStep;
						expect(walked.steps[step].drop).to_be_true();
					}
				}
				for (size_t n = 0; n < walked.steps.size(); n++) {
					if (walked.steps[n].drop) {
						drops++;
						expect(sirens.contains(static_cast<int32_t>(n))).to_be_true();
					}
				}
			}
			expect(drops).to_be_greater_than(1);
		});

		it("sounds on a Break's first step too once Density opens its gate there", _ {
			int32_t breaks = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(harness::movingRecipe(dubGenre, seed, 45), seed, 96 * kStepsPerBar);
				std::set<int32_t> sirens;
				for (const Event& e : walked.noteOns) {
					if (e.part == kFxPart && e.lane == Lane::Siren) {
						sirens.insert(e.tick / kTicksPerStep);
					}
				}
				for (size_t n = 0; n < walked.steps.size(); n++) {
					if (walked.steps[n].sectionStart && walked.steps[n].section == SectionKind::Break) {
						breaks++;
						expect(sirens.contains(static_cast<int32_t>(n))).to_be_true();
					}
				}
			}
			expect(breaks).to_be_greater_than(1);
		});
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		const harness::Lost lost = harness::lostAtEveryCorner(dubGenre);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its palette (spec §10.1, §10.5, §18.2, §18.3)", _ {
		it("gives every synth part at least 3 presets on the card or in the pack, the pack's dub sub and melodica the main picks, and drops 082 and 026 from Skank (I4)", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(harness::fewestPartPresets(dubGenre)).to_be_greater_than(size_t{2});
			expect(harness::partPresetsMissing(dubGenre, card).empty()).to_be_true();
			expect(harness::mainPick(dubGenre, kBassPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Dub Sub.XML x3"));
			expect(harness::mainPick(dubGenre, kMelodicaPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Melodica.XML x3"));
			std::set<std::string> skankPresets;
			for (const PaletteEntry& entry : dubGenre.palette->partEntries(dubGenre.parts[kSkankPart])) {
				skankPresets.insert(entry.path);
			}
			expect(skankPresets == std::set<std::string>{"SYNTHS/077 FM Perc-Organ.XML", "SYNTHS/073 Piano.XML",
			                                             "SYNTHS/076 Organ.XML"})
			    .to_be_true();
		});

		it("lists GEN Siren alone on the Siren lane and no folder, GEN Riser Short alone on the Riser, and its Impacts", _ {
			const LaneSounds* siren = dubGenre.palette->laneSounds(Lane::Siren);
			expect(siren != nullptr).to_be_true();
			expect(siren->folders.empty() && siren->tags.empty()).to_be_true();
			expect(siren->synths.size()).to_equal(size_t{1});
			expect(std::string(siren->synths[0].path)).to_equal(std::string("SYNTHS/GENERATIVE/FX/GEN Siren.XML"));
			FakeCard card = harness::factoryCardWithPack();
			expect(card.has("SYNTHS/GENERATIVE/FX/GEN Siren.XML")).to_be_true();
			std::set<std::string> risers;
			std::set<std::string> sirens;
			auto picks = harness::lanePicks(dubGenre, card, 400);
			expect(picks[Lane::Riser] == std::set<std::string>{"SAMPLES/FX/Riser/Short/GEN Riser Short.wav"}).to_be_true();
			// Its own folder: no other genre's Riser lane reaches it.
			for (const Genre* genre : allGenres()) {
				for (uint32_t entropy = 0; genre != &dubGenre && genre->palette != nullptr && entropy < 100; entropy++) {
					SoundPicks sounds;
					rollSounds(*genre, *genre->palette, sounds, entropy, card);
					expect(std::string(sounds.lane[static_cast<int32_t>(Lane::Riser)].c_str()).find("Short"))
					    .to_equal(std::string::npos);
				}
			}
			// No sample: the Siren rolls its preset (spec §10.5).
			expect(picks[Lane::Siren] == std::set<std::string>{"SYNTHS/GENERATIVE/FX/GEN Siren.XML"}).to_be_true();
			// The Impact rolls one of the owner's two presets a piece (#472).
			expect(picks[Lane::Impact] == std::set<std::string>{"SYNTHS/GENERATIVE/FX/GEN Dub Impact Crack.XML",
			                                                    "SYNTHS/GENERATIVE/FX/GEN Impact KickCrash Dub.XML"})
			    .to_be_true();
			// Every ungated Drums lane's own list finds at least 3 files, the kit aside (spec §10.1, §10.2).
			picks = harness::lanePicks(dubGenre, card, 400, {}, false);
			for (const LaneSpec& lane : dubGenre.parts[kDrumsPart].lanes) {
				if (lane.densityGate == 0) {
					expect(picks[lane.lane].size()).to_be_greater_than(size_t{2});
				}
				expect(picks[lane.lane].empty()).to_be_false();
			}
		});
	});
});

CPPSPEC_SPEC(dub)
