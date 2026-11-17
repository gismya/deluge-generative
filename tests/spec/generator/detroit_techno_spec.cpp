// Detroit techno (spec §4.5, #220): a Techno subgenre whose Chord graph matters. Home i, 2 bars a chord, no repeats,
// the phrase rule turning the loop round on bars 7–8, every chord with a 7th or a 9th. A root-rhythm bass Loop, a
// Strings Pad, a Stab on syncopated 16ths off the 1, and a Motif lead gated at 30 so it comes in on Drops. Run
// straight through the core.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/palette.h"
#include "generative/core/voicing.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace harness;

namespace {

const Genre& detroit = genres::technoDetroit;

constexpr int32_t kBassPart = 1;
constexpr int32_t kStringsPart = 2;
constexpr int32_t kStabPart = 3;
constexpr int32_t kMotifPart = 4;
constexpr int32_t kFxPart = 5;

constexpr int32_t kChordSteps = 2 * kStepsPerBar;

Recipe detroitRecipe(Scale scale, uint8_t keyRoot = 9, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(detroit, entropy);
	recipe.keyRoot = keyRoot;
	recipe.scale = scale;
	return recipe;
}

/// Complexity and Density corners, with Motion high enough to vary the Arc, in each of Detroit's scales.
std::vector<Recipe> sweep(Scale scale, uint8_t keyRoot = 9) {
	std::vector<Recipe> out;
	for (int32_t complexity : {0, 25, 50}) {
		for (int32_t density : {10, 25, 50}) {
			Recipe recipe = detroitRecipe(scale, keyRoot, static_cast<uint32_t>(complexity * 7 + density + keyRoot));
			recipe.macros.set(Macro::Complexity, macroPoints(complexity));
			recipe.macros.set(Macro::Density, macroPoints(density));
			recipe.macros.set(Macro::Motion, macroPoints(40));
			out.push_back(recipe);
		}
	}
	return out;
}

/// The brief's moves, each written from–to (spec §4.5).
const std::set<std::pair<int32_t, int32_t>> kBriefMoves = {{kI, kVII}, {kI, kIV}, {kI, kV}, {kIV, kV}, {kVI, kVII}};

/// Whether `from` → `to` is a move of the graph, either way: a move back to home is the home pull.
bool graphMove(int32_t from, int32_t to) {
	for (const ChordMove& move : detroit.harmony.graph->moves) {
		if (move.from == from && move.to == to) {
			return true;
		}
	}
	return false;
}

/// The pitch classes of `chord` (scale steps from the tonic) in the key.
std::set<int32_t> classesOf(const Chord& chord, const Recipe& recipe) {
	std::set<int32_t> out;
	for (int32_t i = 0; i < chord.size; i++) {
		out.insert(pitchClass(semitonesOf(chord.steps[i], recipe.scale) + recipe.keyRoot));
	}
	return out;
}

/// The pitch classes of the chord's 7th and 9th over its root, the Extensions a minor-key chord gets first.
std::set<int32_t> seventhAndNinth(int32_t root, const Recipe& recipe) {
	return {pitchClass(semitonesOf(root + 6, recipe.scale) + recipe.keyRoot),
	        pitchClass(semitonesOf(root + 8, recipe.scale) + recipe.keyRoot)};
}

/// Whether `classes` holds the 7th or the 9th of the chord on `root`.
bool extended(const std::set<int32_t>& classes, int32_t root, const Recipe& recipe) {
	const std::set<int32_t> extensions = seventhAndNinth(root, recipe);
	return std::any_of(classes.begin(), classes.end(), [&](int32_t pc) { return extensions.count(pc) != 0; });
}

/// Bar → the Strings' pitch classes sounding in it, over the walk's steps.
std::map<int32_t, std::set<int32_t>> stringsBars(const Walked& walked) {
	const int32_t bars = static_cast<int32_t>(walked.steps.size()) / kStepsPerBar;
	std::map<int32_t, std::set<int32_t>> out;
	for (const Held& note : heldNotes(walked.notes, kStringsPart)) {
		for (int32_t bar = note.on / kBarTicks; bar < bars && bar * kBarTicks < note.off; bar++) {
			out[bar].insert(pitchClass(note.pitch));
		}
	}
	return out;
}

} // namespace

