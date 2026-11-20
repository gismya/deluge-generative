// Psytrance (spec §4.5, #127): a genre of its own, six parts (Drums, Bass, Drone, Lead, Arp, FX), a four-on-the-floor
// kick under a rolling bass that Avoids it, an empty Harmony slot, 16-bar phrases and long Builds into hard Drops. Run
// straight through the core.

#include "core_walk.h"
#include "fake_card.h"
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

const Genre& psyGenre = genres::psytrance;

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kDronePart = 2;
constexpr int32_t kLeadPart = 3;
constexpr int32_t kArpPart = 4;
constexpr int32_t kFxPart = 5;

constexpr int32_t kPhraseSteps = 16 * kStepsPerBar;

const LaneSpec& laneOf(Lane lane) {
	return harness::laneOf(psyGenre, kDrumsPart, lane);
}

bool onBeat(int32_t tick) {
	return (tick / kTicksPerStep) % kStepsPerBeat == 0;
}

/// The step a note-on falls in (floor: Lean 0 everywhere keeps Psytrance's notes on the grid).
int32_t stepAt(const Event& e) {
	return e.tick / kTicksPerStep;
}

/// Each Build's last step (the step before a Drop starts) in a walk.
std::vector<int32_t> buildEnds(const Walked& walked) {
	std::vector<int32_t> out;
	for (size_t n = 1; n < walked.steps.size(); n++) {
		if (walked.steps[n].drop && walked.steps[n - 1].section == SectionKind::Build) {
			out.push_back(static_cast<int32_t>(n) - 1);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe psytrance("Psytrance", $ {
	it("is a genre of its own with six parts: Drums, Bass, Drone, Lead, Arp and FX (spec §4.5)", _ {
		expect(std::string(psyGenre.name)).to_equal(std::string("psytrance"));
		expect(std::string(psyGenre.oledName)).to_equal(std::string("PSYTRANCE"));
		expect(psyGenre.parent == nullptr).to_be_true();
		expect(psyGenre.version).to_equal(1);
		expect(findGenre("psytrance") == &psyGenre).to_be_true();
		expect(partCount(psyGenre)).to_equal(6);
		const std::vector<std::tuple<Role, Flavour, std::string>> roster = {
		    {Role::Drums, Flavour::None, "DRUMS"},  {Role::Bass, Flavour::Loop, "BASS"},
		    {Role::Pad, Flavour::Sustain, "DRONE"}, {Role::Lead, Flavour::Loop, "LEAD"},
		    {Role::Lead, Flavour::Arp, "ARP"},      {Role::FX, Flavour::None, "FX"}};
		for (size_t p = 0; p < roster.size(); p++) {
			const PartSpec& part = psyGenre.parts[p];
			expect(part.role == std::get<0>(roster[p])).to_be_true();
			expect(part.flavour == std::get<1>(roster[p])).to_be_true();
			expect(std::string(part.name)).to_equal(std::get<2>(roster[p]));
		}
		expect(rosterFits(psyGenre)).to_be_true();
	});

	it("plays 138–146 BPM in Minor, Phrygian or Harmonic Minor, straight, in 16-bar phrases, with an empty Harmony slot", _ {
		expect(psyGenre.facts.tempoLowBpm).to_equal(138);
		expect(psyGenre.facts.tempoHighBpm).to_equal(146);
		std::set<Scale> scales(psyGenre.facts.scales.begin(), psyGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor}).to_be_true();
		expect(psyGenre.facts.swing).to_equal(0);
		expect(psyGenre.phraseBars).to_equal(16);
		expect(psyGenre.harmony.graph == nullptr).to_be_true();
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(psyGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("takes its Arc from the brief: Groove 3–6, Break 1 keeping Drone, Arp, Lead and Closed hat, Builds 16 ×2 and 32 ×1", _ {
		const ArcSpec& arc = psyGenre.arc;
		expect(arc.introPhrases).to_equal(1);
		expect(arc.outroPhrases).to_equal(1);
		expect(arc.groovePhrases.low).to_equal(3);
		expect(arc.groovePhrases.high).to_equal(6);
		expect(arc.breakPhrases.low).to_equal(1);
		expect(arc.breakPhrases.high).to_equal(1);
		expect(arc.grooveToBuild).to_equal(0);
		expect(arc.buildBars.size()).to_equal(size_t{2});
		expect(arc.buildBars[0].bars).to_equal(16);
		expect(arc.buildBars[0].weight).to_equal(2);
		expect(arc.buildBars[1].bars).to_equal(32);
		expect(arc.buildBars[1].weight).to_equal(1);
		expect(arc.breakKeeps == PartsAndLanes{.parts = partBit(kDronePart) | partBit(kArpPart) | partBit(kLeadPart),
		                                       .lanes = laneBit(Lane::ClosedHat)})
		    .to_be_true();
		// Everything but the Lead, which comes in on the Drop.
		expect(arc.buildKeeps == kEverything.without({.parts = partBit(kLeadPart)})).to_be_true();
		expect(arc.buildCut == PartsAndLanes{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)}).to_be_true();
		expect(arc.buildCutBars).to_equal(2);
		expect(arc.gap == GapLength::Beat).to_be_true();
		expect(arc.roll.lane == Lane::Snare).to_be_true();
		expect(arc.roll.top == RollStage::ThirtySecond).to_be_true();
		expect(arc.dropLiftEnergy).to_equal(macroPoints(8));
		expect(arc.dropDecayPhrases).to_equal(2);
		expect(arc.accentLane == Lane::Crash).to_be_true();
		expect(arc.accentChance).to_equal(0);
		expect(arc.transitionEntry == TransitionEntry::Drop).to_be_true();
	});

	it("stops its 2-phrase Drop decay at the next Section when a Groove after a Drop lasts one phrase (Motion 50)", _ {
		// Psytrance's decay runs past its shortest Groove at Motion 50 (spec §9.3's named exception, #349): the lift
		// is still up when that Groove ends, and the Break's offsets take over on its first step.
		Macros centre;
		centre.set(Macro::Motion, kMacroFull);
		int32_t cut = 0;
		for (uint32_t seed = 1; seed <= 40 && cut == 0; seed++) {
			Arc arc;
			arc.start(psyGenre);
			int32_t lastDensity = 0;
			int32_t dropStart = -1;
			for (int32_t step = 0; step < 400 * kStepsPerBar; step++) {
				if (arc.enterStep(psyGenre, seed, step, centre)) {
					if (dropStart >= 0 && step - dropStart == kPhraseSteps) {
						// Half the 2-phrase decay is left when the 1-phrase Groove ends.
						expect(lastDensity).to_be_greater_than(0);
						expect(arc.offset(psyGenre, Macro::Density, centre[Macro::Density])).to_be_less_than(1);
						cut++;
						break;
					}
					dropStart = arc.dropStartsHere() ? step : -1;
				}
				lastDensity = arc.offset(psyGenre, Macro::Density, centre[Macro::Density]);
			}
		}
		expect(cut).to_be_greater_than(0);
	});

	it("fills with a Rest on Kick and Bass (1–4 beats, weight 3, the held fill) and a Roll on Snare (1–2 beats, weight 2)", _ {
		int32_t restWeight = 0;
		int32_t rollWeight = 0;
		std::set<int32_t> restBeats;
		std::set<int32_t> rollBeats;
		for (const FillEntry& fill : psyGenre.fills) {
			if (fill.kind == FillKind::Rest) {
				expect(fill.mask == PartsAndLanes{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)}).to_be_true();
				restWeight += fill.weight;
				restBeats.insert(fill.beats);
			}
			else {
				expect(fill.kind == FillKind::Roll).to_be_true();
				expect(fill.mask == PartsAndLanes{.lanes = laneBit(Lane::Snare)}).to_be_true();
				rollWeight += fill.weight;
				rollBeats.insert(fill.beats);
			}
		}
		expect(restWeight).to_equal(3);
		expect(rollWeight).to_equal(2);
		expect(restBeats == std::set<int32_t>{1, 2, 4}).to_be_true();
		expect(rollBeats == std::set<int32_t>{1, 2}).to_be_true();
		expect(psyGenre.fills[psyGenre.heldFill].kind == FillKind::Rest).to_be_true();
		expect(psyGenre.fills[psyGenre.heldFill].beats).to_equal(2);
		expect(psyGenre.fillChance).to_equal(400);
	});

	context("its kick and rolling bass (spec §4.5, §5.5)", _ {
		it("plays the kick four on the floor at every Complexity, with no pickup off the beat", _ {
			const LaneSpec& kick = laneOf(Lane::Kick);
			for (const Pattern& pattern : kick.pool) {
				for (int32_t i = 0; i < pattern.length; i++) {
					expect(pattern.importance[i] == kAnchor).to_equal(i % kStepsPerBeat == 0);
				}
			}
			expect(kick.fillPool.empty()).to_be_true();
		});

		it("plays the open hat every other bar, the hit in the second (#350)", _ {
			const LaneSpec& openHat = laneOf(Lane::OpenHat);
			for (const Pattern& pattern : openHat.pool) {
				expect(pattern.length).to_equal(2 * kStepsPerBar);
				for (int32_t i = 0; i < kStepsPerBar; i++) {
					expect(pattern.importance[i]).to_equal(0);
				}
			}
			int32_t bars = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 96 * kStepsPerBar);
				std::set<int32_t> hatBars;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDrumsPart && e.lane == Lane::OpenHat) {
						hatBars.insert(stepAt(e) / kStepsPerBar);
					}
				}
				for (int32_t bar : hatBars) {
					expect(bar % 2).to_equal(1);
				}
				bars += static_cast<int32_t>(hatBars.size());
			}
			expect(bars).to_be_greater_than(100);
		});

		it("gives the Bass Avoid Kick at strength 100, sensitivity 0, onsets on every 16th at even levels, the & anchored, a "
		   "20-tick gate, Lean 0, no slides",
		   _ {
			const PartSpec& bass = psyGenre.parts[kBassPart];
			expect(bass.rules.size()).to_equal(size_t{1});
			for (const InterplayRule& rule : bass.rules) {
				expect(rule.kind == RuleKind::Avoid).to_be_true();
				expect(rule.target.part).to_equal(kDrumsPart);
				expect(rule.target.lane == Lane::Kick).to_be_true();
				expect(rule.strength).to_equal(100);
				expect(rule.sensitivity).to_equal(0);
			}
			for (const Pattern& pattern : bass.pool) {
				for (int32_t i = 0; i < pattern.length; i++) {
					expect(pattern.importance[i]).to_be_greater_than(0);
					expect(pattern.importance[i] == kAnchor).to_equal(i % kStepsPerBeat == 2);
					// An even roll (#353): no ghost levels.
					expect(pattern.level[i] == VelocityLevel::Ghost).to_be_false();
				}
			}
			// Each 16th held 20 ticks (#133's listening session, round 1).
			expect(bass.loop.gateTicks).to_equal(20);
			for (int8_t lean : bass.expression.lean) {
				expect(lean).to_equal(0);
			}
			expect(bass.loop.slide.atZero).to_equal(0);
			expect(bass.loop.slide.atFull).to_equal(0);
			expect(bass.loop.degrees.size()).to_equal(size_t{1});
			expect(bass.loop.degrees[0].value).to_equal(0);
		});

		it("never starts or sounds a Bass note on a kick step, over many pieces at every macro corner", _ {
			int32_t bassNotes = 0;
			int32_t fullGates = 0;
			int32_t offBeats[kStepsPerBeat] = {};
			auto check = [&](const Recipe& recipe, uint32_t entropy) {
				const Walked walked = walk(recipe, entropy, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part == kBassPart) {
						bassNotes++;
						expect(onBeat(e.tick)).to_be_false();
						offBeats[stepAt(e) % kStepsPerBeat]++;
					}
				}
				for (const Held& note : heldNotes(walked.notes, kBassPart)) {
					// No beat line falls inside a Bass note: each ends by the next beat's line.
					const int32_t nextBeat =
					    (note.on / (kStepsPerBeat * kTicksPerStep) + 1) * kStepsPerBeat * kTicksPerStep;
					if (note.off != INT32_MAX) {
						expect(note.off).to_be_less_than(nextBeat + 1);
						expect(note.off - note.on).to_be_less_than(21);
						fullGates += note.off - note.on == 20 ? 1 : 0;
					}
				}
			};
			for (Scale scale : psyGenre.facts.scales) {
				for (int32_t complexity : {0, 25, 50}) {
					for (int32_t density : {0, 25, 50}) {
						const uint32_t entropy = static_cast<uint32_t>(complexity + density + 1);
						check(harness::movingRecipe(psyGenre, scale, entropy, density, complexity), entropy);
					}
				}
			}
			// Every corner of the four macros.
			for (int32_t corner = 0; corner < 16; corner++) {
				const uint32_t entropy = static_cast<uint32_t>(corner + 100);
				Recipe recipe = harness::movingRecipe(psyGenre, Scale::Phrygian, entropy, (corner & 1) ? 50 : 0, (corner & 2) ? 50 : 0);
				recipe.macros.set(Macro::Energy, macroPoints((corner & 4) ? 50 : 0));
				recipe.macros.set(Macro::Motion, macroPoints((corner & 8) ? 50 : 0));
				check(recipe, entropy);
			}
			expect(bassNotes).to_be_greater_than(1000);
			expect(fullGates).to_be_greater_than(1000);
			// It rolls on the e, the & and the a.
			expect(offBeats[0]).to_equal(0);
			for (int32_t i = 1; i < kStepsPerBeat; i++) {
				expect(offBeats[i]).to_be_greater_than(100);
			}
		});

		it("plays the Bass's root and the octave over it now and then, nothing else", _ {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				Recipe recipe = harness::movingRecipe(psyGenre, Scale::Phrygian, seed, 25, 50);
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				std::set<int32_t> pitches;
				for (const Event& e : walked.noteOns) {
					if (e.part == kBassPart) {
						pitches.insert(e.pitch);
						expect(pitchClass(e.pitch)).to_equal(pitchClass(recipe.keyRoot));
					}
				}
				expect(pitches.size()).to_be_greater_than(size_t{1});
				expect(pitches.size()).to_be_less_than(size_t{3});
				expect(*pitches.rbegin() - *pitches.begin()).to_equal(12);
			}
		});
	});

	context("its pitched parts (spec §4.5)", _ {
		it("puts the Drone's tonic an octave under the Arp's and the Lead's two octaves over it, in every key", _ {
			const Register arp = psyGenre.parts[kArpPart].registerOrDefault();
			const Register drone = psyGenre.parts[kDronePart].registerOrDefault();
			const Register lead = psyGenre.parts[kLeadPart].registerOrDefault();
			for (uint8_t key = 0; key < 12; key++) {
				expect(tonicNote(drone, key)).to_equal(tonicNote(arp, key) - 12);
				expect(tonicNote(lead, key)).to_equal(tonicNote(arp, key) + 24);
			}
		});

		it("holds a pedal Drone on the key's root and fifth", _ {
			const PartSpec& drone = psyGenre.parts[kDronePart];
			expect(drone.sustain.pedal).to_be_true();
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = harness::movingRecipe(psyGenre, Scale::Minor, seed);
				const Walked walked = walk(recipe, seed, 40 * kStepsPerBar);
				int32_t notes = 0;
				for (const Event& e : walked.noteOns) {
					if (e.part == kDronePart) {
						notes++;
						const int32_t interval = pitchClass(e.pitch - recipe.keyRoot);
						expect(interval == 0 || interval == 7).to_be_true();
						expect(e.pitch).to_be_less_than(62);
					}
				}
				expect(notes).to_be_greater_than(0);
			}
		});

		it("gates the Lead at 30 with a 16-bar Sweep, so it comes in on Drops, and leaves the Arp ungated with one", _ {
			const PartSpec& lead = psyGenre.parts[kLeadPart];
			const PartSpec& arp = psyGenre.parts[kArpPart];
			expect(lead.densityGate).to_equal(macroPoints(30));
			expect(lead.drift.sweepBars).to_equal(16);
			expect(arp.densityGate).to_equal(0);
			expect(arp.drift.sweepBars).to_equal(16);
			int32_t onDrops = 0;
			int32_t dropPhrases = 0;
			int32_t builds = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 160 * kStepsPerBar);
				const int32_t steps = static_cast<int32_t>(walked.steps.size());
				// The first phrase after each Drop, counted from the step where it starts (as buildEnds() finds it).
				std::vector<bool> dropPhrase(walked.steps.size(), false);
				for (int32_t end : buildEnds(walked)) {
					for (int32_t n = end + 1; n < std::min(steps, end + 1 + kPhraseSteps); n++) {
						dropPhrase[n] = true;
					}
				}
				for (const Walked::Step& step : walked.steps) {
					builds += step.section == SectionKind::Build ? 1 : 0;
				}
				std::set<int32_t> leadBars;
				for (const Event& e : walked.noteOns) {
					if (e.part != kLeadPart) {
						continue;
					}
					const int32_t step = std::min<int32_t>(stepAt(e), steps - 1);
					// Never in the Intro at the default Density, and never in a Build, though its Density ramp
					// passes the gate.
					expect(walked.steps[step].section == SectionKind::Intro).to_be_false();
					expect(walked.steps[step].section == SectionKind::Build).to_be_false();
					onDrops += dropPhrase[step] ? 1 : 0;
					leadBars.insert(step / kStepsPerBar);
				}
				// The Lead plays every bar of each Drop's first phrase (#349: its lift decays over two phrases).
				for (int32_t end : buildEnds(walked)) {
					if (end + 1 + kPhraseSteps > steps) {
						continue;
					}
					dropPhrases++;
					for (int32_t bar = (end + 1) / kStepsPerBar; bar < (end + 1 + kPhraseSteps) / kStepsPerBar; bar++) {
						expect(leadBars.count(bar)).to_equal(size_t{1});
					}
				}
			}
			expect(builds).to_be_greater_than(0);
			expect(onDrops).to_be_greater_than(0);
			expect(dropPhrases).to_be_greater_than(0);
		});

		it("walks the Arp Up or Up-down over 1–2 octaves on 16ths, an onset on every 16th in every entry", _ {
			const PartSpec& arp = psyGenre.parts[kArpPart];
			expect(arp.arp.walks.size()).to_equal(arp.pool.size());
			for (const ArpWalk& walk : arp.arp.walks) {
				expect(walk.order == ArpOrder::Up || walk.order == ArpOrder::UpDown).to_be_true();
				expect(walk.octaves >= 1 && walk.octaves <= 2).to_be_true();
			}
			for (const Pattern& pattern : arp.pool) {
				expect(pattern.length).to_equal(kStepsPerBar);
				// Straight 16ths, no gallop skipping the & (#354).
				for (int32_t i = 0; i < pattern.length; i++) {
					expect(pattern.importance[i]).to_be_greater_than(0);
				}
			}
		});
	});

	context("its Arc (spec §4.5, §9)", _ {
		it("recedes to the Drone, the Arp, the Lead and the closed hat in a Break, and plays the Arp and the hat there", _ {
			std::map<int32_t, int32_t> kept;
			int32_t hats = 0;
			int32_t breakSteps = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 160 * kStepsPerBar);
				for (const Walked::Step& step : walked.steps) {
					breakSteps += step.section == SectionKind::Break ? 1 : 0;
				}
				for (const Event& e : walked.noteOns) {
					const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
					if (walked.steps[step].section != SectionKind::Break) {
						continue;
					}
					if (e.part == kDrumsPart) {
						expect(e.lane == Lane::ClosedHat).to_be_true();
						hats++;
						continue;
					}
					expect(e.part == kDronePart || e.part == kLeadPart || e.part == kArpPart || e.part == kFxPart)
					    .to_be_true();
					kept[e.part]++;
				}
			}
			expect(breakSteps).to_be_greater_than(kPhraseSteps);
			expect(kept[kArpPart]).to_be_greater_than(0);
			expect(hats).to_be_greater_than(0);
		});

		it("cuts the Kick and Bass for a Build's last 2 bars, rolls the Snare, leaves a beat's Gap, and drops with a Crash", _ {
			int32_t builds = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 160 * kStepsPerBar);
				for (int32_t end : buildEnds(walked)) {
					builds++;
					const int32_t cutFrom = stepTick(end + 1 - 2 * kStepsPerBar);
					const int32_t gapFrom = stepTick(end + 1 - kStepsPerBeat);
					const int32_t drop = stepTick(end + 1);
					int32_t snares = 0;
					int32_t crash = 0;
					for (const Event& e : walked.noteOns) {
						if (e.tick >= cutFrom && e.tick < drop) {
							expect(e.part == kBassPart).to_be_false();
							expect(e.part == kDrumsPart && e.lane == Lane::Kick).to_be_false();
							snares += e.part == kDrumsPart && e.lane == Lane::Snare && e.tick < gapFrom ? 1 : 0;
						}
						if (e.tick >= gapFrom && e.tick < drop) {
							// The Gap: only an FX lane (the Riser) may still start.
							expect(e.part).to_equal(kFxPart);
						}
						crash += e.tick == drop && e.part == kDrumsPart && e.lane == Lane::Crash ? 1 : 0;
					}
					// 32nds by the cut: more than one hit a step.
					expect(snares).to_be_greater_than(2 * kStepsPerBar - kStepsPerBeat);
					expect(crash).to_equal(1);
				}
			}
			expect(builds).to_be_greater_than(2);
		});

		it("plays non-silent through every Section kind, the Build and the Drop included", _ {
			const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, 3), 3, 160 * kStepsPerBar);
			std::map<SectionKind, int32_t> notes;
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
				notes[walked.steps[step].section]++;
			}
			for (SectionKind kind : {SectionKind::Intro, SectionKind::Groove, SectionKind::Break, SectionKind::Build}) {
				expect(notes[kind]).to_be_greater_than(0);
			}
		});
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		const harness::Lost lost = harness::lostAtEveryCorner(psyGenre, psyGenre.facts.scales);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its palette (spec §10.1, §18.2, §18.3)", _ {
		it("gives the Bass GEN Psy Bass alone, the Drone and the Arp 2 presets and the Lead 3, each on the factory card or "
		   "in the pack (the rest dropped by ear in #133)",
		   _ {
			   FakeCard card = harness::factoryCardWithPack();
			   auto presets = [](int32_t part) { return psyGenre.palette->partEntries(psyGenre.parts[part]).size(); };
			   expect(presets(kBassPart)).to_equal(size_t{1});
			   expect(presets(kDronePart)).to_equal(size_t{2});
			   expect(presets(kLeadPart)).to_equal(size_t{3});
			   expect(presets(kArpPart)).to_equal(size_t{2});
			   expect(harness::partPresetsMissing(psyGenre, card).empty()).to_be_true();
			   expect(harness::mainPick(psyGenre, kBassPart))
			       .to_equal(std::string("SYNTHS/GENERATIVE/GEN Psy Bass.XML x3"));
		   });

		it("finds at least 3 files for every ungated lane, the Psy set's among them, and pack risers alone on the Riser", _ {
			FakeCard card = harness::factoryCardWithPack();
			constexpr const char* kPsyFolder = "SAMPLES/Artists/Dean Daughters/Psy/";
			auto picks = harness::lanePicks(psyGenre, card, 800, [&](const SoundPicks& sounds) {
				// The hats are always one set: the Psy set's pair.
				expect(std::string(sounds.lane[static_cast<int32_t>(Lane::ClosedHat)].c_str()).rfind(kPsyFolder, 0))
				    .to_equal(size_t{0});
				expect(std::string(sounds.lane[static_cast<int32_t>(Lane::OpenHat)].c_str()).rfind(kPsyFolder, 0))
				    .to_equal(size_t{0});
			});
			for (const PartSpec& part : psyGenre.parts) {
				for (const LaneSpec& lane : part.lanes) {
					if (lane.densityGate == 0 && lane.lane != Lane::Riser && lane.lane != Lane::Fall
					    && lane.lane != Lane::Impact) {
						expect(picks[lane.lane].size()).to_be_greater_than(size_t{2});
					}
					expect(picks[lane.lane].empty()).to_be_false();
				}
			}
			for (Lane lane : {Lane::Kick, Lane::Snare, Lane::Clap, Lane::ClosedHat, Lane::OpenHat}) {
				bool psy = false;
				for (const std::string& path : picks[lane]) {
					psy = psy || path.rfind(kPsyFolder, 0) == 0;
				}
				expect(psy).to_be_true();
			}
			for (const std::string& riser : picks[Lane::Riser]) {
				expect(riser.rfind("SAMPLES/FX/Riser/GEN ", 0)).to_equal(size_t{0});
			}
		});
	});
});

CPPSPEC_SPEC(psytrance)
