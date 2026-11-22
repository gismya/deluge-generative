// Psytrance (spec §4.5, #127): Classic psytrance, the Psytrance parent's only subgenre (#319), six parts (Drums, Bass,
// Drone, Lead, Arp, FX), a four-on-the-floor kick under a rolling bass that Avoids it, an empty Harmony slot, 16-bar
// phrases and long Builds into hard Drops. Run straight through the core.

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

const Genre& psyGenre = genres::psytranceClassic;

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
	it("is Classic, the Psytrance parent's only subgenre, with no musical overrides (spec §4.5, #319)", _ {
		expect(std::string(psyGenre.name)).to_equal(std::string("psytrance-classic"));
		expect(std::string(psyGenre.oledName)).to_equal(std::string("CLASSIC PSY"));
		expect(std::string(psyGenre.shortName)).to_equal(std::string("CLASSIC"));
		expect(psyGenre.version).to_equal(4);
		expect(findGenre("psytrance-classic") == &psyGenre).to_be_true();
		// The parent is never played, and no Recipe names it: no alias maps the old name to Classic (spec §4.4).
		expect(findGenre("psytrance") == nullptr).to_be_true();
		const Genre* parent = psyGenre.parent;
		expect(parent != nullptr).to_be_true();
		expect(std::string(parent->name)).to_equal(std::string("psytrance"));
		expect(std::string(parent->oledName)).to_equal(std::string("PSYTRANCE"));
		expect(parent->version).to_equal(0);
		int32_t subgenres = 0;
		for (const Genre* genre : allGenres()) {
			subgenres += genre->parent == parent ? 1 : 0;
		}
		expect(subgenres).to_equal(1);
		// The parent's table is Psytrance's: Classic plays it as it is.
		expect(psyGenre.parts.data() == parent->parts.data()).to_be_true();
		expect(psyGenre.fills.data() == parent->fills.data()).to_be_true();
		expect(psyGenre.palette == parent->palette).to_be_true();
		expect(psyGenre.palette == &genres::psytrancePalette).to_be_true();
		expect(psyGenre.facts.tempoLowBpm).to_equal(parent->facts.tempoLowBpm);
		expect(psyGenre.facts.tempoHighBpm).to_equal(parent->facts.tempoHighBpm);
		expect(psyGenre.facts.scales.data() == parent->facts.scales.data()).to_be_true();
		expect(psyGenre.phraseBars).to_equal(parent->phraseBars);
		expect(psyGenre.fillChance).to_equal(parent->fillChance);
		expect(psyGenre.sectionEndChance).to_equal(parent->sectionEndChance);
		expect(psyGenre.repickChance).to_equal(parent->repickChance);
		expect(psyGenre.densityJitter).to_equal(parent->densityJitter);
		expect(psyGenre.arc.buildBars.data() == parent->arc.buildBars.data()).to_be_true();
		expect(psyGenre.arc.dropDecayPhrases).to_equal(parent->arc.dropDecayPhrases);
		expect(psyGenre.arc.breakKeeps.parts).to_equal(parent->arc.breakKeeps.parts);
		expect(psyGenre.harmony.extensionCeiling).to_equal(parent->harmony.extensionCeiling);
	});

	it("has six parts: Drums, Bass, Drone, Lead, Arp and FX (spec §4.5)", _ {
		expect(partCount(psyGenre)).to_equal(6);
		const std::vector<std::tuple<Role, Flavour, std::string>> roster = {
		    {Role::Drums, Flavour::None, "DRUMS"},  {Role::Bass, Flavour::Loop, "BASS"},
		    {Role::Pad, Flavour::Sustain, "DRONE"}, {Role::Lead, Flavour::Loop, "LEAD"},
		    {Role::Lead, Flavour::Loop, "ARP"},     {Role::FX, Flavour::None, "FX"}};
		for (size_t p = 0; p < roster.size(); p++) {
			const PartSpec& part = psyGenre.parts[p];
			expect(part.role == std::get<0>(roster[p])).to_be_true();
			expect(part.flavour == std::get<1>(roster[p])).to_be_true();
			expect(std::string(part.name)).to_equal(std::get<2>(roster[p]));
		}
		expect(rosterFits(psyGenre)).to_be_true();
	});

	it("ghosts step 10 of the Lead's tier-1 riff X.88.88.X.88.8.8, as notes/132.md meant (listening round 2, QM2)", _ {
		int32_t riffs = 0;
		for (const Pattern& pattern : psyGenre.parts[kLeadPart].pool) {
			if (pattern.tier == 1 && pattern.length == 16 && pattern.importance[2] != 0 && pattern.importance[3] != 0
			    && pattern.importance[8] != 0) {
				riffs++;
				expect(pattern.level[3] == VelocityLevel::Ghost).to_be_true();
				expect(pattern.level[10] == VelocityLevel::Ghost).to_be_true();
			}
		}
		expect(riffs).to_equal(1);
	});

	it("steps the Lead under its root and over its fifth too, in five tier-1 riffs, one of two bars (listening round 2, S7)", _ {
		const PartSpec& lead = psyGenre.parts[kLeadPart];
		std::map<int32_t, int32_t> degrees;
		for (const LineWeight& degree : lead.loop.degrees) {
			degrees[degree.value] = degree.weight;
		}
		// The root, the 2nd, the 3rd, the 5th and the 7th as before; the 7th and the 6th under the root and the 6th over
		// the 5th now join them.
		expect(degrees == std::map<int32_t, int32_t>{{-2, 1}, {-1, 2}, {0, 5}, {1, 2}, {2, 2}, {4, 3}, {5, 1}, {6, 1}})
		    .to_be_true();
		int32_t tierOne = 0;
		int32_t twoBars = 0;
		for (const Pattern& pattern : lead.pool) {
			if (pattern.tier == 1) {
				tierOne++;
				twoBars += pattern.length == 2 * kStepsPerBar ? 1 : 0;
				expect(pattern.importance[0] == kAnchor).to_be_true();
			}
		}
		expect(tierOne).to_equal(5);
		expect(twoBars).to_equal(1);
		// Notes under the riff's root sound: over many pieces the Lead plays the leading tone (harmonic minor) too.
		std::set<int32_t> intervals;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Recipe recipe = harness::movingRecipe(psyGenre, Scale::HarmonicMinor, seed, 34);
			const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				if (e.part == kLeadPart) {
					intervals.insert(pitchClass(e.pitch - recipe.keyRoot));
				}
			}
		}
		expect(intervals.contains(11)).to_be_true();
		expect(intervals.contains(8)).to_be_true();
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

	it("takes its Arc from round 2: Intro 2, Groove 2–4, half into a Build, Break 1 keeping Drone, Arp, Lead and Closed "
	   "hat, Builds 8 ×2, 16 ×2 and 32 ×1",
	   _ {
		const ArcSpec& arc = psyGenre.arc;
		// A 32-bar Intro (round 2, Q8); more, shorter Grooves, half of them into a Build with no Break, and 8-bar Builds
		// (round 2, S2).
		expect(arc.introPhrases).to_equal(2);
		expect(arc.outroPhrases).to_equal(1);
		expect(arc.groovePhrases.low).to_equal(2);
		expect(arc.groovePhrases.high).to_equal(4);
		expect(arc.breakPhrases.low).to_equal(1);
		expect(arc.breakPhrases.high).to_equal(1);
		expect(arc.grooveToBuild).to_equal(500);
		expect(arc.buildBars.size()).to_equal(size_t{3});
		expect(arc.buildBars[0].bars).to_equal(8);
		expect(arc.buildBars[0].weight).to_equal(2);
		expect(arc.buildBars[1].bars).to_equal(16);
		expect(arc.buildBars[1].weight).to_equal(2);
		expect(arc.buildBars[2].bars).to_equal(32);
		expect(arc.buildBars[2].weight).to_equal(1);
		expect(arc.breakKeeps == PartsAndLanes{.parts = partBit(kDronePart) | partBit(kArpPart) | partBit(kLeadPart),
		                                       .lanes = laneBit(Lane::ClosedHat)})
		    .to_be_true();
		// Everything but the Lead, which comes in on the Drop.
		expect(arc.buildKeeps == kEverything.without({.parts = partBit(kLeadPart)})).to_be_true();
		expect(arc.buildCut == PartsAndLanes{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)}).to_be_true();
		expect(arc.buildCutBars).to_equal(4); // round 2, S5
		expect(arc.gap == GapLength::Beat).to_be_true();
		expect(arc.roll.lane == Lane::Snare).to_be_true();
		expect(arc.roll.top == RollStage::ThirtySecond).to_be_true();
		expect(arc.dropLiftEnergy).to_equal(macroPoints(8));
		expect(arc.dropDecayPhrases).to_equal(4);
		expect(arc.accentLane == Lane::Crash).to_be_true();
		expect(arc.accentChance).to_equal(0);
		expect(arc.transitionEntry == TransitionEntry::Drop).to_be_true();
	});

	it("stops its 4-phrase Drop decay at the next Section when a Groove after a Drop lasts one phrase (Motion 50)", _ {
		// Psytrance's decay runs past its shortest Groove at Motion 50 (spec §9.3's named exception, #349, round 2's
		// S2): the lift is still up when that Groove ends, and the Break's offsets take over on its first step.
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
					if (dropStart >= 0 && step - dropStart == kPhraseSteps && arc.section() == SectionKind::Break) {
						// Three quarters of the 4-phrase decay (+8) are left when the 1-phrase Groove ends: a
						// 1-phrase decay would be under 1 point there.
						expect(lastDensity >= macroPoints(5)).to_be_true();
						// The Break takes over on its first step: its own Density dip (-5) exactly, no lift left.
						expect(arc.offset(psyGenre, Macro::Density, centre[Macro::Density])).to_equal(macroPoints(-5));
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

	it("fills with a Rest on Kick and Bass (1–4 beats, weight 3, the held fill) and a Roll on Snare (1–2 beats, weight 2), "
	   "at 600 per-mille and on every Section's last phrase, re-picking at 300 (round 2, S9)",
	   _ {
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
		expect(psyGenre.fillChance).to_equal(600);
		expect(psyGenre.sectionEndChance).to_equal(1000);
		expect(psyGenre.repickChance).to_equal(300);
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

		it("gives the Bass Avoid Kick at strength 100, sensitivity 0, onsets on every 16th at even levels, the & anchored, "
		   "the e's thinned before the a's, a 20-tick gate, Lean 0, no slides, no accents and no mutation (round 2, S8)",
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
				// Density takes every e before any a (round 2, S8): the Intro thins to the offbeat, then K.BB.
				for (int32_t e = 1; e < pattern.length; e += kStepsPerBeat) {
					for (int32_t a = 3; a < pattern.length; a += kStepsPerBeat) {
						expect(pattern.importance[e]).to_be_less_than(pattern.importance[a]);
					}
				}
			}
			// Even and fixed per pick (round 2, S8), with a narrower Density jitter so the ranking holds.
			expect(bass.loop.draws.accent).to_equal(0);
			expect(bass.loop.mutation).to_equal(0);
			expect(psyGenre.densityJitter).to_equal(15);
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

		it("gates the Lead at 31 with a 16-bar Sweep, so it plays a Drop's first two phrases and leaves on the phrase line; the "
		   "Arp ungated with an 8-bar Sweep, the Drone a 32-bar one (round 2, S2, S10)",
		   _ {
			const PartSpec& lead = psyGenre.parts[kLeadPart];
			const PartSpec& arp = psyGenre.parts[kArpPart];
			expect(lead.densityGate).to_equal(macroPoints(31));
			expect(laneOf(Lane::Perc1).densityGate).to_equal(macroPoints(29));
			expect(lead.drift.sweepBars).to_equal(16);
			expect(arp.densityGate).to_equal(0);
			expect(arp.drift.sweepBars).to_equal(8);
			expect(psyGenre.parts[kDronePart].drift.sweepBars).to_equal(32);
			int32_t leaves = 0;
			int32_t onDrops = 0;
			int32_t dropPhrases = 0;
			int32_t builds = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				// Default Motion (25): Grooves run 2–4 phrases, so 3- and 4-phrase Grooves reach the third phrase
				// after a Drop and show the Lead leaving on the phrase line (at Motion 50 Grooves last 1–2 phrases).
				Recipe recipe = newRecipe(psyGenre, seed);
				recipe.scale = Scale::Minor;
				const Walked walked = walk(recipe, seed, 320 * kStepsPerBar);
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
				// The Lead plays every Groove bar of each Drop's first two phrases (its lift decays over four), and
				// none of the third: it leaves on the phrase line (round 2, S2).
				for (int32_t end : buildEnds(walked)) {
					if (end + 1 + 3 * kPhraseSteps > steps) {
						continue;
					}
					dropPhrases++;
					const int32_t dropBar = (end + 1) / kStepsPerBar;
					for (int32_t bar = dropBar; bar < dropBar + 48; bar++) {
						if (walked.steps[bar * kStepsPerBar].section != SectionKind::Groove) {
							break;
						}
						expect(leadBars.count(bar)).to_equal(bar < dropBar + 32 ? size_t{1} : size_t{0});
						leaves += bar == dropBar + 32 ? 1 : 0;
					}
				}
			}
			expect(builds).to_be_greater_than(0);
			expect(onDrops).to_be_greater_than(0);
			expect(dropPhrases).to_be_greater_than(0);
			expect(leaves).to_be_greater_than(0);
		});

		it("plays the Arp as a seeded sequence of the root, 3rd, 5th and 7th that holds until the next pick, on 16ths, an "
		   "onset on every 16th in every entry (round 2, S1 D)",
		   _ {
			const PartSpec& arp = psyGenre.parts[kArpPart];
			std::map<int32_t, int32_t> degrees;
			for (const LineWeight& degree : arp.loop.degrees) {
				degrees[degree.value] = degree.weight;
			}
			expect(degrees == std::map<int32_t, int32_t>{{0, 5}, {2, 2}, {4, 3}, {6, 1}}).to_be_true();
			expect(arp.loop.mutation).to_equal(0);
			expect(arp.loop.slide.atZero).to_equal(0);
			expect(arp.loop.slide.atFull).to_equal(0);
			expect(arp.loop.gateTicks).to_equal(18);
			for (const Pattern& pattern : arp.pool) {
				expect(pattern.length).to_equal(kStepsPerBar);
				// Straight 16ths, no gallop skipping the & (#354).
				for (int32_t i = 0; i < pattern.length; i++) {
					expect(pattern.importance[i]).to_be_greater_than(0);
				}
			}
			// Only the home chord's four notes, and a different figure from piece to piece: today's walks made four
			// distinct Groove bars over 40 pieces.
			std::set<std::vector<std::pair<int32_t, int32_t>>> figures;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Recipe recipe = harness::movingRecipe(psyGenre, Scale::Minor, seed);
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
				std::map<int32_t, std::vector<std::pair<int32_t, int32_t>>> bars;
				for (const Event& e : walked.noteOns) {
					if (e.part != kArpPart) {
						continue;
					}
					const int32_t interval = pitchClass(e.pitch - recipe.keyRoot);
					expect(interval == 0 || interval == 3 || interval == 7 || interval == 10).to_be_true();
					const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
					if (walked.steps[step].section == SectionKind::Groove) {
						bars[step / kStepsPerBar].emplace_back(step % kStepsPerBar, e.pitch - recipe.keyRoot);
					}
				}
				for (const auto& [bar, notes] : bars) {
					figures.insert(notes);
				}
			}
			expect(figures.size()).to_be_greater_than(size_t{6});
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

		it("cuts the Kick and Bass for a Build's last 4 bars, rolls the Snare, leaves a beat's Gap, and drops with a Crash "
		   "(round 2, S5)",
		   _ {
			int32_t builds = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 160 * kStepsPerBar);
				for (int32_t end : buildEnds(walked)) {
					builds++;
					const int32_t cutFrom = stepTick(end + 1 - 4 * kStepsPerBar);
					const int32_t gapFrom = stepTick(end + 1 - kStepsPerBeat);
					const int32_t drop = stepTick(end + 1);
					const int32_t lastBar = stepTick(end + 1 - kStepsPerBar);
					int32_t snares = 0;
					int32_t crash = 0;
					for (const Event& e : walked.noteOns) {
						if (e.tick >= cutFrom && e.tick < drop) {
							expect(e.part == kBassPart).to_be_false();
							expect(e.part == kDrumsPart && e.lane == Lane::Kick).to_be_false();
							snares += e.part == kDrumsPart && e.lane == Lane::Snare && e.tick >= lastBar && e.tick < gapFrom
							              ? 1
							              : 0;
						}
						if (e.tick >= gapFrom && e.tick < drop) {
							// The Gap: only an FX lane (the Riser) may still start.
							expect(e.part).to_equal(kFxPart);
						}
						crash += e.tick == drop && e.part == kDrumsPart && e.lane == Lane::Crash ? 1 : 0;
					}
					// 32nds in the last bar before the Gap: more than one hit a step (a 16th roll gives 12 at most).
					expect(snares).to_be_greater_than(kStepsPerBar - kStepsPerBeat);
					expect(crash).to_equal(1);
				}
			}
			expect(builds).to_be_greater_than(2);
		});

		it("plays non-silent through every Section kind, the Build and the Drop included", _ {
			std::set<SectionKind> seen;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(harness::movingRecipe(psyGenre, Scale::Minor, seed), seed, 160 * kStepsPerBar);
				std::map<SectionKind, int32_t> notes;
				for (const Event& e : walked.noteOns) {
					const int32_t step = std::min<int32_t>(stepAt(e), static_cast<int32_t>(walked.steps.size()) - 1);
					notes[walked.steps[step].section]++;
				}
				// Every Section kind this seed reaches plays something in it.
				std::set<SectionKind> reached;
				for (const auto& step : walked.steps) {
					reached.insert(step.section);
				}
				for (SectionKind kind : reached) {
					expect(notes[kind]).to_be_greater_than(0);
					seen.insert(kind);
				}
			}
			for (SectionKind kind : {SectionKind::Intro, SectionKind::Groove, SectionKind::Break, SectionKind::Build}) {
				expect(seen.count(kind)).to_equal(1U);
			}
		});
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		const harness::Lost lost = harness::lostAtEveryCorner(psyGenre, psyGenre.facts.scales);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its palette (spec §10.1, §18.2, §18.3)", _ {
		it("gives the Bass GEN Psy Bass with 000 Rich Saw Bass as its fallback, the Drone 5 presets, the Lead 3 and the Arp 2, "
		   "each on the factory card or in the pack (the rest dropped by ear in #133)",
		   _ {
			   FakeCard card = harness::factoryCardWithPack();
			   auto presets = [](int32_t part) { return psyGenre.palette->partEntries(psyGenre.parts[part]).size(); };
			   expect(presets(kBassPart)).to_equal(size_t{2});
			   expect(presets(kDronePart)).to_equal(size_t{5});
			   expect(presets(kLeadPart)).to_equal(size_t{3});
			   expect(presets(kArpPart)).to_equal(size_t{2});
			   expect(harness::partPresetsMissing(psyGenre, card).empty()).to_be_true();
			   expect(harness::mainPick(psyGenre, kBassPart))
			       .to_equal(std::string("SYNTHS/GENERATIVE/GEN Psy Bass.XML x3"));
			   // Round 2 (H3): three darker pads join 149 and 126, all at the same weight.
			   std::map<std::string, int32_t> drones;
			   for (const PaletteEntry& entry : psyGenre.palette->partEntries(psyGenre.parts[kDronePart])) {
				   drones[entry.path] = entry.weight;
			   }
			   expect(drones == std::map<std::string, int32_t>{{"SYNTHS/126 Dark FM Pad.XML", 2},
			                                                   {"SYNTHS/149 Cold 5th Pad.XML", 2},
			                                                   {"SYNTHS/130 Dark Strings.XML", 2},
			                                                   {"SYNTHS/148 Warm 5th Pad.XML", 2},
			                                                   {"SYNTHS/135 Soothing Growth Pad.XML", 2}})
			       .to_be_true();
		   });

		it("falls back to 000 Rich Saw Bass on a card without the pack, never rolling it where the pack is (round 2, H5)", _ {
			FakeLoader loader;
			loader.missing = {"SYNTHS/GENERATIVE/GEN Psy Bass.XML"};
			const SoundLoad bass = loadPartSound(*psyGenre.palette, psyGenre.parts[kBassPart],
			                                     "SYNTHS/GENERATIVE/GEN Psy Bass.XML", loader);
			expect(bass.source == SoundSource::Fallback).to_be_true();
			expect(loader.tried.back()).to_equal(std::string("SYNTHS/000 Rich Saw Bass.XML"));
			for (const PaletteEntry& entry : psyGenre.palette->partEntries(psyGenre.parts[kBassPart])) {
				if (std::string(entry.path) == "SYNTHS/000 Rich Saw Bass.XML") {
					expect(static_cast<int32_t>(entry.weight)).to_equal(0);
				}
			}
		});

		it("loads the Drone at 55 % of its preset's volume, louder than round 1's 40 (round 2, H3)", _ {
			expect(psyGenre.parts[kDronePart].volume).to_equal(55);
		});

		it("finds at least 3 files for every ungated lane, the Psy set's among them, and pack risers alone on the Riser", _ {
			FakeCard card = harness::factoryCardWithPack();
			constexpr const char* kPsyFolder = "SAMPLES/Artists/Dean Daughters/Psy/";
			auto withKits = harness::lanePicks(psyGenre, card, 800, [&](const SoundPicks& sounds) {
				// The hats are always one set: the Psy set's pair, or the 909's under the 909 kit.
				const std::string closed = sounds.lane[static_cast<int32_t>(Lane::ClosedHat)].c_str();
				const std::string open = sounds.lane[static_cast<int32_t>(Lane::OpenHat)].c_str();
				expect(closed.rfind(kPsyFolder, 0) == 0).to_equal(open.rfind(kPsyFolder, 0) == 0);
				expect(closed.rfind(kPsyFolder, 0) == 0 || closed == "SAMPLES/DRUMS/HatC/909 Closed hihat.wav").to_be_true();
			});
			// The lanes' own lists, the kit aside (spec §10.1, §10.2).
			auto picks = harness::lanePicks(psyGenre, card, 800, {}, false);
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
				for (const std::string& path : withKits[lane]) {
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
