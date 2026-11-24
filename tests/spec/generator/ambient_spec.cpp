// Ambient (spec §4.5, §9.5, #125): a genre of its own, six parts (Drums, Bass, Pad, Drone, Lead, FX) over a slow Chord
// graph, an Arc that swells and recedes and never drops, and FADE THROUGH as its transition entry. Run straight through
// the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/palette.h"
#include "generative/core/voicing.h"
#include "harness.h"
#include "palette_helpers.h"
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
			step.silenced |= generator.partSilenced(p) ? partBit(p) : 0U;
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

/// Over Pad chord changes: a pitch class the voicings on both sides share, held at its pitch, or moved an octave and
/// struck again there (round 2's HOLD). The Pad played from a copy of Ambient whose holdSharedChance is `chance`.
struct SharedTones {
	int32_t held = 0;
	int32_t moved = 0;
};

SharedTones padSharedTones(int32_t chance) {
	std::vector<PartSpec> parts(ambientGenre.parts.begin(), ambientGenre.parts.end());
	parts[kPadPart].sustain.holdSharedChance = static_cast<uint16_t>(chance);
	Genre genre = ambientGenre;
	genre.parts = parts;
	SharedTones out;
	for (uint32_t seed = 1; seed <= 6; seed++) {
		for (Scale scale : ambientGenre.facts.scales) {
			Recipe recipe = harness::movingRecipe(ambientGenre, scale, seed, 40);
			recipe.genre = &genre;
			const Walked walked = walk(recipe, seed, 128 * kStepsPerBar, {}, true);
			const std::vector<Held> notes = heldNotes(walked.notes, kPadPart);
			for (int32_t change : walked.changes()) {
				const int32_t tick = change * kTicksPerStep;
				std::set<int32_t> before;
				std::set<int32_t> after;
				for (const Held& note : notes) {
					if (note.on < tick && note.off >= tick) {
						before.insert(note.pitch);
					}
					if (note.on <= tick && note.off > tick) {
						after.insert(note.pitch);
					}
				}
				for (int32_t pitch : before) {
					if (after.count(pitch) != 0) {
						out.held++;
						continue;
					}
					for (int32_t other : after) {
						out.moved += pitchClass(other - pitch) == 0 ? 1 : 0;
					}
				}
			}
		}
	}
	return out;
}

/// Ambient with its Bass's tie chance at `chance` per-mille (LoopSpec::tieChance).
struct TiedAmbient {
	std::vector<PartSpec> parts;
	Genre genre;
	explicit TiedAmbient(int32_t chance)
	    : parts(ambientGenre.parts.begin(), ambientGenre.parts.end()), genre(ambientGenre) {
		parts[kBassPart].loop.tieChance = static_cast<uint16_t>(chance);
		genre.parts = parts;
	}
};

/// A piece of `genre`'s Bass: its notes, and the steps it was decided on.
struct BassRun {
	std::vector<Held> notes;
	std::vector<Walked::Step> steps;
};

