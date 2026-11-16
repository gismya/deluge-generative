// Dub techno (spec §4.5, #221): a Techno subgenre with an empty Harmony slot and Extension floor 1, so one m7 or m9
// chord throughout. A Stab plays it on the & and the 16th after, with ghosted repeats, over a sub bass Loop of long
// root notes and a pedal Pad, with lighter drums and a shaker. Run straight through the core.

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
#include <vector>

using namespace harness;

namespace {

const Genre& dub = genres::technoDub;

constexpr int32_t kSubPart = 1;
constexpr int32_t kStabPart = 2;
constexpr int32_t kPadPart = 3;
constexpr int32_t kFxPart = 4;

Recipe dubRecipe(Scale scale, uint8_t keyRoot = 9, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(dub, entropy);
	recipe.keyRoot = keyRoot;
	recipe.scale = scale;
	return recipe;
}

/// Complexity and Density corners, with Motion high enough to vary the Arc, in each of Dub's scales and three keys.
std::vector<Recipe> sweep() {
	std::vector<Recipe> out;
	for (Scale scale : dub.facts.scales) {
		for (uint8_t keyRoot : {0, 5, 9}) {
			for (int32_t complexity : {0, 25, 50}) {
				for (int32_t density : {10, 25, 50}) {
					Recipe recipe =
					    dubRecipe(scale, keyRoot, static_cast<uint32_t>(complexity * 7 + density + keyRoot));
					recipe.macros.set(Macro::Complexity, macroPoints(complexity));
					recipe.macros.set(Macro::Density, macroPoints(density));
					recipe.macros.set(Macro::Motion, macroPoints(40));
					out.push_back(recipe);
				}
			}
		}
	}
	return out;
}

int32_t pitchClass(int32_t pitch) {
	return ((pitch % 12) + 12) % 12;
}

} // namespace