// clang-format off
describe detroit_techno("Detroit techno", $ {
	it("is a Techno subgenre of six parts: Drums, Bass, Strings, Stab, Motif and FX (spec §4.5)", _ {
		expect(std::string(detroit.name)).to_equal(std::string("techno-detroit"));
		expect(std::string(detroit.oledName)).to_equal(std::string("DETROIT TECHNO"));
		expect(std::string(detroit.shortName)).to_equal(std::string("DETROIT"));
		expect(detroit.parent == genres::technoAcid.parent).to_be_true();
		expect(findGenre("techno-detroit") == &detroit).to_be_true();
		expect(partCount(detroit)).to_equal(6);
		expect(technoRosterValid(detroit)).to_be_true();
		const char* names[] = {"DRUMS", "BASS", "STRINGS", "STAB", "MOTIF", "FX"};
		const Role roles[] = {Role::Drums, Role::Bass, Role::Pad, Role::Chords, Role::Lead, Role::FX};
		const Flavour flavours[] = {Flavour::None, Flavour::Loop, Flavour::Sustain, Flavour::Stab, Flavour::Motif,
		                            Flavour::None};
		for (int32_t p = 0; p < 6; p++) {
			expect(std::string(detroit.parts[p].name)).to_equal(std::string(names[p]));
			expect(detroit.parts[p].role == roles[p]).to_be_true();
			expect(detroit.parts[p].flavour == flavours[p]).to_be_true();
		}
		expect(detroit.facts.tempoLowBpm).to_equal(125);
		expect(detroit.facts.tempoHighBpm).to_equal(135);
		std::set<Scale> scales(detroit.facts.scales.begin(), detroit.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Dorian}).to_be_true();
		expect(detroit.harmony.extensionFloor).to_equal(1);
		expect(detroit.harmony.extensionCeiling).to_equal(2);
		expect(motifNotesValid(detroit)).to_be_true();
		expect(stabNotesValid(detroit)).to_be_true();
		expect(loopNotesValid(detroit)).to_be_true();
		expect(harmonyValid(detroit)).to_be_true();
	});

	it("takes the Techno parent's Drums, fills, Arc and FX part", _ {
		const Genre& parent = *detroit.parent;
		expect(detroit.parts[0].lanes.data() == parent.parts.front().lanes.data()).to_be_true();
		expect(detroit.parts[kFxPart].lanes.data() == parent.parts.back().lanes.data()).to_be_true();
		expect(detroit.fills.data() == parent.fills.data()).to_be_true();
		expect(detroit.fillChance).to_equal(parent.fillChance);
		expect(detroit.arc.groovePhrases.low).to_equal(parent.arc.groovePhrases.low);
		expect(detroit.arc.buildBars.data() == parent.arc.buildBars.data()).to_be_true();
		expect(detroit.arc.roll.lane == parent.arc.roll.lane).to_be_true();
		expect(detroit.arc.accentLane == parent.arc.accentLane).to_be_true();
	});

	context("its Chord graph (spec §4.5, §7.4)", _ {
		it("is home i with the brief's moves, 2 bars a chord, no repeats and no cadence target", _ {
			const ChordGraph& graph = *detroit.harmony.graph;
			expect(graph.barsPerChord).to_equal(2);
			expect(graph.repeatWeight).to_equal(0);
			expect(graph.homePull).to_be_greater_than(0);
			expect(graph.cadenceTarget).to_equal(kNoCadenceTarget);
			for (const auto& [from, to] : kBriefMoves) {
				expect(graphMove(from, to)).to_be_true();
			}
			// Every chord the graph reaches has a way on that isn't home, so a phrase's last chord, which excludes
			// home, never stalls into a repeat.
			for (const ChordMove& move : graph.moves) {
				expect(kBriefMoves.count({move.from, move.to}) + kBriefMoves.count({move.to, move.from})).to_be_greater_than(size_t{0});
				bool onward = false;
				for (const ChordMove& next : graph.moves) {
					onward = onward || (next.from == move.to && next.to != kHomeChord);
				}
				expect(onward).to_be_true();
			}
		});

		it("changes chord on 2-bar lines through its moves or home, Dorian's i–IV too, and hears every move", _ {
			std::map<Scale, std::set<std::pair<int32_t, int32_t>>> heard;
			for (Scale scale : detroit.facts.scales) {
				for (const Recipe& recipe : sweep(scale)) {
					for (uint32_t seed = 1; seed <= 3; seed++) {
						const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
						expect(walked.steps[0].chord).to_equal(kHomeChord);
						for (int32_t n : walked.changes()) {
							const Walked::Step& step = walked.steps[n];
							const int32_t from = walked.steps[n - 1].chord;
							expect(step.stepInPhrase % kChordSteps == 0
							       || (step.sectionStart && step.stepInPhrase % kStepsPerBar == 0))
							    .to_be_true();
							const bool colour = scale == Scale::Dorian && from == kI && step.chord == kIV;
							expect(graphMove(from, step.chord) || step.chord == kHomeChord || colour).to_be_true();
							heard[scale].insert({from, step.chord});
						}
					}
				}
			}
			for (const ChordMove& move : detroit.harmony.graph->moves) {
				expect(heard[Scale::Minor].count({move.from, move.to})).to_equal(size_t{1});
			}
			expect(heard[Scale::Dorian].count({kI, kIV})).to_equal(size_t{1});
		});

		it("never repeats a chord on a chord line, but into a Drop, which is always home", _ {
			int32_t lines = 0;
			int32_t drops = 0;
			for (Scale scale : detroit.facts.scales) {
				for (const Recipe& recipe : sweep(scale)) {
					for (uint32_t seed = 1; seed <= 3; seed++) {
						const Walked walked = walk(recipe, seed, 128 * kStepsPerBar);
						for (size_t n = 1; n < walked.steps.size(); n++) {
							const Walked::Step& step = walked.steps[n];
							if (step.drop) {
								drops++;
								expect(step.chord).to_equal(kHomeChord);
								continue;
							}
							// A chord line on the 2-bar grid, inside a Section.
							if (step.stepInPhrase % kChordSteps != 0 || step.sectionStart) {
								continue;
							}
							lines++;
							expect(step.chord != walked.steps[n - 1].chord).to_be_true();
						}
					}
				}
			}
			expect(lines).to_be_greater_than(2000);
			expect(drops).to_be_greater_than(20);
		});

		it("keeps home off each phrase's last chord, so bars 7–8 always turn the loop round", _ {
			int32_t turns = 0;
			for (Scale scale : detroit.facts.scales) {
				for (const Recipe& recipe : sweep(scale)) {
					for (uint32_t seed = 1; seed <= 3; seed++) {
						for (const Walked::Step& step : walk(recipe, seed, 128 * kStepsPerBar).steps) {
							if (step.stepInPhrase / kStepsPerBar >= 6) {
								turns++;
								expect(step.chord != kHomeChord).to_be_true();
							}
						}
					}
				}
			}
			expect(turns).to_be_greater_than(10000);
		});
	});

	it("voices its home chord with an Extension from the start, before its first step is decided", _ {
		for (int32_t complexity : {0, 25, 50}) {
			Recipe recipe = detroitRecipe(Scale::Minor);
			recipe.macros.set(Macro::Complexity, macroPoints(complexity));
			Generator generator;
			generator.start(recipe, 1, Takes{}, false);
			const Chord before = generator.chord();
			expect(before.size).to_equal(uint8_t{4});
			expect(extended(classesOf(before, recipe), kHomeChord, recipe)).to_be_true();
			SteeringQueue queue;
			generator.decideNextStep(queue, nullptr);
			const Chord first = generator.chord();
			expect(std::equal(std::begin(before.steps), std::end(before.steps), std::begin(first.steps))).to_be_true();
		}
	});

	it("gives every chord the Stab and the Strings voice at least one Extension: a 7th or a 9th (Done-when, spec §7.4)", _ {
		int32_t stabs = 0;
		int32_t strings = 0;
		std::set<int32_t> stabSizes;
		for (Scale scale : detroit.facts.scales) {
			for (uint8_t keyRoot : {0, 5, 9}) {
				for (const Recipe& recipe : sweep(scale, keyRoot)) {
					const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 1, 2);
					expect(level == 1 || level == 2).to_be_true();
					const Walked walked = walk(recipe, 1, 64 * kStepsPerBar);
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
					std::map<int32_t, std::set<int32_t>> stabAt;    // tick → the Stab's pitch classes starting there
					std::map<int32_t, int32_t> notesAt;
					for (const Event& e : walked.noteOns) {
						if (e.part == kStabPart) {
							stabAt[e.tick].insert(pitchClass(e.pitch));
							notesAt[e.tick]++;
						}
					}
					for (const auto& [tick, classes] : stabAt) {
						const int32_t step = std::min<int32_t>(tick / kTicksPerStep, static_cast<int32_t>(walked.steps.size()) - 1);
						const int32_t root = walked.steps[step].chord;
						const Chord voiced = voiceChord(recipe.scale, root, level);
						stabs++;
						stabSizes.insert(notesAt[tick]);
						expect(classes == classesOf(voiced, recipe)).to_be_true();
						expect(extended(classes, root, recipe)).to_be_true();
					}
					// The Extension is an anchor of the Strings' voicing: every bar they sound has it, at any Density.
					for (const auto& [bar, classes] : stringsBars(walked)) {
						const Walked::Step& step = walked.steps[bar * kStepsPerBar];
						strings++;
						expect(extended(classes, step.chord, recipe)).to_be_true();
					}
				}
			}
		}
		expect(stabs).to_be_greater_than(3000);
		expect(strings).to_be_greater_than(500);
		expect(stabSizes.count(4)).to_equal(size_t{1});
	});

	it("sounds an Extension in every bar its Strings play, whatever the seed and the take (Done-when, spec §5.2)", _ {
		// The Extension's place is an anchor of the Strings' voicing: no jitter draw, which is keyed on the seed, the
		// take and the place and fixed for the piece, thins it out. So sweep the draws, at the default Density and
		// above, in the Break's dip and under the default, where the Intro swells.
		struct Corner {
			Scale scale;
			int32_t density;
			int32_t complexity;
		};
		constexpr Corner kCorners[] = {{Scale::Minor, 25, 0},  {Scale::Dorian, 25, 50}, {Scale::Minor, 35, 25},
		                               {Scale::Dorian, 50, 0}, {Scale::Minor, 10, 50},  {Scale::Dorian, 0, 25}};
		constexpr int32_t kBars = 128;
		std::map<SectionKind, int32_t> bars;
		int32_t failed = 0;
		uint32_t seed = 0;
		for (const Corner& corner : kCorners) {
			Recipe recipe = detroitRecipe(corner.scale, 9, static_cast<uint32_t>(corner.density * 3 + corner.complexity));
			recipe.macros.set(Macro::Density, macroPoints(corner.density));
			recipe.macros.set(Macro::Complexity, macroPoints(corner.complexity));
			recipe.macros.set(Macro::Motion, macroPoints(40));
			for (int32_t piece = 0; piece < 6; piece++) {
				seed += 7919; // a fresh seed for each piece of each corner: 36 seeds, 144 jitter draws in all
				for (uint8_t take = 0; take < 4; take++) {
					Takes takes;
					takes.take[kStringsPart] = take;
					const Walked walked = walk(recipe, seed, kBars * kStepsPerBar, {}, false, takes);
					for (const auto& [bar, classes] : stringsBars(walked)) {
						const Walked::Step& step = walked.steps[bar * kStepsPerBar];
						bars[step.section]++;
						failed += extended(classes, step.chord, recipe) ? 0 : 1;
					}
				}
			}
		}
		expect(failed).to_equal(0);
		expect(bars[SectionKind::Groove]).to_be_greater_than(8000);
		expect(bars[SectionKind::Break]).to_be_greater_than(2000);
		expect(bars[SectionKind::Build]).to_be_greater_than(1500);
		expect(bars[SectionKind::Intro]).to_be_greater_than(2000);
	});

	context("its Bass (spec §4.5, §5.2)", _ {
		it("takes the Bass Register's default, the tonic within 6 semitones of C2 (spec §10.6: heard pitch)", _ {
			const Register reg = detroit.parts[kBassPart].registerOrDefault();
			const Register fallback = defaultRegister(Role::Bass);
			expect(static_cast<int32_t>(reg.anchor)).to_equal(36);
			expect(static_cast<int32_t>(reg.anchor)).to_equal(static_cast<int32_t>(fallback.anchor));
			expect(static_cast<int32_t>(reg.spanLow)).to_equal(static_cast<int32_t>(fallback.spanLow));
			expect(static_cast<int32_t>(reg.spanHigh)).to_equal(static_cast<int32_t>(fallback.spanHigh));
		});

		it("is a Loop on root rhythm: the beats and the & before the next, a 16th pickup with Complexity", _ {
			const PartSpec& bass = detroit.parts[kBassPart];
			for (const Pattern& pattern : bass.pool) {
				int32_t beats = 0;
				for (int32_t i = 0; i < pattern.length; i++) {
					if (pattern.importance[i] == 0) {
						continue;
					}
					const int32_t inBeat = i % kStepsPerBeat;
					beats += inBeat == 0 ? 1 : 0;
					expect(inBeat == 0 || inBeat == 2 || (inBeat == 3 && pattern.tier > 0)).to_be_true();
					if (inBeat == 2) {
						expect(pattern.importance[i - 2]).to_be_greater_than(uint8_t{0});
					}
				}
				expect(beats * 4).to_be_greater_than(pattern.length * 3 / 4 - 1); // most beats
				expect(pattern.importance[0]).to_equal(kAnchor);
			}
		});

		it("plays the chord root alone at Complexity 0, and its octave and 5th as Complexity rises", _ {
			std::map<int32_t, std::map<int32_t, int32_t>> intervals; // Complexity → interval over the root → notes
			for (int32_t complexity : {0, 40, 50}) {
				for (uint32_t seed = 1; seed <= 4; seed++) {
					Recipe recipe = detroitRecipe(Scale::Minor);
					recipe.macros.set(Macro::Complexity, macroPoints(complexity));
					const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
					const Register reg = detroit.parts[kBassPart].registerOrDefault();
					const int32_t tonic = tonicNote(reg, recipe.keyRoot);
					for (const Event& e : walked.noteOns) {
						if (e.part != kBassPart) {
							continue;
						}
						const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
						const int32_t root = pitchClass(semitonesOf(walked.steps[step].chord, recipe.scale) + recipe.keyRoot);
						expect(e.pitch >= tonic + reg.spanLow && e.pitch <= tonic + reg.spanHigh).to_be_true();
						const int32_t pc = pitchClass(e.pitch - root);
						const int32_t octaveUp = e.pitch - tonic >= 12 ? 12 : 0;
						intervals[complexity][pc + octaveUp]++;
					}
				}
			}
			for (const auto& [interval, notes] : intervals[0]) {
				expect(interval == 0 || interval == 12).to_be_true();
			}
			expect(intervals[0][0]).to_be_greater_than(500);
			expect(intervals[40][7] + intervals[40][19]).to_be_greater_than(20); // from Complexity 34
			expect(intervals[50][7] + intervals[50][19]).to_be_greater_than(20);
			int32_t octaves = 0;
			for (const auto& [interval, notes] : intervals[50]) {
				octaves += interval >= 12 ? notes : 0;
			}
			expect(octaves).to_be_greater_than(20);
		});
	});

	context("its Strings (spec §4.5, §5.2)", _ {
		it("are a Sustain Pad voicing each chord, not gated, with an 8-bar Brightness Sweep", _ {
			const PartSpec& strings = detroit.parts[kStringsPart];
			expect(strings.sustain.pedal).to_be_false();
			expect(strings.densityGate).to_equal(0);
			expect(strings.drift.sweepBars).to_equal(8);
		});

		it("play only the chord's notes, re-struck with each chord, never more than 4 at once", _ {
			int32_t notes = 0;
			for (const Recipe& recipe : sweep(Scale::Minor)) {
				const Walked walked = walk(recipe, 2, 64 * kStepsPerBar);
				const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 1, 2);
				std::map<int32_t, int32_t> sounding;
				for (const Held& note : heldNotes(walked.notes, kStringsPart)) {
					notes++;
					const int32_t step = note.on / kTicksPerStep;
					const std::set<int32_t> chord = classesOf(voiceChord(recipe.scale, walked.steps[step].chord, level), recipe);
					expect(chord.count(pitchClass(note.pitch))).to_equal(size_t{1});
					for (int32_t tick = note.on; tick < std::min(note.off, kBarTicks * 64); tick += kTicksPerStep) {
						expect(++sounding[tick]).to_be_less_than(5);
					}
				}
			}
			expect(notes).to_be_greater_than(500);
		});
	});

	context("its Stab (spec §4.5, §5.2)", _ {
		it("plays syncopated 16ths that keep off the 1, each pattern with an anchor for the Break", _ {
			const PartSpec& stab = detroit.parts[kStabPart];
			expect(stab.densityGate).to_equal(0);
			for (const Pattern& pattern : stab.pool) {
				bool anchor = false;
				bool sixteenth = false;
				for (int32_t i = 0; i < pattern.length; i++) {
					if (pattern.importance[i] == 0) {
						continue;
					}
					expect(i % kStepsPerBar != 0).to_be_true();
					anchor = anchor || pattern.importance[i] == kAnchor;
					sixteenth = sixteenth || i % 2 == 1;
				}
				expect(anchor).to_be_true();
				expect(sixteenth).to_be_true();
			}
		});

		it("never stabs on the 1, across whole pieces", _ {
			int32_t stabs = 0;
			for (const Recipe& recipe : sweep(Scale::Dorian)) {
				const Walked walked = walk(recipe, 3, 96 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part == kStabPart) {
						stabs++;
						expect(stepOf(e) % kStepsPerBar != 0).to_be_true();
					}
				}
			}
			expect(stabs).to_be_greater_than(1000);
		});
	});

	context("its Motif (spec §4.5, §5.2, §5.6)", _ {
		it("is a Lead Motif of one- and two-bar riffs, gated at 30", _ {
			const PartSpec& motif = detroit.parts[kMotifPart];
			expect(motif.densityGate).to_equal(macroPoints(30));
			bool one = false;
			bool two = false;
			for (const Pattern& pattern : motif.pool) {
				one = one || pattern.length == kStepsPerBar;
				two = two || pattern.length == 2 * kStepsPerBar;
			}
			expect(one && two).to_be_true();
		});

		it("is silent in a Groove at the default macros, and plays from each Drop (Done-when)", _ {
			int32_t drops = 0;
			int32_t heardDrops = 0;
			int32_t grooveSteps = 0;
			for (Scale scale : detroit.facts.scales) {
				for (uint32_t seed = 1; seed <= 10; seed++) {
					const Walked walked = walk(detroitRecipe(scale, static_cast<uint8_t>(seed % 12), seed), seed, 160 * kStepsPerBar);
					std::vector<int32_t> motifSteps;
					for (const Event& e : walked.noteOns) {
						if (e.part == kMotifPart) {
							motifSteps.push_back(std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1));
						}
					}
					// Each step's Section, and whether its Groove opened with a Drop.
					std::vector<bool> fromDrop(walked.steps.size());
					bool dropped = false;
					for (size_t n = 0; n < walked.steps.size(); n++) {
						const Walked::Step& step = walked.steps[n];
						dropped = step.sectionStart ? step.drop : dropped;
						fromDrop[n] = dropped && step.section == SectionKind::Groove;
						grooveSteps += !fromDrop[n] && step.section == SectionKind::Groove ? 1 : 0;
						if (step.drop) {
							drops++;
							const bool heard = std::any_of(motifSteps.begin(), motifSteps.end(), [&](int32_t s) {
								return s >= static_cast<int32_t>(n) && s < static_cast<int32_t>(n) + kStepsPerBar;
							});
							heardDrops += heard ? 1 : 0;
						}
					}
					for (int32_t s : motifSteps) {
						expect(static_cast<bool>(fromDrop[s])).to_be_true();
					}
				}
			}
			expect(grooveSteps).to_be_greater_than(10000);
			expect(drops).to_be_greater_than(10);
			expect(heardDrops).to_equal(drops);
		});

		it("plays only the playing chord's notes, in its Register, one note at a time", _ {
			int32_t notes = 0;
			for (const Recipe& recipe : sweep(Scale::Minor)) {
				const Walked walked = walk(recipe, 4, 96 * kStepsPerBar);
				const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 1, 2);
				const Register reg = detroit.parts[kMotifPart].registerOrDefault();
				const int32_t tonic = tonicNote(reg, recipe.keyRoot);
				const std::vector<Held> held = heldNotes(walked.notes, kMotifPart);
				for (size_t i = 0; i < held.size(); i++) {
					notes++;
					const int32_t step = std::min<int32_t>((held[i].on + kTicksPerStep / 2) / kTicksPerStep, static_cast<int32_t>(walked.steps.size()) - 1);
					const std::set<int32_t> chord = classesOf(voiceChord(recipe.scale, walked.steps[step].chord, level), recipe);
					expect(chord.count(pitchClass(held[i].pitch))).to_equal(size_t{1});
					expect(held[i].pitch >= tonic + reg.spanLow && held[i].pitch <= tonic + reg.spanHigh).to_be_true();
					if (i + 1 < held.size()) {
						expect(held[i].off).to_be_less_than(held[i + 1].on + 1);
					}
				}
			}
			expect(notes).to_be_greater_than(500);
		});
	});

	it("keeps only the Strings, the Stab and the Closed hat in a Break, with the FX part's section rules, and plays all three", _ {
		std::map<int32_t, int32_t> kept; // part (the Closed hat as -1) → note-ons in Breaks
		int32_t breaks = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked = walk(detroitRecipe(Scale::Minor), seed, 96 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break) {
					continue;
				}
				const bool hat = e.part == 0 && e.lane == Lane::ClosedHat;
				const bool ok = hat || e.part == kStringsPart || e.part == kStabPart || e.part == kFxPart;
				expect(ok).to_be_true();
				kept[hat ? -1 : e.part]++;
				breaks += walked.steps[step].sectionStart ? 0 : 1;
			}
		}
		expect(breaks).to_be_greater_than(100);
		expect(kept[kStringsPart]).to_be_greater_than(0);
		expect(kept[kStabPart]).to_be_greater_than(0);
		expect(kept[-1]).to_be_greater_than(0);
	});

	it("never drops an event or evicts a note across whole pieces at every corner", _ {
		for (Scale scale : detroit.facts.scales) {
			for (const Recipe& recipe : sweep(scale, 4)) {
				for (uint32_t seed = 1; seed <= 2; seed++) {
					const Walked walked = walk(recipe, seed, 128 * kStepsPerBar);
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
				}
			}
		}
	});

	context("its palette (spec §10.1, §18.3)", _ {
		it("gives the Bass, the Strings, the Stab and the Motif at least 3 presets each, on the factory card", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(detroit.palette != nullptr && detroit.palette != genres::technoAcid.palette).to_be_true();
			int32_t synthParts = 0;
			for (const PartSpec& part : detroit.parts) {
				if (!part.lanes.empty()) {
					continue;
				}
				synthParts++;
				auto entries = detroit.palette->partEntries(part);
				expect(entries.size()).to_be_greater_than(size_t{2});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
				}
			}
			expect(synthParts).to_equal(4);
			// Each synth part's role lists the part's presets, so an override file's role reaches it ({Role::Lead, Motif}
			// among them).
			for (int32_t p = kBassPart; p <= kMotifPart; p++) {
				const PartSpec& part = detroit.parts[p];
				bool listed = false;
				for (const RoleSounds& role : detroit.palette->roles) {
					listed = listed || (role.role == part.role && role.entries.data() == detroit.palette->partEntries(part).data());
				}
				expect(listed).to_be_true();
			}
		});

		it("shares Techno's drum and FX lanes", _ {
			expect(detroit.palette->lanes.data() == genres::technoAcid.palette->lanes.data()).to_be_true();
		});
	});
});

CPPSPEC_SPEC(detroit_techno)
