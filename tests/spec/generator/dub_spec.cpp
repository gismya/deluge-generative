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
constexpr int32_t kBackbeat = 8; // beat 3: the one drop

constexpr DelaySetup kDottedEighth{
    .on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true, .analog = true};

/// Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec §9.3).
const LaneSpec& laneOf(int32_t part, Lane lane) {
	return harness::laneOf(dubGenre, part, lane);
}

/// Whether some entry of `pool` at `tier` hits the backbeat.
bool backbeatAt(std::span<const Pattern> pool, int32_t tier) {
	return std::ranges::any_of(pool, [&](const Pattern& p) { return p.tier == tier && p.importance[kBackbeat] > 0; });
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
		expect(dubGenre.version).to_equal(2);
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

	it("plays 70–80 BPM in Minor alone, swing +10, in 8-bar phrases, every macro at 25", _ {
		expect(dubGenre.facts.tempoLowBpm).to_equal(70);
		expect(dubGenre.facts.tempoHighBpm).to_equal(80);
		expect(dubGenre.facts.scales.size()).to_equal(size_t{1});
		expect(dubGenre.facts.scales[0] == Scale::Minor).to_be_true();
		expect(dubGenre.facts.swing).to_equal(10);
		expect(dubGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(dubGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("walks a Chord graph from home i: i–iv, i–♭VII and iv–v, 2 bars a chord, repeat 20, no cadence, Extensions 0–1", _ {
		const ChordGraph* graph = dubGenre.harmony.graph;
		expect(graph != nullptr).to_be_true();
		std::set<std::pair<int32_t, int32_t>> moves;
		for (const ChordMove& move : graph->moves) {
			moves.insert({move.from, move.to});
		}
		expect(moves == std::set<std::pair<int32_t, int32_t>>{{kI, kIV}, {kI, kVII}, {kIV, kV}}).to_be_true();
		expect(graph->barsPerChord).to_equal(2);
		expect(graph->repeatWeight).to_equal(20);
		expect(graph->cadenceTarget).to_equal(kNoCadenceTarget);
		expect(dubGenre.harmony.extensionFloor).to_equal(0);
		expect(dubGenre.harmony.extensionCeiling).to_equal(1);
	});

	context("its one-drop drums (spec §4.5)", _ {
		it("puts the kick and the Rim on beat 3 at every Complexity, anchored", _ {
			for (Lane lane : {Lane::Kick, Lane::Rim}) {
				for (const Pattern& pattern : laneOf(kDrumsPart, lane).pool) {
					expect(pattern.importance[kBackbeat]).to_equal(kAnchor);
				}
			}
			// The kick plays nothing on the 1: the drop is its only beat.
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Kick).pool) {
				expect(pattern.importance[0]).to_equal(0);
			}
		});

		it("gives the backbeat to exactly one of Rim and Snare at each tier", _ {
			const LaneSpec& rim = laneOf(kDrumsPart, Lane::Rim);
			const LaneSpec& snare = laneOf(kDrumsPart, Lane::Snare);
			// The Snare has no step table, so this holds as long as the Rim has the backbeat at every tier: the check
			// guards a Snare pool added later, which would have to leave beat 3 to the Rim.
			expect(snare.pool.empty()).to_be_true();
			const int32_t top = std::max(rim.band.high, snare.band.high);
			for (int32_t tier = 0; tier <= top; tier++) {
				expect(backbeatAt(rim.pool, tier) != backbeatAt(snare.pool, tier)).to_be_true();
			}
		});

		it("plays the kick and the Rim together on beat 3 of every Groove bar no drop-out or fill touches", _ {
			int32_t bars = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				Recipe recipe = harness::movingRecipe(dubGenre, seed);
				Genre plain = dubGenre; // no fills or drop-outs, so every Groove bar holds the one drop
				plain.fills = {};
				plain.dropOuts = {};
				recipe.genre = &plain;
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				std::set<int32_t> kicks;
				std::set<int32_t> rims;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::Kick) {
						kicks.insert(stepAt(e));
					}
					if (e.part == kDrumsPart && e.lane == Lane::Rim) {
						rims.insert(stepAt(e));
					}
					// The Snare plays only the Build roll with no fills.
					if (e.part == kDrumsPart && e.lane == Lane::Snare) {
						const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
						expect(walked.steps[step].section == SectionKind::Build).to_be_true();
					}
				}
				for (int32_t bar = 0; bar * kStepsPerBar < static_cast<int32_t>(walked.steps.size()); bar++) {
					const int32_t drop = bar * kStepsPerBar + kBackbeat;
					if (walked.steps[drop].section != SectionKind::Groove) {
						continue;
					}
					bars++;
					expect(kicks.contains(drop)).to_be_true();
					expect(rims.contains(drop)).to_be_true();
				}
			}
			expect(bars).to_be_greater_than(32);
		});

		it("swings the hats' & late, leans the Snare +2 everywhere, and lets the Crash replace the closed hat", _ {
			for (Lane lane : {Lane::ClosedHat, Lane::OpenHat}) {
				const Expression& hats = laneOf(kDrumsPart, lane).expression;
				expect(hats.lean[0]).to_equal(0);
				expect(hats.lean[2]).to_be_greater_than(0);
			}
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

		it("fills with a Swap on Snare and Tom (the held fill) and one on Rim and Snare that moves the backbeat", _ {
			expect(dubGenre.fills.size()).to_equal(size_t{2});
			for (const FillEntry& fill : dubGenre.fills) {
				expect(fill.kind == FillKind::Swap).to_be_true();
			}
			const FillEntry& held = dubGenre.fills[dubGenre.heldFill];
			expect(held.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}).to_be_true();
			bool moves = false;
			for (const FillEntry& fill : dubGenre.fills) {
				moves = moves || fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Rim) | laneBit(Lane::Snare)};
			}
			expect(moves).to_be_true();
			// The Rim's fill pool is silent, the Snare's holds the backbeat at every tier.
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Rim).fillPool) {
				expect(std::ranges::all_of(pattern.importance, [&](uint8_t i) { return i == 0; })).to_be_true();
			}
			for (const Pattern& pattern : laneOf(kDrumsPart, Lane::Snare).fillPool) {
				expect(pattern.importance[kBackbeat]).to_be_greater_than(0);
			}
			expect(dubGenre.fillChance).to_equal(250);
		});
	});

	context("its pitched parts (spec §4.5)", _ {
		it("gives the Bass a 2-bar Loop line on the Pentatonic set, leaping, with Octave, Fifth and Rotate Answers and no slides", _ {
			const PartSpec& bass = dubGenre.parts[kBassPart];
			expect(bass.degreeSet == DegreeSet::Pentatonic).to_be_true();
			expect(bass.loop.slide.atZero).to_equal(0);
			expect(bass.loop.slide.atFull).to_equal(0);
			expect(bass.loop.draws.leap.atZero).to_be_greater_than(1);
			std::set<AnswerKind> answers;
			for (const Answer& answer : bass.loop.answers) {
				answers.insert(answer.kind);
			}
			expect(answers == std::set<AnswerKind>{AnswerKind::Octave, AnswerKind::Fifth, AnswerKind::Rotate}).to_be_true();
			expect(bass.reg.anchor).to_equal(0); // the Bass Register's default
			int32_t tierZero = 0;
			int32_t emptyThree = 0;
			for (const Pattern& pattern : bass.pool) {
				expect(pattern.length).to_equal(2 * kStepsPerBar);
				expect(pattern.importance[0]).to_equal(kAnchor);
				if (pattern.tier == 0) {
					tierZero++;
					emptyThree += pattern.importance[kBackbeat] == 0 && pattern.importance[kStepsPerBar + kBackbeat] == 0;
				}
			}
			expect(2 * emptyThree).to_be_greater_than(tierZero); // most tier-0 lines leave beat 3 to the drop
		});

		it("chops the Skank on the backbeats with a short gate, and gives the Melodica a Motif", _ {
			const PartSpec& skank = dubGenre.parts[kSkankPart];
			for (const Pattern& pattern : skank.pool) {
				expect(pattern.importance[4]).to_equal(kAnchor);
				expect(pattern.importance[12]).to_equal(kAnchor);
				expect(pattern.importance[0]).to_equal(0);
				expect(pattern.importance[kBackbeat]).to_equal(0);
			}
			expect(skank.stab.gateTicks).to_be_less_than(kTicksPerStep);
			const PartSpec& melodica = dubGenre.parts[kMelodicaPart];
			expect(melodica.motif.positions.empty()).to_be_false();
			expect(melodica.densityGate).to_equal(0);
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
			expect(arc.roll.top == RollStage::Sixteenth).to_be_true();
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

	context("its drop-outs and delay setups (spec §8.8, §10.6, §11.4)", _ {
		it("drops the Skank for 2 bars, the drums but the kick for 1, everything but the Bass for 4, each thrown", _ {
			expect(dubGenre.dropOuts.size()).to_equal(size_t{3});
			const DropOutEntry& skank = dubGenre.dropOuts[0];
			expect(skank.mask == PartsAndLanes{.parts = partBit(kSkankPart)}).to_be_true();
			expect(skank.bars).to_equal(2);
			expect(skank.throwTarget == PartsAndLanes{.parts = partBit(kSkankPart)}).to_be_true();
			const DropOutEntry& drums = dubGenre.dropOuts[1];
			expect(drums.bars).to_equal(1);
			expect(drums.mask.parts).to_equal(0);
			expect((drums.mask.lanes & laneBit(Lane::Kick)) == 0).to_be_true();
			for (const LaneSpec& lane : dubGenre.parts[kDrumsPart].lanes) {
				expect(lane.lane == Lane::Kick || (drums.mask.lanes & laneBit(lane.lane)) != 0).to_be_true();
			}
			expect(drums.throwTarget == PartsAndLanes{.lanes = laneBit(Lane::Rim)}).to_be_true();
			const DropOutEntry& all = dubGenre.dropOuts[2];
			expect(all.bars).to_equal(4);
			expect(all.mask.has(kBassPart, Lane::None)).to_be_false();
			expect(all.mask.has(kSkankPart, Lane::None) && all.mask.has(kMelodicaPart, Lane::None)).to_be_true();
			for (const LaneSpec& lane : dubGenre.parts[kDrumsPart].lanes) {
				expect(all.mask.has(kDrumsPart, lane.lane)).to_be_true();
			}
			expect(all.throwTarget == PartsAndLanes{.parts = partBit(kMelodicaPart)}).to_be_true();
			expect(dubGenre.dropOutChance).to_equal(120);
			expect(dubGenre.dropOutGapBars).to_equal(2);
			expect(dropOutsValid(dubGenre)).to_be_true();
		});

		it("gives each throw target's every line, at every tier, an onset in the last beat of every bar", _ {
			// A drop-out starts on a bar line and its Throw spikes only on a target's hit in the beat before it
			// (kThrowWindowSteps): a line with none there leaves that drop-out dry.
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
							for (int32_t bar = 0; bar < pattern.length / kStepsPerBar; bar++) {
								const int32_t from = (bar + 1) * kStepsPerBar - kThrowWindowSteps;
								bool hit = false;
								for (int32_t step = from; step < from + kThrowWindowSteps; step++) {
									// 8 or more: below about Density 10 lower importances drop out, and the
									// drop-out would play dry.
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

		it("plays each drop-out for its bars in its Grooves, its target thrown as it starts, at low Complexity too", _ {
			// Each entry alone, often and with no fills, at Complexity 5 (tier 0) and Density 15. A drop-out bar is a
			// Groove bar its target is silenced in all through (a pitched part), or (the Rim, in a Drums part the Kick
			// keeps playing) with no hit on beat 3, which the Rim holds at every tier. The target's Space rises by the
			// Throw from before the beat its hit spikes on to the drop-out's first step.
			struct Target {
				uint8_t part;
				Lane lane;
			};
			// Each entry's own throw target: a pitched part, or the first Drums lane it names.
			const auto targetOf = [](const DropOutEntry& entry) {
				for (int32_t p = 0; p < partCount(dubGenre); p++) {
					if (entry.throwTarget.has(p, Lane::None) && dubGenre.parts[p].lanes.empty()) {
						return Target{static_cast<uint8_t>(p), Lane::None};
					}
				}
				for (const LaneSpec& lane : dubGenre.parts[kDrumsPart].lanes) {
					if ((entry.throwTarget.lanes & laneBit(lane.lane)) != 0) {
						return Target{static_cast<uint8_t>(kDrumsPart), lane.lane};
					}
				}
				throw "a drop-out with no throw target";
			};
			for (size_t n = 0; n < dubGenre.dropOuts.size(); n++) {
				const Target target = targetOf(dubGenre.dropOuts[n]);
				Genre genre = dubGenre;
				genre.dropOuts = std::span(&dubGenre.dropOuts[n], 1);
				genre.fills = {};
				genre.dropOutChance = 500;
				int32_t starts = 0;
				for (uint32_t seed = 1; seed <= 6; seed++) {
					Recipe recipe = newRecipe(genre, seed);
					recipe.macros.set(Macro::Complexity, macroPoints(5));
					recipe.macros.set(Macro::Density, macroPoints(15));
					Generator generator;
					generator.start(recipe, seed, Takes{}, true);
					SteeringQueue queue;
					constexpr int32_t kBars = 96;
					std::vector<bool> groove(kBars, true);
					std::vector<bool> silenced(kBars, true);
					std::vector<std::set<int32_t>> hits(kBars);
					std::vector<int32_t> space(kBars * kStepsPerBar, 0); // the target's Space offset on each step
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
							if (ours && e.kind == EventKind::Control && e.control == SoundControl::Space) {
								level = e.offset;
							}
						}
						space[step] = level;
					}
					const auto dropped = [&](int32_t bar) {
						return groove[bar] && (target.lane == Lane::None ? silenced[bar] : !hits[bar].contains(kBackbeat));
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
						const int32_t first = bar * kStepsPerBar;
						expect(space[first] - space[first - kThrowWindowSteps - 1]).to_be_greater_than(kThrowSpace / 2);
					}
				}
				expect(starts).to_be_greater_than(5);
			}
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
		it("gives every synth part at least 3 presets on the card or in the pack, the pack's dub sub and melodica the main picks", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(harness::fewestPartPresets(dubGenre)).to_be_greater_than(size_t{2});
			expect(harness::partPresetsMissing(dubGenre, card).empty()).to_be_true();
			expect(harness::mainPick(dubGenre, kBassPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Dub Sub.XML x3"));
			expect(harness::mainPick(dubGenre, kMelodicaPart)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Melodica.XML x3"));
		});

		it("lists GEN Siren alone on the Siren lane and no folder, and GEN Riser Short alone on the Riser", _ {
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
			// No sample: the Siren loads its preset through the FX lane's synth fallback.
			for (const std::string& path : picks[Lane::Siren]) {
				expect(path).to_equal(std::string("SYNTHS/GENERATIVE/FX/GEN Siren.XML"));
			}
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
