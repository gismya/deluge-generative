// Ambient (spec §4.5, §9.5, #125): a genre of its own, six parts (Drums, Bass, Pad, Drone, Lead, FX) over a slow Chord
// graph, an Arc that swells and recedes and never drops, and FADE THROUGH as its transition entry. Run straight through
// the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/palette.h"
#include "generative/core/voicing.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace harness;

namespace {

const Genre& ambientGenre = genres::ambient;

constexpr int32_t kBassPart = 1;
constexpr int32_t kPadPart = 2;
constexpr int32_t kDronePart = 3;
constexpr int32_t kLeadPart = 4;
constexpr int32_t kFxPart = 5;

Recipe ambientRecipe(Scale scale = Scale::Major, uint8_t keyRoot = 2, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(ambientGenre, entropy);
	recipe.keyRoot = keyRoot;
	recipe.scale = scale;
	return recipe;
}

/// Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec §9.3: at Motion 50 a Groove
/// lasts a third of its base).
Recipe movingRecipe(Scale scale, uint32_t entropy, int32_t density = 25) {
	Recipe recipe = ambientRecipe(scale, static_cast<uint8_t>(entropy % 12), entropy);
	recipe.macros.set(Macro::Motion, macroPoints(50));
	recipe.macros.set(Macro::Density, macroPoints(density));
	return recipe;
}

/// The pitch classes of `chord` (scale steps from the tonic) in the key.
std::set<int32_t> classesOf(const Chord& chord, const Recipe& recipe) {
	std::set<int32_t> out;
	for (int32_t i = 0; i < chord.size; i++) {
		out.insert(pitchClass(semitonesOf(chord.steps[i], recipe.scale) + recipe.keyRoot));
	}
	return out;
}

/// What each decided step of a piece had: where the Arc stood, the effective macros, which parts the Arc silenced, and
/// its Control events.
struct ArcStep {
	SectionKind section = SectionKind::Intro;
	bool drop = false;
	int32_t energy = 0;
	int32_t density = 0;
	uint32_t silenced = 0;
	std::vector<Event> controls;
};

std::vector<ArcStep> arcSteps(const Recipe& recipe, uint32_t seed, int32_t count) {
	std::vector<ArcStep> out;
	Generator generator;
	generator.start(recipe, seed, Takes{}, false);
	SteeringQueue queue;
	for (int32_t n = 0; n < count && !generator.ended(); n++) {
		generator.decideNextStep(queue, nullptr);
		ArcStep step{generator.arc().section(), generator.arc().dropStartsHere(),
		             generator.effectiveMacro(Macro::Energy), generator.effectiveMacro(Macro::Density)};
		for (int32_t p = 0; p < partCount(ambientGenre); p++) {
			step.silenced |= generator.partSilenced(p) ? 1U << p : 0U;
		}
		Event event;
		while (generator.popDue(INT32_MAX, &event)) {
			if (event.kind == EventKind::Control) {
				step.controls.push_back(event);
			}
		}
		out.push_back(step);
	}
	return out;
}

/// A Control event's set and control.
using ControlKey = std::tuple<uint8_t, Lane, SoundControl>;

ControlKey keyOf(const Event& e) {
	return {e.part, e.lane, e.control};
}

} // namespace