BassRun bassRun(const Genre& genre, uint32_t seed, const WalkSteer& steer = {}) {
	Recipe recipe = ambientRecipe(Scale::Dorian, 7, seed);
	recipe.genre = &genre;
	Walked walked = walk(recipe, seed, 128 * kStepsPerBar, steer);
	return {heldNotes(walked.notes, kBassPart), std::move(walked.steps)};
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
		expect(ambientGenre.version).to_equal(3);
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

	it("plays 60–90 BPM in Major, Lydian, Dorian, Minor or Mixolydian, straight, in 8-bar phrases, every macro at 25", _ {
		expect(ambientGenre.facts.tempoLowBpm).to_equal(60);
		expect(ambientGenre.facts.tempoHighBpm).to_equal(90);
		std::set<Scale> scales(ambientGenre.facts.scales.begin(), ambientGenre.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Major, Scale::Lydian, Scale::Dorian, Scale::Minor, Scale::Mixolydian})
		    .to_be_true(); // Minor and Mixolydian: rules round 1's S6-scales A
		expect(ambientGenre.facts.swingLow).to_equal(int8_t{0});
		expect(ambientGenre.facts.swingHigh).to_equal(int8_t{0});
		expect(ambientGenre.phraseBars).to_equal(8);
		for (int32_t m = 0; m < kNumMacros; m++) {
			expect(ambientGenre.defaults[static_cast<Macro>(m)]).to_equal(macroPoints(25));
		}
	});

	it("has no fills, no accent lane, no roll, no Gap and no Build cut, and enters by FADE THROUGH (spec §9.5)", _ {
		expect(ambientGenre.fills.empty()).to_be_true();
		expect(ambientGenre.arc.accentLane == Lane::None).to_be_true();
		expect(ambientGenre.arc.roll.lane == Lane::None).to_be_true();
		expect(ambientGenre.arc.gap == GapLength::None).to_be_true();
		expect(ambientGenre.arc.buildCutBars).to_equal(0);
		expect(ambientGenre.arc.transitionEntry == TransitionEntry::Intro).to_be_true();
	});

	it("rests the Bass and the bells for 4 or 8 bars now and then in a Groove, never the Pad or the Drone (spec §8.8)", _ {
		expect(ambientGenre.dropOuts.empty()).to_be_false();
		for (const DropOutEntry& entry : ambientGenre.dropOuts) {
			expect(entry.mask.lanes).to_equal(0U);
			expect((entry.mask.parts & ~(partBit(kBassPart) | partBit(kLeadPart))) == 0).to_be_true();
			expect(entry.bars == 4 || entry.bars == 8).to_be_true();
			expect(entry.throwTarget.empty()).to_be_true();
		}
		expect(ambientGenre.dropOutChance).to_equal(150);
		expect(ambientGenre.dropOutGapBars).to_equal(4);
		std::map<int32_t, int32_t> restingBars; // part → Groove bars it rests
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Walked walked = walk(ambientRecipe(Scale::Major, 0, seed), seed, 96 * kStepsPerBar, {}, true);
			for (int32_t part : {kBassPart, kPadPart, kDronePart, kLeadPart}) {
				for (size_t bar = 0; bar * kStepsPerBar < walked.steps.size(); bar++) {
					if (walked.steps[bar * kStepsPerBar].section != SectionKind::Groove) {
						continue;
					}
					const int32_t start = static_cast<int32_t>(bar) * kTicksPerBar;
					bool sounds = false;
					for (const Held& note : heldNotes(walked.notes, part)) {
						sounds = sounds || (note.on < start + kTicksPerBar && note.off > start);
					}
					restingBars[part] += sounds ? 0 : 1;
				}
			}
		}
		expect(restingBars[kBassPart]).to_be_greater_than(7);
		expect(restingBars[kLeadPart]).to_be_greater_than(7);
		expect(restingBars[kPadPart]).to_equal(0);
		expect(restingBars[kDronePart]).to_equal(0);
	});

	it("takes its Arc from rules round 1: Groove 4–8, Break 2–4, a 16-bar Build, Intro and Outro 2, Drop decay 2", _ {
		const ArcSpec& arc = ambientGenre.arc;
		expect(arc.groovePhrases.low).to_equal(4);
		expect(arc.groovePhrases.high).to_equal(8);
		expect(arc.breakPhrases.low).to_equal(2);
		expect(arc.breakPhrases.high).to_equal(4);
		expect(arc.buildBars.size()).to_equal(size_t{1});
		expect(arc.buildBars[0].bars).to_equal(16);
		expect(arc.grooveToBuild).to_equal(150);
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
			// +5 (owner, round 2's S4): the mallets (29) enter late in the Build and fade after the Drop.
			expect(ambientGenre.arc.dropLiftEnergy).to_equal(macroPoints(5));
			expect(ambientGenre.arc.dropLiftDensity).to_equal(macroPoints(5));
		});

		it("never lowers Energy or Density on a Drop's step, nor silences a part there, at three Densities", _ {
			int32_t drops = 0;
			for (int32_t density : {10, 25, 40}) {
				for (uint32_t seed = 1; seed <= 4; seed++) {
					const std::vector<ArcStep> steps = arcSteps(harness::movingRecipe(ambientGenre, Scale::Major, seed, density), seed, 160 * kStepsPerBar);
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
				const std::vector<ArcStep> steps = arcSteps(harness::movingRecipe(ambientGenre, Scale::Lydian, seed), seed, 160 * kStepsPerBar);
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
		it("is home I with round 2's moves (iii and ♭VII, each mostly back home), a heavy repeat, 8 bars a chord, no cadence target, Extensions 1–2", _ {
			const ChordGraph& graph = *ambientGenre.harmony.graph;
			std::set<std::pair<int32_t, int32_t>> moves;
			for (const ChordMove& move : graph.moves) {
				moves.insert({move.from, move.to});
				expect(move.weight).to_be_less_than(graph.repeatWeight);
			}
			// Owner, round 2's S6: between today's graph and the review's, adding I–iii and I–♭VII, each mostly back home.
			expect(moves == std::set<std::pair<int32_t, int32_t>>{{kI, kIV}, {kI, kVI}, {kI, kIII}, {kI, kVII}, {kI, kV},
			                                                      {kIV, kI}, {kVI, kIV}, {kVI, kI}, {kIII, kIV},
			                                                      {kVII, kI}, {kII, kI}, {kV, kI}})
			    .to_be_true();
			expect(graph.repeatWeight).to_equal(40);
			expect(graph.homePull).to_equal(40); // owner, round 1's Q338 (#338)
			expect(graph.barsPerChord).to_equal(8);
			expect(graph.cadenceTarget).to_equal(kNoCadenceTarget);
			expect(ambientGenre.harmony.extensionFloor).to_equal(1);
			expect(ambientGenre.harmony.extensionCeiling).to_equal(2);
		});

		it("tries add9 and sus4 before the 7th, so V carries no leading tone against the Drone at level 2 (S8)", _ {
			const ExtensionOrder& order = ambientGenre.harmony.extensionOrder;
			for (int32_t i = 0; i < 4; i++) {
				expect(order.tries[i] == kNinthFirst.tries[i]).to_be_true();
			}
			for (uint32_t seed = 1; seed <= 3; seed++) {
				Recipe recipe = harness::movingRecipe(ambientGenre, Scale::Major, seed, 40);
				recipe.macros.set(Macro::Complexity, macroPoints(80));
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					const Walked::Step& step = walked.steps[e.tick / kTicksPerStep];
					if (e.part != kPadPart || step.chord != kV) {
						continue;
					}
					// V's 3rd is the key's leading tone, 11 semitones over the tonic: the sus4 takes its place.
					expect(pitchClass(e.pitch - recipe.keyRoot)).not_().to_equal(11);
				}
			}
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

		it("never plays Lydian's #iv° (its IV), the Bass a tritone from the Drone: its guard factor is 0", _ {
			expect(ambientGenre.harmony.guard.factor).to_equal(0);
			expect(ambientGenre.harmony.guard.rule == ChordGuardRule::Strict).to_be_true();
			int32_t bars = 0;
			for (uint32_t seed = 1; seed <= 24; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Lydian, static_cast<uint8_t>(seed % 12), seed),
				                           seed, 192 * kStepsPerBar, {}, true);
				for (int32_t bar = 0; bar < 192; bar++) {
					expect(walked.steps[bar * kStepsPerBar].chord).not_().to_equal(kIV);
					bars++;
				}
			}
			expect(bars).to_equal(24 * 192);
		});

		it("leaves Lydian's II as it leaves V, so II holds about as long as its other chords, not a sink", _ {
			int32_t onII = 0;
			int32_t entries = 0;
			for (uint32_t seed = 1; seed <= 48; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Lydian, 5, seed), seed, 192 * kStepsPerBar, {}, true);
				for (int32_t bar = 0; bar < 192; bar++) {
					const bool ii = walked.steps[bar * kStepsPerBar].chord == kII;
					onII += ii ? 1 : 0;
					entries += ii && (bar == 0 || walked.steps[(bar - 1) * kStepsPerBar].chord != kII) ? 1 : 0;
				}
			}
			expect(entries).to_be_greater_than(30);
			// About 14 bars a visit with ii–I, about 27 without it (48 seeds).
			expect(onII).to_be_less_than(entries * 20);
		});

		it("leaves Lydian's vi as it leaves V, so vi holds about as long as its other chords, not a sink", _ {
			int32_t onVI = 0;
			int32_t entries = 0;
			for (uint32_t seed = 1; seed <= 48; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Lydian, 5, seed), seed, 192 * kStepsPerBar, {}, true);
				for (int32_t bar = 0; bar < 192; bar++) {
					const bool vi = walked.steps[bar * kStepsPerBar].chord == kVI;
					onVI += vi ? 1 : 0;
					entries += vi && (bar == 0 || walked.steps[(bar - 1) * kStepsPerBar].chord != kVI) ? 1 : 0;
				}
			}
			expect(entries).to_be_greater_than(30);
			// About 17 bars a visit with vi–I, about 26 without it (48 seeds).
			expect(onVI).to_be_less_than(entries * 21);
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
			// I–V at 8 (round 2's S6, was 10): V entered 30 times here, about 13 bars a visit.
			expect(entries).to_be_greater_than(20);
			// With V–I (25) besides the home pull, V holds about 13 bars on average here; with the home pull alone at
			// 10 it held about 29 (the walk's Section lines cut the longest holds short).
			expect(onV).to_be_less_than(entries * 24);
		});
	});

	context("its parts (spec §4.5)", _ {
		it("plays a Bass of the chord root alone, one note on the 1 of each two bars held to the pattern's end or tied on", _ {
			const PartSpec& bass = ambientGenre.parts[kBassPart];
			expect(bass.densityGate).to_equal(macroPoints(15));
			expect(bass.loop.answers.empty()).to_be_true();
			expect(bass.loop.slide.atZero == 0 && bass.loop.slide.atFull == 0).to_be_true();
			expect(bass.expression.spread).to_equal(0);
			// The key's root between F#2 and F3, the Drone's own (bass-up); V, vi and ♭VII under it (round 2's BASS).
			expect(bass.reg.anchor).to_equal(48);
			expect(bass.reg.spanLow).to_equal(-6);
			expect(bass.reg.spanHigh).to_equal(6);
			for (uint8_t key = 0; key < 12; key++) {
				const int32_t tonic = tonicNote(bass.reg, key);
				expect(tonic >= 42 && tonic <= 53).to_be_true();
				expect(tonicNote(ambientGenre.parts[kDronePart].reg, key)).to_equal(tonic);
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
					const int32_t root = pitchClass(semitonesOf(walked.steps[step].chord, recipe.scale) + recipe.keyRoot);
					expect(pitchClass(note.pitch)).to_equal(root);
				}
			}
			expect(notes).to_be_greater_than(40);
		});

		it("ties about half its notes on the root it still holds, sounding as it would struck (sign-off 1's BASS)", _ {
			expect(ambientGenre.parts[kBassPart].loop.tieChance).to_equal(500);
			const TiedAmbient struck(0);
			int32_t candidates = 0;
			int32_t ties = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				const BassRun plain = bassRun(struck.genre, seed);
				const BassRun tied = bassRun(ambientGenre, seed);
				// Each tied note is a run of the struck notes, each ending where the next starts on its pitch: it
				// sounds the same pitch over the same ticks, with fewer strikes. None runs across a Section line.
				size_t i = 0;
				for (const Held& note : tied.notes) {
					expect(i < plain.notes.size()).to_be_true();
					if (i >= plain.notes.size()) {
						break;
					}
					expect(plain.notes[i].on).to_equal(note.on);
					expect(plain.notes[i].pitch).to_equal(note.pitch);
					while (plain.notes[i].off < note.off && i + 1 < plain.notes.size()) {
						const int32_t line = plain.notes[i].off;
						expect(plain.notes[i + 1].on).to_equal(line);
						expect(plain.notes[i + 1].pitch).to_equal(note.pitch);
						expect(tied.steps[line / kTicksPerStep].sectionStart).to_be_false();
						ties++;
						i++;
					}
					expect(plain.notes[i].off).to_equal(note.off);
					i++;
				}
				expect(i).to_equal(plain.notes.size());
				// The strikes that could tie: the next note on the same pitch from the tick the last ends, inside a
				// Section.
				for (size_t n = 0; n + 1 < plain.notes.size(); n++) {
					const int32_t line = plain.notes[n].off;
					candidates += plain.notes[n + 1].on == line && plain.notes[n + 1].pitch == plain.notes[n].pitch
					                      && !plain.steps[line / kTicksPerStep].sectionStart
					                  ? 1
					                  : 0;
				}
			}
			expect(candidates).to_be_greater_than(60);
			expect(ties * 100 / candidates).to_be_between(35, 65);
		});

		it("strikes the next root after a mute lands, muted, instead of tying the unmuted note on through it", _ {
			// A plain mute lands on the next bar line, inside a 2-bar note the Bass would tie on.
			const TiedAmbient always(1000);
			const BassRun tied = bassRun(always.genre, 1);
			int32_t tie = -1;
			for (const Held& note : tied.notes) {
				if (note.off != INT32_MAX && note.off - note.on > 2 * kBarTicks) {
					tie = note.on + 2 * kBarTicks;
					break;
				}
			}
			expect(tie).to_be_greater_than(0);
			const int32_t press = tie / kTicksPerStep - 2 * kStepsPerBar + kStepsPerBeat;
			Recipe recipe = ambientRecipe(Scale::Dorian, 7, 1);
			recipe.genre = &always.genre;
			const Walked walked = walk(recipe, 1, 128 * kStepsPerBar, [press](int32_t n, const Generator&, SteeringQueue& queue) {
				if (n == press) {
					queue.push(mute(kBassPart));
				}
			});
			bool struck = false;
			for (const Event& e : walked.notes) {
				if (e.part == kBassPart && e.kind == EventKind::NoteOn && e.tick == tie) {
					struck = true;
					expect(e.muted).to_be_true();
				}
				// The note struck before the mute ends at its gate, not later.
				if (e.part == kBassPart && e.kind == EventKind::NoteOff && e.tick > tie - 2 * kBarTicks
				    && e.tick < tie + kTicksPerStep) {
					expect(e.tick).to_equal(tie);
				}
			}
			expect(struck).to_be_true();
		});

		it("folds V, vi and ♭VII under the key's root and plays the other roots at or over it, never under C#2 (about "
		   "69 Hz), at Complexity 50 too",
		   _ {
			   const PartSpec& bass = ambientGenre.parts[kBassPart];
			   expect(bass.loop.draws.octaveDown.atZero == 0 && bass.loop.draws.octaveDown.atFull == 0).to_be_true();
			   std::set<int32_t> under;
			   std::set<int32_t> over;
			   for (uint32_t seed = 1; seed <= 12; seed++) {
				   const Scale scale = ambientGenre.facts.scales[seed % ambientGenre.facts.scales.size()];
				   Recipe recipe = ambientRecipe(scale, static_cast<uint8_t>(seed % 12), seed);
				   recipe.macros.set(Macro::Complexity, macroPoints(50));
				   const int32_t tonic = tonicNote(bass.reg, recipe.keyRoot);
				   const Walked walked = walk(recipe, seed, 96 * kStepsPerBar, {}, true);
				   for (const Event& e : walked.noteOns) {
					   if (e.part != kBassPart) {
						   continue;
					   }
					   // C#2, the 5th under F#2: the lowest key's V.
					   expect(e.pitch).to_be_greater_than(36);
					   const int32_t chord = walked.steps[e.tick / kTicksPerStep].chord;
					   const bool folds = chord == kV || chord == kVI || chord == kVII;
					   expect(folds ? e.pitch < tonic && e.pitch >= tonic - 5 : e.pitch >= tonic && e.pitch < tonic + 6)
					       .to_be_true();
					   (folds ? under : over).insert(chord);
				   }
			   }
			   expect(under == std::set<int32_t>{kV, kVI, kVII}).to_be_true();
			   expect(over.count(kI) == 1 && over.count(kIV) == 1).to_be_true();
		   });

		it("voices each chord on its Pad, Extensions included, never more than 4 notes, with a 16-bar Sweep", _ {
			const PartSpec& pad = ambientGenre.parts[kPadPart];
			expect(pad.sustain.pedal || pad.sustain.doubleRoot).to_be_false();
			expect(pad.sustain.retriggerBars).to_equal(0);
			expect(pad.sustain.openVoicing).to_be_true();
			expect(pad.reg.anchor).to_equal(64);
			expect(pad.reg.spanLow).to_equal(-4);
			expect(pad.reg.spanHigh).to_equal(19);
			expect(pad.drift.sweepBars).to_equal(16);
			expect(pad.densityGate).to_equal(0);
			int32_t notes = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = harness::movingRecipe(ambientGenre, Scale::Lydian, seed, 40);
				const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
				const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 1, 2);
				std::map<int32_t, int32_t> sounding;
				for (const Held& note : heldNotes(walked.notes, kPadPart)) {
					notes++;
					const int32_t step = note.on / kTicksPerStep;
					const std::set<int32_t> chord = classesOf(
					    voiceChord(recipe.scale, walked.steps[step].chord, level, {}, kNinthFirst), recipe);
					expect(chord.count(pitchClass(note.pitch))).to_equal(size_t{1});
					for (int32_t tick = note.on; tick < std::min(note.off, kBarTicks * 96); tick += kTicksPerStep) {
						expect(++sounding[tick]).to_be_less_than(5);
					}
				}
			}
			expect(notes).to_be_greater_than(50);
		});

		// S9: the root and the 5th near the tonic, the 3rd and the Extensions an octave up, each voice then moving the
		// least, so the voicing stays open over a whole piece rather than closing to a cluster within an octave.
		it("keeps its Pad open: a four-note voicing spans more than an octave, in nearly every chord of every piece", _ {
			int32_t voicings = 0;
			int32_t open = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				for (Scale scale : {Scale::Major, Scale::Lydian, Scale::Dorian}) {
					const Recipe recipe = harness::movingRecipe(ambientGenre, scale, seed, 40);
					constexpr int32_t kBars = 128;
					const Walked walked = walk(recipe, seed, kBars * kStepsPerBar);
					std::map<int32_t, std::set<int32_t>> byBar;
					for (const Held& note : heldNotes(walked.notes, kPadPart)) {
						// A note still sounding at the end runs to INT32_MAX.
						for (int32_t bar = note.on / kBarTicks; bar < kBars && bar * kBarTicks < note.off; bar++) {
							byBar[bar].insert(note.pitch);
						}
					}
					for (const auto& [bar, pitches] : byBar) {
						if (pitches.size() != 4) {
							continue;
						}
						voicings++;
						open += *pitches.rbegin() - *pitches.begin() > 12 ? 1 : 0;
					}
				}
			}
			expect(voicings).to_be_greater_than(1000);
			expect(open * 100).to_be_greater_than(voicings * 90);
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

		it("plays sparse bells on its Lead: a Motif gated at 8, two-bar figures with empty bars, about 10 notes a minute", _ {
			const PartSpec& lead = ambientGenre.parts[kLeadPart];
			expect(lead.densityGate).to_equal(macroPoints(8));
			expect(lead.expression.spread).to_equal(10);
			expect(lead.expression.spreadMode == SpreadMode::Fresh).to_be_true();
			expect(lead.motif.reroll).to_equal(350);
			expect(ambientGenre.repickChance).to_equal(400);
			// Every figure is two bars, with no anchor, so the default Density leaves a bar of it empty now and then.
			for (const Pattern& pattern : lead.pool) {
				expect(pattern.length).to_equal(2 * kStepsPerBar);
				for (int32_t i = 0; i < pattern.length; i++) {
					expect(pattern.importance[i] != kAnchor).to_be_true();
				}
			}
			// About 10 notes a minute in the Groove at the default Recipe (owner, rules round 1's S1-amount).
			int32_t notes = 0;
			int32_t emptyBars = 0;
			int32_t grooveBars = 0;
			const Recipe recipe = ambientRecipe(Scale::Major, 0, 0);
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar, {}, true);
				for (size_t bar = 0; bar * kStepsPerBar < walked.steps.size(); bar++) {
					if (walked.steps[bar * kStepsPerBar].section != SectionKind::Groove) {
						continue;
					}
					grooveBars++;
					int32_t inBar = 0;
					for (const Event& e : walked.noteOns) {
						const int32_t eventBar = e.tick / kTicksPerBar;
						inBar += e.part == kLeadPart && eventBar == static_cast<int32_t>(bar) ? 1 : 0;
					}
					notes += inBar;
					emptyBars += inBar == 0 ? 1 : 0;
				}
			}
			// Notes a minute: notes ÷ (bars × 4 beats ÷ BPM), the tempo in hundredths of a BPM.
			const int64_t perMinuteTimes400 = int64_t{notes} * recipe.tempo / grooveBars;
			expect(perMinuteTimes400).to_be_greater_than(6 * 400);
			expect(perMinuteTimes400).to_be_less_than(14 * 400);
			expect(emptyBars).to_be_greater_than(0);
		});

		// §5.2's exception: a Sustain kept on both sides of a Section line holds the pitches that stay, so the Drop's
		// step re-strikes nothing that sounds on (spec §9.5's no heard Drop).
		it("holds its Pad and Drone across every Section line and chord change, the Drop's included: only a changed pitch "
		   "strikes", _ {
			expect(ambientGenre.parts[kPadPart].sustain.holdsAcrossSections).to_be_true();
			expect(ambientGenre.parts[kPadPart].sustain.holdsCommonTones).to_be_true();
			expect(ambientGenre.parts[kDronePart].sustain.holdsAcrossSections).to_be_true();
			int32_t drops = 0;
			int32_t heldOverDrop = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Walked walked = walk(harness::movingRecipe(ambientGenre, Scale::Major, seed), seed, 160 * kStepsPerBar, {}, true);
				std::set<int32_t> dropTicks;
				for (int32_t n = 0; n < static_cast<int32_t>(walked.steps.size()); n++) {
					if (walked.steps[n].drop) {
						dropTicks.insert(n * kTicksPerStep);
					}
				}
				drops += static_cast<int32_t>(dropTicks.size());
				for (int32_t p : {kPadPart, kDronePart}) {
					const std::vector<Held> notes = heldNotes(walked.notes, p);
					for (const Held& a : notes) {
						for (const Held& b : notes) {
							// A note ending on a line or a chord change and the same pitch starting there: a re-strike. The
							// walk starts in the Groove, so the first Groove's one re-strike is on its first step.
							expect(a.pitch == b.pitch && a.off == b.on).to_be_false();
						}
						for (int32_t tick : dropTicks) {
							heldOverDrop += a.on < tick && a.off > tick ? 1 : 0;
						}
					}
				}
			}
			expect(drops).to_be_greater_than(2);
			expect(heldOverDrop).to_be_greater_than(2);
		});

		// Owner, round 2's HOLD: today's re-strike, or round 2's held common tones, "sometimes one, sometimes the other".
		it("on a chord change, now holds a shared pitch class where today's voicing moved it an octave, about half the "
		   "time",
		   _ {
			   expect(ambientGenre.parts[kPadPart].sustain.holdSharedChance).to_equal(500);
			   const SharedTones today = padSharedTones(0);
			   const SharedTones always = padSharedTones(1000);
			   const SharedTones half = padSharedTones(ambientGenre.parts[kPadPart].sustain.holdSharedChance);
			   // Measured: moved an octave 125 times at 0, 8 at 1000 and 63 at 500; held 418, 535 and 480.
			   expect(always.moved).to_be_less_than(today.moved / 3);
			   expect(half.moved).to_be_greater_than(always.moved + (today.moved - always.moved) / 4);
			   expect(half.moved).to_be_less_than(today.moved - (today.moved - always.moved) / 4);
		   });

		// The Drone and the Pad are gated at 0 and count toward the Intro floor (spec §9.4), so the Intro builds from
		// them: Density rises from 0 over the Intro's 16 bars, the bells (8) about 5 bars in, the Bass (15) about 10.
		it("builds the Intro from the Drone: an Intro floor of 0, the bells about 5 bars in, the Bass about 10", _ {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(ambientRecipe(Scale::Major, 2, seed), seed, 16 * kStepsPerBar);
				expect(walked.generator->arc().introFloor()).to_equal(0);
				int32_t firstBells = INT32_MAX;
				int32_t firstBass = INT32_MAX;
				for (const Event& e : walked.noteOns) {
					if (e.part == kLeadPart) {
						firstBells = std::min(firstBells, e.tick);
					}
					if (e.part == kBassPart) {
						firstBass = std::min(firstBass, e.tick);
					}
				}
				expect(firstBells).to_be_greater_than(4 * kBarTicks - 1);
				expect(firstBells).to_be_less_than(9 * kBarTicks);
				expect(firstBass).to_be_greater_than(8 * kBarTicks - 1);
				expect(firstBass).to_be_less_than(14 * kBarTicks);
				expect(firstBass).to_be_greater_than(firstBells);
			}
		});

		it("drifts the Pad, the Drone and the Lead at ×1.5", _ {
			for (int32_t p : {kPadPart, kDronePart, kLeadPart}) {
				expect(ambientGenre.parts[p].drift.weight).to_equal(1500);
			}
		});

		it("staggers the soft percussion's gates from 29 to 47, mallets first and the kick last, the FX lanes at 35, with "
		   "no Ornaments and no Impact lane", _ {
			std::vector<Lane> lanes;
			std::vector<int16_t> gates;
			for (const LaneSpec& lane : ambientGenre.parts[0].lanes) {
				lanes.push_back(lane.lane);
				gates.push_back(lane.densityGate);
				expect(lane.pool.empty()).to_be_false();
				expect(lane.expression.ornaments.rateCeiling == 0 && lane.expression.ornaments.rateFloor == 0).to_be_true();
			}
			expect(lanes == std::vector<Lane>{Lane::Kick, Lane::Snare, Lane::Rim, Lane::ClosedHat, Lane::Perc1, Lane::Perc2})
			    .to_be_true();
			expect(gates
			       == std::vector<int16_t>{macroPoints(47), macroPoints(44), macroPoints(40), macroPoints(36),
			                               macroPoints(29), macroPoints(32)})
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
			const Walked walked = walk(harness::movingRecipe(ambientGenre, Scale::Major, seed), seed, 128 * kStepsPerBar);
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
		const harness::Lost lost = harness::lostAtEveryCorner(ambientGenre, ambientGenre.facts.scales);
		expect(lost.dropped).to_equal(0U);
		expect(lost.evicted).to_equal(0U);
	});

	context("its palette (spec §10.1, §18.2, §18.3)", _ {
		it("gives every synth part at least 3 presets, each on the factory card or in the pack, its role listing them", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(harness::fewestPartPresets(ambientGenre)).to_be_greater_than(size_t{2});
			expect(harness::partPresetsMissing(ambientGenre, card).empty()).to_be_true();
			for (const RoleSounds& role : ambientGenre.palette->roles) {
				expect(role.role == Role::Bass || role.role == Role::Pad || role.role == Role::Lead).to_be_true();
			}
		});

		it("rolls the pack's three basses for the Bass, and the pack's drones before the factory ones for the Drone "
		   "(owner, Ambient sounds round 1)", _ {
			FakeCard card = harness::factoryCardWithPack();
			std::set<std::string> basses;
			std::set<std::string> drones;
			for (uint32_t entropy = 0; entropy < 200; entropy++) {
				SoundPicks sounds;
				expect(rollSounds(ambientGenre, *ambientGenre.palette, sounds, entropy, card)).to_be_true();
				basses.insert(sounds.part[kBassPart].c_str());
				drones.insert(sounds.part[kDronePart].c_str());
			}
			expect(basses == std::set<std::string>{"SYNTHS/GENERATIVE/GEN Ambient Bass 2.XML",
			                                       "SYNTHS/GENERATIVE/GEN Ambient Bass 3.XML",
			                                       "SYNTHS/GENERATIVE/GEN Ambient Bass 4.XML"})
			    .to_be_true();
			expect(drones == std::set<std::string>{"SYNTHS/GENERATIVE/GEN Ambient Drone 1.XML",
			                                       "SYNTHS/GENERATIVE/GEN Ambient Drone 2.XML",
			                                       "SYNTHS/GENERATIVE/GEN Ambient Drone 3.XML",
			                                       "SYNTHS/125 Evolving Pad.XML", "SYNTHS/148 Warm 5th Pad.XML"})
			    .to_be_true();
			const auto droneEntries = ambientGenre.palette->partEntries(ambientGenre.parts[kDronePart]);
			expect(std::string(droneEntries.back().path).rfind("SYNTHS/GENERATIVE/", 0)).to_equal(std::string::npos);
		});

		it("rolls the Pad from 131, 151, 157 and 145, not 118 Small Bridge Pad (owner, round 2's I3-118)", _ {
			std::set<std::string> pads;
			for (const PaletteEntry& entry : ambientGenre.palette->partEntries(ambientGenre.parts[kPadPart])) {
				pads.insert(entry.path);
			}
			expect(pads == std::set<std::string>{"SYNTHS/131 Warm Strings.XML", "SYNTHS/151 Radiant FM Pad.XML",
			                                     "SYNTHS/157 Rich FM Pad 4.XML", "SYNTHS/145 Eerie High Pad.XML"})
			    .to_be_true();
		});

		it("falls back to a factory bass, and to a factory drone, on a card without the pack (spec §10.1)", _ {
			FakeLoader loader;
			loader.missing = {"SYNTHS/GENERATIVE/GEN Ambient Bass 2.XML", "SYNTHS/GENERATIVE/GEN Ambient Bass 3.XML",
			                  "SYNTHS/GENERATIVE/GEN Ambient Bass 4.XML", "SYNTHS/GENERATIVE/GEN Ambient Drone 1.XML",
			                  "SYNTHS/GENERATIVE/GEN Ambient Drone 2.XML", "SYNTHS/GENERATIVE/GEN Ambient Drone 3.XML"};
			SoundLoad bass = loadPartSound(*ambientGenre.palette, ambientGenre.parts[kBassPart],
			                               "SYNTHS/GENERATIVE/GEN Ambient Bass 2.XML", loader);
			expect(bass.source == SoundSource::Fallback).to_be_true();
			expect(loader.tried.back()).to_equal(std::string("SYNTHS/002 Basic Square Bass.XML"));
			SoundLoad drone = loadPartSound(*ambientGenre.palette, ambientGenre.parts[kDronePart],
			                                "SYNTHS/GENERATIVE/GEN Ambient Drone 3.XML", loader);
			expect(drone.source == SoundSource::Fallback).to_be_true();
			expect(loader.tried.back()).to_equal(std::string("SYNTHS/125 Evolving Pad.XML"));
		});

		it("finds files for every lane, the pack's brushes alone on the Snare's list and pack risers alone on the Riser", _ {
			FakeCard card = harness::factoryCardWithPack();
			// The lanes' own lists, the kit aside: the kit's machine plays the Snare, the brush only as a swap (spec §10.2).
			auto picks = harness::lanePicks(ambientGenre, card, 600, {}, false);
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