// clang-format off
describe dub_techno("Dub techno", $ {
	it("is a Techno subgenre of five parts: Drums, Sub, Stab, Pad and FX (spec §4.5)", _ {
		expect(std::string(dub.name)).to_equal(std::string("techno-dub"));
		expect(std::string(dub.oledName)).to_equal(std::string("DUB TECHNO"));
		expect(std::string(dub.shortName)).to_equal(std::string("DUB"));
		expect(dub.parent == genres::technoAcid.parent).to_be_true();
		expect(findGenre("techno-dub") == &dub).to_be_true();
		expect(partCount(dub)).to_equal(5);
		expect(technoRosterValid(dub)).to_be_true();
		expect(std::string(dub.parts[kSubPart].name)).to_equal(std::string("SUB"));
		expect(dub.parts[kStabPart].role == Role::Chords).to_be_true();
		expect(dub.parts[kStabPart].flavour == Flavour::Stab).to_be_true();
		expect(dub.parts[kPadPart].role == Role::Pad).to_be_true();
		expect(dub.parts[kFxPart].role == Role::FX).to_be_true();
		expect(dub.facts.tempoLowBpm).to_equal(115);
		expect(dub.facts.tempoHighBpm).to_equal(125);
		std::set<Scale> scales(dub.facts.scales.begin(), dub.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Dorian}).to_be_true();
		// An empty Harmony slot, every chord with one or two Extensions.
		expect(dub.harmony.graph == nullptr).to_be_true();
		expect(dub.harmony.extensionFloor).to_equal(1);
		expect(dub.harmony.extensionCeiling).to_equal(2);
	});

	it("takes the Techno parent's fills, Arc and FX part", _ {
		const Genre& parent = *dub.parent;
		expect(dub.fills.data() == parent.fills.data()).to_be_true();
		expect(dub.fillChance).to_equal(parent.fillChance);
		expect(dub.arc.groovePhrases.low).to_equal(parent.arc.groovePhrases.low);
		expect(dub.arc.buildBars.data() == parent.arc.buildBars.data()).to_be_true();
		expect(dub.arc.roll.lane == parent.arc.roll.lane).to_be_true();
		expect(dub.arc.accentLane == parent.arc.accentLane).to_be_true();
		expect(dub.parts[kFxPart].lanes.data() == parent.parts.back().lanes.data()).to_be_true();
		expect(dub.parts[0].gestures.off).to_equal(parent.parts[0].gestures.off);
	});

	context("its drums (spec §4.5)", _ {
		it("are lighter: kick, rim, hats and a shaker on Perc 2, with the parent's snare, crash and tom for its fills, roll and accent", _ {
			std::vector<Lane> lanes;
			for (const LaneSpec& lane : dub.parts[0].lanes) {
				lanes.push_back(lane.lane);
			}
			expect(lanes == std::vector<Lane>{Lane::Kick, Lane::Snare, Lane::Rim, Lane::ClosedHat, Lane::OpenHat,
			                                  Lane::Crash, Lane::Tom, Lane::Perc2})
			    .to_be_true();
			const std::span<const LaneSpec> parentDrums = dub.parent->parts[0].lanes;
			for (const LaneSpec& lane : dub.parts[0].lanes) {
				if (lane.lane == Lane::Snare || lane.lane == Lane::Tom) {
					expect(lane.pool.empty()).to_be_true(); // only a Swap and the Build roll play them
					bool sharesFills = false;
					for (const LaneSpec& theirs : parentDrums) {
						sharesFills = sharesFills || (theirs.lane == lane.lane && theirs.fillPool.data() == lane.fillPool.data());
					}
					expect(sharesFills).to_be_true();
				}
				if (lane.lane == Lane::Kick) {
					// Four on the floor at every Complexity.
					for (const Pattern& pattern : lane.pool) {
						for (int32_t i = 0; i < pattern.length; i++) {
							expect(pattern.importance[i]).to_equal(i % kStepsPerBeat == 0 ? kAnchor : 0);
						}
					}
				}
				if (lane.lane == Lane::Perc2 || lane.lane == Lane::Rim) {
					expect(lane.pool.empty()).to_be_false();
				}
			}
		});

		it("play the shaker in a Groove at the default Density", _ {
			int32_t shakes = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Walked walked = walk(dubRecipe(Scale::Minor), seed, 32 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					shakes += e.part == 0 && e.lane == Lane::Perc2 ? 1 : 0;
				}
			}
			expect(shakes).to_be_greater_than(100);
		});
	});

	context("its Stab (spec §4.5, §5.2, §7.4)", _ {
		it("plays on the & and the 16th after it, the & soft and the 16ths after it and the repeats ghosted", _ {
			const PartSpec& stab = dub.parts[kStabPart];
			expect(stab.densityGate).to_equal(0);
			for (const Pattern& pattern : stab.pool) {
				bool pair = false;
				for (int32_t i = 0; i < pattern.length; i++) {
					if (pattern.importance[i] == 0) {
						continue;
					}
					const int32_t inBeat = i % kStepsPerBeat;
					expect(inBeat == 2 || inBeat == 3).to_be_true();
					expect(pattern.level[i] != VelocityLevel::Accent).to_be_true();
					if (inBeat == 3) {
						expect(pattern.level[i] == VelocityLevel::Ghost).to_be_true();
					}
					pair = pair || (inBeat == 2 && pattern.importance[i] == kAnchor
					                && pattern.importance[(i + 1) % pattern.length] > 0);
				}
				expect(pair).to_be_true(); // an anchored & with the 16th after it
			}
		});

		it("voices the home chord with one or two Extensions throughout: m7 or m9 (Done-when, spec §7.4)", _ {
			int32_t stabs = 0;
			std::set<int32_t> sizes;
			std::set<std::set<int32_t>> shapes;
			for (const Recipe& recipe : sweep()) {
				const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], dub.harmony.extensionFloor,
				                                     dub.harmony.extensionCeiling);
				expect(level == 1 || level == 2).to_be_true();
				const Chord home = voiceChord(recipe.scale, 0, level);
				std::set<int32_t> expected;
				for (int32_t i = 0; i < home.size; i++) {
					expected.insert(pitchClass(semitonesOf(home.steps[i], recipe.scale) + recipe.keyRoot));
				}
				// i7 (R ♭3 5 ♭7) at level 1, i9 without its 5th (R ♭3 ♭7 9) at level 2, in Minor and Dorian alike.
				const int32_t r = recipe.keyRoot;
				const std::set<int32_t> m7{r % 12, (r + 3) % 12, (r + 7) % 12, (r + 10) % 12};
				const std::set<int32_t> m9{r % 12, (r + 3) % 12, (r + 10) % 12, (r + 2) % 12};
				expect(expected == (level == 1 ? m7 : m9)).to_be_true();
				for (uint32_t seed = 1; seed <= 2; seed++) {
					const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
					// "Throughout": the walk crosses a Break (bar 56 here) and a Drop (bar 80 or 88).
					bool crossedBreak = false;
					bool crossedDrop = false;
					for (const Walked::Step& step : walked.steps) {
						crossedBreak = crossedBreak || step.section == SectionKind::Break;
						crossedDrop = crossedDrop || step.drop;
					}
					expect(crossedBreak).to_be_true();
					expect(crossedDrop).to_be_true();
					std::map<int32_t, std::set<int32_t>> chordAt; // tick → the Stab's pitch classes starting there
					std::map<int32_t, int32_t> notesAt;
					for (const Event& e : walked.noteOns) {
						if (e.part == kStabPart) {
							chordAt[e.tick].insert(pitchClass(e.pitch));
							notesAt[e.tick]++;
						}
					}
					for (const auto& [tick, classes] : chordAt) {
						stabs++;
						sizes.insert(notesAt[tick]);
						shapes.insert(classes);
						expect(classes == expected).to_be_true();
					}
				}
			}
			expect(stabs).to_be_greater_than(5000);
			expect(sizes == std::set<int32_t>{4}).to_be_true();
			expect(shapes.size()).to_be_greater_than(size_t{3}); // m7 and m9, in three keys
		});

		it("keeps voicing m7 or m9 across a steered change from Minor to Dorian, which shares its home chord", _ {
			const Recipe recipe = dubRecipe(Scale::Minor);
			const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], dub.harmony.extensionFloor,
			                                     dub.harmony.extensionCeiling);
			const Walked walked = walk(recipe, 1, 24 * kStepsPerBar, [](int32_t n, const Generator&, SteeringQueue& queue) {
				if (n == 20) {
					queue.push({CommandKind::SetKey, 9, static_cast<int32_t>(Scale::Dorian)});
				}
			});
			expect(walked.dropped).to_equal(0U);
			expect(walked.evicted).to_equal(0U);
			expect(walked.generator->recipe().scale).to_equal(Scale::Dorian);
			std::set<int32_t> minorChord;
			std::set<int32_t> dorianChord;
			for (int32_t i = 0; i < voiceChord(Scale::Minor, 0, level).size; i++) {
				minorChord.insert(pitchClass(semitonesOf(voiceChord(Scale::Minor, 0, level).steps[i], Scale::Minor) + 9));
				dorianChord.insert(pitchClass(semitonesOf(voiceChord(Scale::Dorian, 0, level).steps[i], Scale::Dorian) + 9));
			}
			expect(minorChord == dorianChord).to_be_true();
			// The change lands on the next phrase line; the Stab plays the same chord either side of it.
			const int32_t line = dub.phraseSteps() * kTicksPerStep;
			std::map<int32_t, std::set<int32_t>> chordAt;
			for (const Event& e : walked.noteOns) {
				if (e.part == kStabPart) {
					chordAt[e.tick].insert(pitchClass(e.pitch));
				}
			}
			int32_t before = 0;
			int32_t after = 0;
			for (const auto& [tick, classes] : chordAt) {
				expect(classes == minorChord).to_be_true();
				(tick < line ? before : after)++;
			}
			expect(before).to_be_greater_than(0);
			expect(after).to_be_greater_than(0);
		});

		it("plays its ghosted 16th after the & at the default Density", _ {
			int32_t pairs = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Walked walked = walk(dubRecipe(Scale::Minor), seed, 32 * kStepsPerBar);
				std::set<int32_t> steps;
				for (const Event& e : walked.noteOns) {
					if (e.part == kStabPart) {
						steps.insert(stepOf(e));
					}
				}
				for (int32_t step : steps) {
					pairs += step % kStepsPerBeat == 2 && steps.count(step + 1) ? 1 : 0;
				}
			}
			expect(pairs).to_be_greater_than(30);
		});
	});

	context("its Sub (spec §4.5, §5.2)", _ {
		it("is a Bass Loop of roots, now and then an octave down, with no slides and notes longer than a step", _ {
			const PartSpec& sub = dub.parts[kSubPart];
			expect(sub.role == Role::Bass).to_be_true();
			expect(sub.flavour == Flavour::Loop).to_be_true();
			expect(sub.loop.slide.atZero).to_equal(0);
			expect(sub.loop.slide.atFull).to_equal(0);
			expect(sub.loop.draws.octaveUp.atZero).to_equal(0);
			expect(sub.loop.draws.octaveUp.atFull).to_equal(0);
			expect(sub.loop.draws.octaveDown.atFull).to_be_greater_than(int16_t{0});
			for (const LineWeight& degree : sub.loop.degrees) {
				expect(degree.value).to_equal(0);
			}
			expect(sub.loop.gateTicks).to_be_greater_than(uint16_t{2 * kTicksPerStep});
			expect(loopNotesValid(dub)).to_be_true();
		});

		it("plays the root alone, one long note at a time, and an octave lower now and then", _ {
			int32_t notes = 0;
			int32_t longNotes = 0;
			int32_t lower = 0;
			for (const Recipe& recipe : sweep()) {
				const Register reg = dub.parts[kSubPart].registerOrDefault();
				const int32_t tonic = tonicNote(reg, recipe.keyRoot);
				for (uint32_t seed = 1; seed <= 2; seed++) {
					const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
					const std::vector<Held> held = heldNotes(walked.notes, kSubPart);
					for (size_t i = 0; i < held.size(); i++) {
						const Held& note = held[i];
						notes++;
						expect(pitchClass(note.pitch)).to_equal(static_cast<int32_t>(recipe.keyRoot));
						expect(note.pitch >= tonic + reg.spanLow && note.pitch <= tonic + reg.spanHigh).to_be_true();
						longNotes += note.off - note.on > 2 * kTicksPerStep ? 1 : 0;
						lower += note.pitch == tonic - 12 ? 1 : 0;
						if (i + 1 < held.size()) {
							expect(note.off).to_be_less_than(held[i + 1].on + 1); // no slide, no overlap
						}
					}
				}
			}
			expect(notes).to_be_greater_than(2000);
			expect(longNotes * 2).to_be_greater_than(notes);
			expect(lower).to_be_greater_than(50);
			expect(lower * 4).to_be_less_than(notes);
		});
	});

	context("its Pad (spec §4.5, §5.2)", _ {
		it("is a pedal Sustain Pad under the Stab, not gated, with an 8-bar Brightness Sweep", _ {
			const PartSpec& pad = dub.parts[kPadPart];
			expect(pad.flavour == Flavour::Sustain).to_be_true();
			expect(pad.sustain.pedal).to_be_true();
			expect(pad.densityGate).to_equal(0);
			expect(pad.drift.sweepBars).to_equal(8);
			expect(pad.registerOrDefault().anchor)
			    .to_be_less_than(dub.parts[kStabPart].registerOrDefault().anchor);
		});

		it("holds the root and the 5th above it, under the Stab's highest note", _ {
			int32_t padNotes = 0;
			for (const Recipe& recipe : sweep()) {
				const Walked walked = walk(recipe, 1, 32 * kStepsPerBar);
				int32_t stabTop = 0;
				for (const Event& e : walked.noteOns) {
					stabTop = e.part == kStabPart ? std::max<int32_t>(stabTop, e.pitch) : stabTop;
				}
				for (const Event& e : walked.noteOns) {
					if (e.part != kPadPart) {
						continue;
					}
					padNotes++;
					const int32_t pc = pitchClass(e.pitch - recipe.keyRoot);
					expect(pc == 0 || pc == 7).to_be_true();
					expect(static_cast<int32_t>(e.pitch)).to_be_less_than(stabTop);
				}
			}
			expect(padNotes).to_be_greater_than(100);
		});
	});

	it("keeps only the Stab, the Pad and the Rim in a Break, with the FX part's section rules, and plays all three", _ {
		std::map<int32_t, int32_t> kept; // part (the Rim as -1) → note-ons in Breaks
		int32_t breaks = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked = walk(dubRecipe(Scale::Minor), seed, 96 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break) {
					continue;
				}
				const bool rim = e.part == 0 && e.lane == Lane::Rim;
				const bool ok = rim || e.part == kStabPart || e.part == kPadPart || e.part == kFxPart;
				expect(ok).to_be_true();
				// The pedal Pad re-strikes on the Break's first step and holds through it.
				kept[rim ? -1 : e.part]++;
				if (walked.steps[step].sectionStart) {
					continue;
				}
				breaks++;
			}
		}
		expect(breaks).to_be_greater_than(100);
		expect(kept[kStabPart]).to_be_greater_than(0);
		expect(kept[kPadPart]).to_be_greater_than(0);
		expect(kept[-1]).to_be_greater_than(0);
	});

	context("its palette (spec §10.1, §18.3)", _ {
		it("gives the Sub, the Stab and the Pad at least 3 presets each, on the factory card, the Stab's led by 037 Echo Chord", _ {
			FakeCard card = harness::factoryCardWithPack();
			expect(dub.palette != nullptr && dub.palette != genres::technoAcid.palette).to_be_true();
			int32_t synthParts = 0;
			for (const PartSpec& part : dub.parts) {
				if (!part.lanes.empty()) {
					continue;
				}
				synthParts++;
				auto entries = dub.palette->partEntries(part);
				expect(entries.size()).to_be_greater_than(size_t{2});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
				}
			}
			expect(synthParts).to_equal(3);
			const auto stabs = dub.palette->partEntries(dub.parts[kStabPart]);
			const auto main = std::max_element(stabs.begin(), stabs.end(), [](const PaletteEntry& a, const PaletteEntry& b) {
				return a.weight < b.weight;
			});
			expect(std::string(main->path)).to_equal(std::string("SYNTHS/037 Echo Chord.XML"));
		});

		it("takes Techno's lanes but a shaker alone on Perc 2", _ {
			const Palette& techno = *genres::technoAcid.palette;
			expect(dub.palette->lanes.size()).to_equal(techno.lanes.size());
			for (size_t l = 0; l < techno.lanes.size(); l++) {
				const LaneSounds& ours = dub.palette->lanes[l];
				const LaneSounds& theirs = techno.lanes[l];
				expect(ours.lane == theirs.lane).to_be_true();
				if (ours.lane == Lane::Perc2) {
					expect(ours.folders.size()).to_equal(size_t{1});
					expect(std::string(ours.folders[0])).to_equal(std::string("SAMPLES/DRUMS/Shaker"));
				}
				else {
					expect(ours.folders.data() == theirs.folders.data()).to_be_true();
					expect(ours.tags.data() == theirs.tags.data()).to_be_true();
				}
			}
		});
	});
});

CPPSPEC_SPEC(dub_techno)
