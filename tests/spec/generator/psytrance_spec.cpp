// Psytrance (spec §4.5, #127): a genre of its own, six parts (Drums, Bass, Drone, Lead, Arp, FX), a four-on-the-floor
// kick under a rolling bass that Avoids it, an empty Harmony slot, 16-bar phrases and long Builds into hard Drops. Run
// straight through the core.

#include "core_walk.h"
#include "fake_card.h"
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

const Genre& psyGenre = genres::psytrance;

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kDronePart = 2;
constexpr int32_t kLeadPart = 3;
constexpr int32_t kArpPart = 4;
constexpr int32_t kFxPart = 5;

constexpr int32_t kPhraseSteps = 16 * kStepsPerBar;

/// Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec §9.3: at Motion 50 a Groove
/// lasts a third of its base).
Recipe movingRecipe(Scale scale, uint32_t entropy, int32_t density = 25, int32_t complexity = 25) {
	Recipe recipe = newRecipe(psyGenre, entropy);
	recipe.keyRoot = static_cast<uint8_t>(entropy % 12);
	recipe.scale = scale;
	recipe.macros.set(Macro::Motion, macroPoints(50));
	recipe.macros.set(Macro::Density, macroPoints(density));
	recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	return recipe;
}

const LaneSpec& laneOf(Lane lane) {
	for (const LaneSpec& spec : psyGenre.parts[kDrumsPart].lanes) {
		if (spec.lane == lane) {
			return spec;
		}
	}
	throw "no such lane";
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
		expect(arc.dropDecayPhrases).to_equal(1);
		expect(arc.accentLane == Lane::Crash).to_be_true();
		expect(arc.accentChance).to_equal(0);
		expect(arc.transitionEntry == TransitionEntry::Drop).to_be_true();
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

		it("gives the Bass Avoid Kick at strength 100, sensitivity 0, onsets on every 16th, the & anchored, Lean 0, no slides", _ {
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
				}
			}
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
					}
				}
			};
			for (Scale scale : psyGenre.facts.scales) {
				for (int32_t complexity : {0, 25, 50}) {
					for (int32_t density : {0, 25, 50}) {
						const uint32_t entropy = static_cast<uint32_t>(complexity + density + 1);
						check(movingRecipe(scale, entropy, density, complexity), entropy);
					}
				}
			}
			// Every corner of the four macros.
			for (int32_t corner = 0; corner < 16; corner++) {
				const uint32_t entropy = static_cast<uint32_t>(corner + 100);
				Recipe recipe = movingRecipe(Scale::Phrygian, entropy, (corner & 1) ? 50 : 0, (corner & 2) ? 50 : 0);
				recipe.macros.set(Macro::Energy, macroPoints((corner & 4) ? 50 : 0));
				recipe.macros.set(Macro::Motion, macroPoints((corner & 8) ? 50 : 0));
				check(recipe, entropy);
			}
			expect(bassNotes).to_be_greater_than(1000);
			// It rolls on the e, the & and the a.
			expect(offBeats[0]).to_equal(0);
			for (int32_t i = 1; i < kStepsPerBeat; i++) {
				expect(offBeats[i]).to_be_greater_than(100);
			}
		});

		it("plays the Bass's root and the octave over it now and then, nothing else", _ {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				Recipe recipe = movingRecipe(Scale::Phrygian, seed, 25, 50);
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
				const Recipe recipe = movingRecipe(Scale::Minor, seed);
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
			int32_t builds = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(movingRecipe(Scale::Minor, seed), seed, 160 * kStepsPerBar);
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
				}
			}
			expect(builds).to_be_greater_than(0);
			expect(onDrops).to_be_greater_than(0);
		});

		it("walks the Arp Up or Up-down over 1–2 octaves on 16ths", _ {
			const PartSpec& arp = psyGenre.parts[kArpPart];
			expect(arp.arp.walks.size()).to_equal(arp.pool.size());
			for (const ArpWalk& walk : arp.arp.walks) {
				expect(walk.order == ArpOrder::Up || walk.order == ArpOrder::UpDown).to_be_true();
				expect(walk.octaves >= 1 && walk.octaves <= 2).to_be_true();
			}
			for (const Pattern& pattern : arp.pool) {
				expect(pattern.length).to_equal(kStepsPerBar);
			}
		});
	});

	context("its Arc (spec §4.5, §9)", _ {
		it("recedes to the Drone, the Arp, the Lead and the closed hat in a Break, and plays the Arp and the hat there", _ {
			std::map<int32_t, int32_t> kept;
			int32_t hats = 0;
			int32_t breakSteps = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(movingRecipe(Scale::Minor, seed), seed, 160 * kStepsPerBar);
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
				const Walked walked = walk(movingRecipe(Scale::Minor, seed), seed, 160 * kStepsPerBar);
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
			const Walked walked = walk(movingRecipe(Scale::Minor, 3), 3, 160 * kStepsPerBar);
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
		for (Scale scale : psyGenre.facts.scales) {
			for (int32_t complexity : {0, 25, 50}) {
				for (int32_t density : {0, 25, 50}) {
					const uint32_t entropy = static_cast<uint32_t>(complexity + density + 1);
					const Walked walked =
					    walk(movingRecipe(scale, entropy, density, complexity), 2, 128 * kStepsPerBar);
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
				}
			}
		}
	});

	context("its palette (spec §10.1, §18.2, §18.3)", _ {
		it("gives every synth part at least 3 presets, each on the factory card or in the pack, GEN Psy Bass the Bass's main pick", _ {
			FakeCard card = harness::factoryCardWithPack();
			for (int32_t p = kBassPart; p <= kArpPart; p++) {
				auto entries = psyGenre.palette->partEntries(psyGenre.parts[p]);
				expect(entries.size()).to_be_greater_than(size_t{2});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
				}
			}
			const auto bass = psyGenre.palette->partEntries(psyGenre.parts[kBassPart]);
			expect(std::string(bass[0].path)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Psy Bass.XML"));
			expect(bass[0].weight).to_equal(3);
		});

		it("finds at least 3 files for every ungated lane, the Psy set's among them, and pack risers alone on the Riser", _ {
			FakeCard card = harness::factoryCardWithPack();
			std::map<Lane, std::set<std::string>> picks;
			constexpr const char* kPsyFolder = "SAMPLES/Artists/Dean Daughters/Psy/";
			for (uint32_t entropy = 0; entropy < 800; entropy++) {
				SoundPicks sounds;
				rollSounds(psyGenre, *psyGenre.palette, sounds, entropy, card);
				// The hats are always one set: the Psy set's pair.
				expect(std::string(sounds.lane[static_cast<int32_t>(Lane::ClosedHat)].c_str()).rfind(kPsyFolder, 0))
				    .to_equal(size_t{0});
				expect(std::string(sounds.lane[static_cast<int32_t>(Lane::OpenHat)].c_str()).rfind(kPsyFolder, 0))
				    .to_equal(size_t{0});
				for (int32_t l = 0; l < kNumLanes; l++) {
					if (!sounds.lane[l].empty()) {
						picks[static_cast<Lane>(l)].insert(sounds.lane[l].c_str());
					}
				}
			}
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