// clang-format off
describe ambient("Ambient", $ {
	it("is a genre of its own with six parts: Drums, Bass, Pad, Drone, Lead and FX (spec §4.5)", _ {
		expect(std::string(ambientGenre.name)).to_equal(std::string("ambient"));
		expect(std::string(ambientGenre.oledName)).to_equal(std::string("AMBIENT"));
		expect(ambientGenre.parent == nullptr).to_be_true();
		expect(ambientGenre.version).to_equal(1);
		expect(findGenre("ambient") == &ambientGenre).to_be_true();
		expect(partCount(ambientGenre)).to_equal(6);
		const std::vector<std::tuple<Role, Flavour, std::string>> roster = {
		    {Role::Drums, Flavour::None, "DRUMS"}, {Role::Bass, Flavour::Loop, "BASS"},
		    {Role::Pad, Flavour::Sustain, "PAD"},  {Role::Pad, Flavour::Sustain, "DRONE"},
		    {Role::Lead, Flavour::Motif, "BELLS"}, {Role::FX, Flavour::None, "FX"}};
		for (size_t p = 0; p < roster.size(); p++) {
			const PartSpec& part = ambientGenre.parts[p];
			expect(part.role == std::get<0>(roster[p])).to_be_true();
			expect(part.flavour == std::get<1>(roster[p])).to_be_true();
			expect(std::string(part.name)).to_equal(std::get<2>(roster[p]));
		}
		expect(rosterFits(ambientGenre)).to_be_true();
	});

	it("plays 60–90 BPM in Major, Lydian or Dorian, straight, in 8-bar phrases, every macro at 25", _ {
		expect(ambientGenre.facts.tempoLowBpm).to_equal(60);
		expect(ambientGenre.facts.tempoHighBpm).to_equal(90);
		std::set<Scale> scales(ambientGenre.facts.scales.begin(), ambientGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Major, Scale::Lydian, Scale::Dorian}).to_be_true();
		expect(ambientGenre.facts.swing).to_equal(0);
		expect(ambientGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(ambientGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("has no fills, no drop-outs, no accent lane, no roll, no Gap and no Build cut, and enters by FADE THROUGH (spec §9.5)", _ {
		expect(ambientGenre.fills.empty()).to_be_true();
		expect(ambientGenre.dropOuts.empty()).to_be_true();
		expect(ambientGenre.arc.accentLane == Lane::None).to_be_true();
		expect(ambientGenre.arc.roll.lane == Lane::None).to_be_true();
		expect(ambientGenre.arc.gap == GapLength::None).to_be_true();
		expect(ambientGenre.arc.buildCutBars).to_equal(0);
		expect(ambientGenre.arc.transitionEntry == TransitionEntry::Intro).to_be_true();
	});

	it("takes its Arc from the brief: Groove 6–12, Break 1–2, a 16-bar Build, Intro and Outro 2, Drop decay 2", _ {
		const ArcSpec& arc = ambientGenre.arc;
		expect(arc.groovePhrases.low).to_equal(6);
		expect(arc.groovePhrases.high).to_equal(12);
		expect(arc.breakPhrases.low).to_equal(1);
		expect(arc.breakPhrases.high).to_equal(2);
		expect(arc.buildBars.size()).to_equal(size_t{1});
		expect(arc.buildBars[0].bars).to_equal(16);
		expect(arc.grooveToBuild).to_equal(300);
		expect(arc.introPhrases).to_equal(2);
		expect(arc.outroPhrases).to_equal(2);
		expect(arc.dropDecayPhrases).to_equal(2);
		const SectionOffsets& dip = arc.offsets[static_cast<int32_t>(SectionKind::Break)];
		expect(dip.energyTo).to_equal(macroPoints(-10));
		expect(dip.densityTo).to_equal(macroPoints(-15));
		// The Break keeps the Pad, the Drone and the Lead; the Build keeps everything.
		expect(arc.breakKeeps == PartsAndLanes{.parts = partBit(kPadPart) | partBit(kDronePart) | partBit(kLeadPart)})
		    .to_be_true();
		expect(arc.buildKeeps == kEverything).to_be_true();
	});

	context("its no-heard-Drop rules (spec §9.5)", _ {
		it("switches every gesture moment off on every part, so no part has a gesture offset for Reset to snap", _ {
			constexpr uint16_t every = (1U << kNumGestureMoments) - 1;
			for (const PartSpec& part : ambientGenre.parts) {
				expect(part.gestures.off & every).to_equal(every);
			}
		});

		it("tops its Build at the Drop lift, for Energy and for Density", _ {
			const SectionOffsets& build = ambientGenre.arc.offsets[static_cast<int32_t>(SectionKind::Build)];
			expect(build.energyTo).to_equal(ambientGenre.arc.dropLiftEnergy);
			expect(build.densityTo).to_equal(ambientGenre.arc.dropLiftDensity);
			expect(ambientGenre.arc.dropLiftEnergy).to_equal(macroPoints(3));
			expect(ambientGenre.arc.dropLiftDensity).to_equal(macroPoints(3));
		});

		it("never lowers Energy or Density on a Drop's step, nor silences a part there, at three Densities", _ {
			int32_t drops = 0;
			for (int32_t density : {10, 25, 40}) {
				for (uint32_t seed = 1; seed <= 4; seed++) {
					const std::vector<ArcStep> steps = arcSteps(movingRecipe(Scale::Major, seed, density), seed, 160 * kStepsPerBar);
					for (size_t n = 1; n < steps.size(); n++) {
						if (!steps[n].drop) {
							continue;
						}
						drops++;
						expect(steps[n - 1].section == SectionKind::Build).to_be_true();
						expect(steps[n].energy).to_equal(steps[n - 1].energy);
						expect(steps[n].density).to_equal(steps[n - 1].density);
						// No gate closes on the Drop's bar line: every part playing at the Build's end plays on.
						expect(steps[n].silenced & ~steps[n - 1].silenced).to_equal(0U);
					}
				}
			}
			expect(drops).to_be_greater_than(11);
		});

		it("moves no sound control on a Drop's step by more than its drift and Sweeps move it on any other step", _ {
			int32_t drops = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const std::vector<ArcStep> steps = arcSteps(movingRecipe(Scale::Lydian, seed), seed, 160 * kStepsPerBar);
				std::map<ControlKey, int32_t> sent;
				std::map<ControlKey, int32_t> largest; // the largest move each control made on a step that isn't a Drop
				std::vector<std::pair<ControlKey, int32_t>> onDrops;
				for (size_t n = 0; n < steps.size(); n++) {
					for (const Event& e : steps[n].controls) {
						const int32_t move = std::abs(e.offset - sent[keyOf(e)]);
						sent[keyOf(e)] = e.offset;
						if (n == 0) {
							continue; // the piece's first step jumps from home
						}
						if (steps[n].drop) {
							onDrops.emplace_back(keyOf(e), move);
						}
						else {
							largest[keyOf(e)] = std::max(largest[keyOf(e)], move);
						}
					}
					drops += steps[n].drop ? 1 : 0;
				}
				for (const auto& [key, move] : onDrops) {
					expect(move).to_be_less_than(largest[key] + 1);
				}
			}
			expect(drops).to_be_greater_than(5);
		});
	});

	context("its Chord graph (spec §4.5, §7.4)", _ {
		it("is home I with the brief's moves, a heavy repeat, 8 bars a chord, no cadence target, Extensions 1–2", _ {
			const ChordGraph& graph = *ambientGenre.harmony.graph;
			std::set<std::pair<int32_t, int32_t>> moves;
			for (const ChordMove& move : graph.moves) {
				moves.insert({move.from, move.to});
				expect(move.weight).to_be_less_than(graph.repeatWeight);
			}
			expect(moves == std::set<std::pair<int32_t, int32_t>>{{kI, kIV}, {kI, kVI}, {kIV, kI}, {kVI, kIV}, {kI, kV}, {kII, kIV}, {kV, kI}})
			    .to_be_true();
			expect(graph.repeatWeight).to_equal(40);
			expect(graph.barsPerChord).to_equal(8);
			expect(graph.cadenceTarget).to_equal(kNoCadenceTarget);
			expect(ambientGenre.harmony.extensionFloor).to_equal(1);
			expect(ambientGenre.harmony.extensionCeiling).to_equal(2);
		});

		it("changes chord only on phrase lines, and holds a chord over two phrases or more now and then", _ {
			int32_t held = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Major, 0, seed), seed, 96 * kStepsPerBar);
				for (int32_t change : walked.changes()) {
					expect(change % (8 * kStepsPerBar)).to_equal(0);
				}
				for (int32_t phrase = 1; phrase < 12; phrase++) {
					held += walked.steps[phrase * 8 * kStepsPerBar].chord == walked.steps[phrase * 8 * kStepsPerBar - 1].chord ? 1 : 0;
				}
			}
			expect(held).to_be_greater_than(4);
		});

		it("leaves V by a move of its own, so V holds about as long as its other chords, not a sink", _ {
			int32_t onV = 0;
			int32_t entries = 0;
			for (uint32_t seed = 1; seed <= 48; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Major, 5, seed), seed, 192 * kStepsPerBar, {}, true);
				for (int32_t bar = 0; bar < 192; bar++) {
					const bool v = walked.steps[bar * kStepsPerBar].chord == kV;
					onV += v ? 1 : 0;
					entries += v && (bar == 0 || walked.steps[(bar - 1) * kStepsPerBar].chord != kV) ? 1 : 0;
				}
			}
			expect(entries).to_be_greater_than(30);
			// With V–I (25) besides the home pull (10), V holds about 20 bars on average here; with the home pull
			// alone it held about 29 (the walk's Section lines cut the longest holds short).
			expect(onV).to_be_less_than(entries * 24);
		});
	});

	context("its parts (spec §4.5)", _ {
		it("plays a Bass of the chord root alone, one note on the 1 of each two bars held to the pattern's end", _ {
			const PartSpec& bass = ambientGenre.parts[kBassPart];
			expect(bass.densityGate).to_equal(macroPoints(15));
			expect(bass.loop.answers.empty()).to_be_true();
			expect(bass.loop.slide.atZero == 0 && bass.loop.slide.atFull == 0).to_be_true();
			expect(bass.expression.spread).to_equal(0);
			// The key's root between F#1 and F2, an octave under the Drone's, with room for the octave down under it.
			expect(bass.reg.anchor).to_equal(36);
			expect(bass.reg.spanLow).to_equal(-12);
			expect(bass.reg.spanHigh).to_equal(5);
			for (uint8_t key = 0; key < 12; key++) {
				const int32_t tonic = tonicNote(bass.reg, key);
				expect(tonic >= 30 && tonic <= 41).to_be_true();
				expect(tonicNote(ambientGenre.parts[kDronePart].reg, key)).to_equal(tonic + 12);
			}
			int32_t notes = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = ambientRecipe(Scale::Dorian, 7, seed);
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar, {}, true);
				for (const Held& note : heldNotes(walked.notes, kBassPart)) {
					if (note.off == INT32_MAX) {
						continue;
					}
					notes++;
					const int32_t step = note.on / kTicksPerStep;
					expect(step % (2 * kStepsPerBar)).to_equal(0);
					expect(note.off - note.on).to_be_less_than(2 * kBarTicks + 1);
					const int32_t root = pitchClass(semitonesOf(walked.steps[step].chord, recipe.scale) + recipe.keyRoot);
					expect(pitchClass(note.pitch)).to_equal(root);
				}
			}
			expect(notes).to_be_greater_than(40);
		});

		it("plays the Bass's octave down at Complexity 50, under the key's root, and nothing an octave over it", _ {
			const PartSpec& bass = ambientGenre.parts[kBassPart];
			int32_t under = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				Recipe recipe = ambientRecipe(Scale::Major, static_cast<uint8_t>(seed * 5 % 12), seed);
				recipe.macros.set(Macro::Complexity, macroPoints(50));
				const int32_t tonic = tonicNote(bass.reg, recipe.keyRoot);
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar, {}, true);
				for (const Event& e : walked.noteOns) {
					if (e.part != kBassPart) {
						continue;
					}
					expect(e.pitch >= tonic - 12 && e.pitch <= tonic + 5).to_be_true();
					// V and vi fold to a 4th and a minor 3rd under the tonic; only the octave down goes lower.
					under += e.pitch < tonic - 5 ? 1 : 0;
				}
			}
			expect(under).to_be_greater_than(0);
		});

		it("voices each chord on its Pad, Extensions included, never more than 4 notes, with a 16-bar Sweep", _ {
			const PartSpec& pad = ambientGenre.parts[kPadPart];
			expect(pad.sustain.pedal || pad.sustain.doubleRoot).to_be_false();
			expect(pad.sustain.retriggerBars).to_equal(0);
			expect(pad.reg.anchor).to_equal(64);
			expect(pad.reg.spanLow).to_equal(-4);
			expect(pad.reg.spanHigh).to_equal(12);
			expect(pad.drift.sweepBars).to_equal(16);
			expect(pad.densityGate).to_equal(0);
			int32_t notes = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = movingRecipe(Scale::Lydian, seed, 40);
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
				const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 1, 2);
				std::map<int32_t, int32_t> sounding;
				for (const Held& note : heldNotes(walked.notes, kPadPart)) {
					notes++;
					const int32_t step = note.on / kTicksPerStep;
					const std::set<int32_t> chord = classesOf(voiceChord(recipe.scale, walked.steps[step].chord, level), recipe);
					expect(chord.count(pitchClass(note.pitch))).to_equal(size_t{1});
					for (int32_t tick = note.on; tick < std::min(note.off, kBarTicks * 96); tick += kTicksPerStep) {
						expect(++sounding[tick]).to_be_less_than(5);
					}
				}
			}
			expect(notes).to_be_greater_than(50);
		});

		it("keeps the Pad over the Drone in every key: no shared pitch, no overlap", _ {
			for (uint8_t key = 0; key < 12; key++) {
				for (Scale scale : {Scale::Major, Scale::Lydian, Scale::Dorian}) {
					Recipe recipe = ambientRecipe(scale, key, key + 1U);
					recipe.macros.set(Macro::Complexity, macroPoints(50));
					const Walked walked = walk(recipe, key + 1U, 64 * kStepsPerBar, {}, true);
					int32_t droneTop = 0;
					int32_t padBottom = 127;
					for (const Event& e : walked.noteOns) {
						if (e.part == kDronePart) {
							droneTop = std::max<int32_t>(droneTop, e.pitch);
						}
						if (e.part == kPadPart) {
							padBottom = std::min<int32_t>(padBottom, e.pitch);
						}
					}
					expect(padBottom).to_be_greater_than(droneTop);
				}
			}
		});

		it("holds a Drone on the key's root and fifth from the Intro's first step, an octave under the Pad", _ {
			const PartSpec& drone = ambientGenre.parts[kDronePart];
			expect(drone.sustain.pedal).to_be_true();
			expect(drone.densityGate).to_equal(0);
			expect(drone.reg.anchor).to_equal(48);
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = ambientRecipe(Scale::Major, 5, seed);
				const Walked walked = walk(recipe, seed, 16 * kStepsPerBar);
				bool first = false;
				for (const Event& e : walked.noteOns) {
					if (e.part != kDronePart) {
						continue;
					}
					first = first || e.tick < kTicksPerStep;
					const int32_t interval = pitchClass(e.pitch - recipe.keyRoot);
					expect(interval == 0 || interval == 7).to_be_true();
				}
				expect(first).to_be_true();
			}
		});

		it("plays sparse bells on its Lead: a Motif gated at 10, tier 0 on the 1 and the & of 3, Fresh spread 10", _ {
			const PartSpec& lead = ambientGenre.parts[kLeadPart];
			expect(lead.densityGate).to_equal(macroPoints(10));
			expect(lead.expression.spread).to_equal(10);
			expect(lead.expression.spreadMode == SpreadMode::Fresh).to_be_true();
			for (const Pattern& pattern : lead.pool) {
				if (pattern.tier != 0) {
					continue;
				}
				for (int32_t i = 0; i < pattern.length; i++) {
					const int32_t inBar = i % kStepsPerBar;
					expect(pattern.importance[i] == 0 || inBar == 0 || inBar == 10).to_be_true();
				}
			}
			int32_t notes = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Major, 0, seed), seed, 32 * kStepsPerBar, {}, true);
				for (const Event& e : walked.noteOns) {
					notes += e.part == kLeadPart ? 1 : 0;
				}
			}
			expect(notes).to_be_greater_than(30);
		});

		it("drifts the Pad, the Drone and the Lead at ×1.5", _ {
			for (int32_t p : {kPadPart, kDronePart, kLeadPart}) {
				expect(ambientGenre.parts[p].drift.weight).to_equal(1500);
			}
		});

		it("gates every soft-percussion lane and both FX lanes at 35, with no Ornaments and no Impact lane", _ {
			std::vector<Lane> lanes;
			for (const LaneSpec& lane : ambientGenre.parts[0].lanes) {
				lanes.push_back(lane.lane);
				expect(lane.densityGate).to_equal(macroPoints(35));
				expect(lane.pool.empty()).to_be_false();
				expect(lane.expression.ornaments.rateCeiling == 0 && lane.expression.ornaments.rateFloor == 0).to_be_true();
			}
			expect(lanes == std::vector<Lane>{Lane::Kick, Lane::Snare, Lane::Rim, Lane::ClosedHat, Lane::Perc1, Lane::Perc2})
			    .to_be_true();
			std::vector<Lane> fx;
			for (const LaneSpec& lane : ambientGenre.parts[kFxPart].lanes) {
				fx.push_back(lane.lane);
				expect(lane.densityGate).to_equal(macroPoints(35));
			}
			expect(fx == std::vector<Lane>{Lane::Riser, Lane::Fall}).to_be_true();
		});

		it("plays no percussion at the default Density, and plays it once Density reaches 40", _ {
			std::map<int32_t, int32_t> hits; // Density → drum hits
			for (int32_t density : {25, 40}) {
				Recipe recipe = ambientRecipe(Scale::Major, 0, 3);
				recipe.macros.set(Macro::Density, macroPoints(density));
				const Walked walked = walk(recipe, 3, 32 * kStepsPerBar, {}, true);
				for (const Event& e : walked.noteOns) {
					hits[density] += e.part == 0 ? 1 : 0;
				}
			}
			expect(hits[25]).to_equal(0);
			expect(hits[40]).to_be_greater_than(50);
		});
	});

	it("recedes to the Pad, the Drone and the bells in a Break, and plays all three there", _ {
		std::map<int32_t, int32_t> kept;
		int32_t breakSteps = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Walked walked = walk(movingRecipe(Scale::Major, seed), seed, 128 * kStepsPerBar);
			for (const Walked::Step& step : walked.steps) {
				breakSteps += step.section == SectionKind::Break ? 1 : 0;
			}
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				// The Pad and the Drone are struck on the Break's first step and held through it.
				if (walked.steps[step].section != SectionKind::Break) {
					continue;
				}
				expect(e.part == kPadPart || e.part == kDronePart || e.part == kLeadPart || e.part == kFxPart).to_be_true();
				kept[e.part]++;
			}
			// A held Sustain sounds through the Break with no note-on of its own there.
			for (int32_t part : {kPadPart, kDronePart}) {
				for (const Held& note : heldNotes(walked.notes, part)) {
					for (size_t n = 0; n < walked.steps.size(); n++) {
						const int32_t tick = static_cast<int32_t>(n) * kTicksPerStep;
						kept[part] += walked.steps[n].section == SectionKind::Break && note.on <= tick && tick < note.off ? 1 : 0;
					}
				}
			}
		}
		expect(breakSteps).to_be_greater_than(8 * kStepsPerBar);
		expect(kept[kPadPart]).to_be_greater_than(0);
		expect(kept[kDronePart]).to_be_greater_than(0);
		expect(kept[kLeadPart]).to_be_greater_than(0);
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		for (Scale scale : ambientGenre.facts.scales) {
			for (int32_t complexity : {0, 25, 50}) {
				for (int32_t density : {0, 25, 50}) {
					Recipe recipe = movingRecipe(scale, static_cast<uint32_t>(complexity + density + 1), density);
					recipe.macros.set(Macro::Complexity, macroPoints(complexity));
					const Walked walked = walk(recipe, 2, 128 * kStepsPerBar);
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
				}
			}
		}
	});

	context("its palette (spec §10.1, §18.2, §18.3)", _ {
		it("gives every synth part at least 3 presets, each on the factory card or in the pack, its role listing them", _ {
			FakeCard card = harness::factoryCardWithPack();
			for (int32_t p = kBassPart; p <= kLeadPart; p++) {
				const PartSpec& part = ambientGenre.parts[p];
				auto entries = ambientGenre.palette->partEntries(part);
				expect(entries.size()).to_be_greater_than(size_t{2});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
				}
			}
			// The pack's sine sub is the Bass's main pick.
			const auto bass = ambientGenre.palette->partEntries(ambientGenre.parts[kBassPart]);
			expect(std::string(bass[0].path)).to_equal(std::string("SYNTHS/GENERATIVE/GEN Sine Sub.XML"));
			expect(bass[0].weight).to_equal(3);
			for (const RoleSounds& role : ambientGenre.palette->roles) {
				expect(role.role == Role::Bass || role.role == Role::Pad || role.role == Role::Lead).to_be_true();
			}
		});

		it("finds files for every lane, the pack's brushes alone on the Snare and pack risers alone on the Riser", _ {
			FakeCard card = harness::factoryCardWithPack();
			std::map<Lane, std::set<std::string>> picks;
			for (uint32_t entropy = 0; entropy < 600; entropy++) {
				SoundPicks sounds;
				rollSounds(ambientGenre, *ambientGenre.palette, sounds, entropy, card);
				for (int32_t l = 0; l < kNumLanes; l++) {
					if (!sounds.lane[l].empty()) {
						picks[static_cast<Lane>(l)].insert(sounds.lane[l].c_str());
					}
				}
			}
			for (const PartSpec& part : ambientGenre.parts) {
				for (const LaneSpec& lane : part.lanes) {
					// Kick, Rim and Closed hat have only the CR78's and the KR55's (#313).
					expect(picks[lane.lane].size()).to_be_greater_than(size_t{1});
				}
			}
			for (const std::string& snare : picks[Lane::Snare]) {
				expect(snare.rfind("SAMPLES/DRUMS/Snare/GEN Brush ", 0)).to_equal(size_t{0});
			}
			for (const std::string& riser : picks[Lane::Riser]) {
				expect(riser.rfind("SAMPLES/FX/Riser/GEN ", 0)).to_equal(size_t{0});
			}
			expect(picks[Lane::Perc1].size()).to_be_greater_than(size_t{4});
		});
	});
});

CPPSPEC_SPEC(ambient)
