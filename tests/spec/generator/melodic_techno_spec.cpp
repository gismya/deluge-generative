// Melodic techno (spec §4.5, #226, #123, #219): a Techno subgenre whose Chord graph walks under a rolling bass, a
// Sustain Pad and an Arp lead. Run straight through the core: the chord walk on 2-bar lines, home on Drops, the
// cadence (♭VII, or V in Harmonic Minor), the bass off the four-on-the-floor kick's steps, the Pad's voicings, the
// Arp's chord tones, the Break keeps.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/palette.h"
#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace harness;

namespace {

const Genre& melodic = genres::technoMelodic;

constexpr int32_t kBassPart = 1;
constexpr int32_t kPadPart = 2;
constexpr int32_t kArpPart = 3;
constexpr int32_t kFxPart = 4;

Recipe melodicRecipe(Scale scale, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(melodic, entropy);
	recipe.keyRoot = 9; // A
	recipe.scale = scale;
	return recipe;
}

/// Every Complexity and Density corner the specs sweep, with Motion high enough to vary the Arc.
std::vector<Recipe> sweep(Scale scale) {
	std::vector<Recipe> out;
	for (int32_t complexity : {0, 25, 50}) {
		for (int32_t density : {10, 25, 50}) {
			Recipe recipe = melodicRecipe(scale, static_cast<uint32_t>(complexity * 7 + density));
			recipe.macros.set(Macro::Complexity, macroPoints(complexity));
			recipe.macros.set(Macro::Density, macroPoints(density));
			recipe.macros.set(Macro::Motion, macroPoints(40));
			out.push_back(recipe);
		}
	}
	return out;
}

/// Whether `from` → `to` is one of the graph's moves.
bool graphMove(int32_t from, int32_t to) {
	for (const ChordMove& move : melodic.harmony.graph->moves) {
		if (move.from == from && move.to == to) {
			return true;
		}
	}
	return false;
}

} // namespace

// clang-format off
describe melodic_techno("Melodic techno", $ {
	it("is a Techno subgenre of five parts: Drums, Bass, Pad, Arp and FX (spec §4.5)", _ {
		expect(std::string(melodic.name)).to_equal(std::string("techno-melodic"));
		expect(std::string(melodic.oledName)).to_equal(std::string("MELODIC TECHNO"));
		expect(std::string(melodic.shortName)).to_equal(std::string("MELODIC"));
		expect(melodic.parent == genres::technoAcid.parent).to_be_true();
		expect(findGenre("techno-melodic") == &melodic).to_be_true();
		expect(partCount(melodic)).to_equal(5);
		expect(melodic.parts[0].role == Role::Drums).to_be_true();
		expect(melodic.parts[kBassPart].role == Role::Bass).to_be_true();
		expect(melodic.parts[kPadPart].role == Role::Pad).to_be_true();
		expect(melodic.parts[kArpPart].role == Role::Lead).to_be_true();
		expect(melodic.parts[kFxPart].role == Role::FX).to_be_true();
		expect(melodic.facts.tempoLowBpm).to_equal(122);
		expect(melodic.facts.tempoHighBpm).to_equal(128);
		std::set<Scale> scales(melodic.facts.scales.begin(), melodic.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor}).to_be_true();
		expect(melodic.harmony.extensionCeiling).to_equal(1);
		expect(loopNotesValid(melodic)).to_be_true();
		expect(arpNotesValid(melodic)).to_be_true();
	});

	it("inherits the Techno parent's drums but the kick, fills, Arc and FX", _ {
		const Genre& parent = *melodic.parent;
		const std::span<const LaneSpec> drums = melodic.parts[0].lanes;
		const std::span<const LaneSpec> parentDrums = parent.parts[0].lanes;
		expect(drums.size()).to_equal(parentDrums.size());
		for (size_t l = 0; l < drums.size(); l++) {
			const LaneSpec& lane = drums[l];
			const LaneSpec& theirs = parentDrums[l];
			expect(lane.lane == theirs.lane).to_be_true();
			expect(lane.fillPool.data() == theirs.fillPool.data()).to_be_true();
			expect(lane.rules.data() == theirs.rules.data()).to_be_true();
			expect(lane.densityGate).to_equal(theirs.densityGate);
			expect(lane.gateTicks).to_equal(theirs.gateTicks);
			if (lane.lane != Lane::Kick) {
				expect(lane.pool.data() == theirs.pool.data()).to_be_true();
				expect(lane.band.low).to_equal(theirs.band.low);
				expect(lane.band.high).to_equal(theirs.band.high);
			}
		}
		expect(melodic.parts[0].gestures.off).to_equal(parent.parts[0].gestures.off);
		expect(melodic.parts[kFxPart].lanes.data() == parent.parts.back().lanes.data()).to_be_true();
		expect(melodic.fills.data() == parent.fills.data()).to_be_true();
		expect(melodic.arc.groovePhrases.low).to_equal(parent.arc.groovePhrases.low);
		expect(melodic.arc.buildBars.data() == parent.arc.buildBars.data()).to_be_true();
		expect(melodic.arc.roll.lane == parent.arc.roll.lane).to_be_true();
	});

	it("changes chord only on 2-bar lines, through the graph's moves, home or the cadence target", _ {
		const ChordGraph& graph = *melodic.harmony.graph;
		expect(graph.barsPerChord).to_equal(2);
		expect(graph.cadenceTarget).to_equal(kVII);
		expect(graph.cadencePeriod).to_equal(4);
		std::set<std::pair<int32_t, int32_t>> heard;
		for (const Recipe& recipe : sweep(Scale::Minor)) {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				expect(walked.steps[0].chord).to_equal(kHomeChord);
				for (int32_t n : walked.changes()) {
					const Walked::Step& step = walked.steps[n];
					const int32_t from = walked.steps[n - 1].chord;
					expect(step.stepInPhrase % (2 * kStepsPerBar) == 0 || (step.sectionStart && step.stepInPhrase % kStepsPerBar == 0))
					    .to_be_true();
					// Minor has no colour moves: a change is a move of the graph, a pull home or the cadence.
					expect(graphMove(from, step.chord) || step.chord == kHomeChord || step.chord == graph.cadenceTarget)
					    .to_be_true();
					heard.insert({from, step.chord});
				}
			}
		}
		for (const ChordMove& move : graph.moves) {
			expect(heard.count({move.from, move.to})).to_equal(size_t{1});
		}
	});

	it("plays home on every Drop", _ {
		int32_t drops = 0;
		for (Scale scale : melodic.facts.scales) {
			for (const Recipe& recipe : sweep(scale)) {
				for (uint32_t seed = 1; seed <= 3; seed++) {
					for (const Walked::Step& step : walk(recipe, seed, 96 * kStepsPerBar).steps) {
						if (step.drop) {
							drops++;
							expect(step.chord).to_equal(kHomeChord);
						}
					}
				}
			}
		}
		expect(drops).to_be_greater_than(20);
	});

	it("leans to ♭VII before a Drop in Minor, and to V with its leading tone in Harmonic Minor", _ {
		std::map<Scale, std::map<int32_t, int32_t>> beforeDrop;
		for (Scale scale : {Scale::Minor, Scale::HarmonicMinor}) {
			for (const Recipe& recipe : sweep(scale)) {
				for (uint32_t seed = 1; seed <= 3; seed++) {
					const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
					for (size_t n = 1; n < walked.steps.size(); n++) {
						if (walked.steps[n].drop) {
							beforeDrop[scale][walked.steps[n - 1].chord]++;
						}
					}
				}
			}
		}
		auto share = [&](Scale scale, int32_t chord) {
			int32_t all = 0;
			for (auto [c, count] : beforeDrop[scale]) {
				all += count;
			}
			return all == 0 ? 0 : 100 * beforeDrop[scale][chord] / all;
		};
		expect(share(Scale::Minor, kVII)).to_be_greater_than(50);
		expect(share(Scale::HarmonicMinor, kV)).to_be_greater_than(50);
		expect(share(Scale::HarmonicMinor, kVII)).to_be_less_than(share(Scale::HarmonicMinor, kV));
	});

	it("keeps its kick on the beats at every tier, with nothing to move it off them", _ {
		const LaneSpec& kick = melodic.parts[0].lanes[0];
		expect(kick.lane == Lane::Kick).to_be_true();
		expect(kick.pool.empty()).to_be_false();
		for (const Pattern& pattern : kick.pool) {
			for (int32_t i = 0; i < pattern.length; i++) {
				expect(pattern.importance[i] == (i % kStepsPerBeat == 0 ? kAnchor : 0)).to_be_true();
			}
		}
		expect(kick.fillPool.empty()).to_be_true(); // no Swap plays it
		for (int8_t lean : kick.expression.lean) {
			expect(lean).to_equal(int8_t{0});
		}
		expect(kick.expression.ornaments.rateFloor).to_equal(int16_t{0}); // no flams or ratchets
		expect(kick.expression.ornaments.rateCeiling).to_equal(int16_t{0});
		for (const Pattern& pattern : melodic.parts[kBassPart].pool) {
			for (int32_t i = 0; i < pattern.length; i += kStepsPerBeat) {
				expect(pattern.importance[i]).to_equal(0);
			}
		}
	});

	it("never starts a bass note on a step the kick plays, across whole Arcs: Intro to Outro, fills and Drops", _ {
		int32_t bassNotes = 0;
		int32_t kickNotes = 0;
		int32_t outros = 0;
		int32_t drops = 0;
		int32_t kickRests = 0; // beats a Rest fill took the kick from, in a Groove
		int32_t swaps = 0;     // Tom hits: only a Swap fill plays the Tom
		int32_t piece = 0;
		for (int32_t complexity : {0, 25, 50}) {
			for (int32_t density : {10, 25, 50}) {
				for (int32_t energy : {10, 25, 50}) {
					const Scale scale = melodic.facts.scales[static_cast<size_t>(piece) % melodic.facts.scales.size()];
					Recipe recipe = melodicRecipe(scale, static_cast<uint32_t>(piece));
					recipe.macros.set(Macro::Complexity, macroPoints(complexity));
					recipe.macros.set(Macro::Density, macroPoints(density));
					recipe.macros.set(Macro::Energy, macroPoints(energy));
					recipe.macros.set(Macro::Motion, macroPoints(piece % 2 == 0 ? 30 : 50));
					piece++;
					for (uint32_t seed = 1; seed <= 3; seed++) {
						// End after 96 bars: the Outro plays from the next phrase line, and the walk stops where it ends.
						const Walked walked = walk(recipe, seed, 160 * kStepsPerBar,
						                           [](int32_t n, const Generator&, SteeringQueue& queue) {
							                           if (n == 96 * kStepsPerBar) {
								                           queue.push({CommandKind::End, 0, 1});
							                           }
						                           });
						outros += walked.generator->ended() ? 1 : 0;
						std::set<int32_t> kicks;
						for (const Event& e : walked.noteOns) {
							if (e.part == 0 && e.lane == Lane::Kick) {
								kickNotes++;
								kicks.insert(stepOf(e));
								expect(stepOf(e) % kStepsPerBeat).to_equal(0);
							}
							if (e.part == 0 && e.lane == Lane::Tom) {
								swaps++;
							}
						}
						for (const Event& e : walked.noteOns) {
							if (e.part == kBassPart) {
								bassNotes++;
								expect(kicks.count(stepOf(e))).to_equal(size_t{0});
							}
						}
						for (size_t n = 0; n < walked.steps.size(); n++) {
							drops += walked.steps[n].drop ? 1 : 0;
							if (walked.steps[n].section == SectionKind::Groove && n % kStepsPerBeat == 0
							    && kicks.count(static_cast<int32_t>(n)) == 0) {
								kickRests++;
							}
						}
					}
				}
			}
		}
		expect(outros).to_equal(piece * 3);
		expect(drops).to_be_greater_than(40);
		expect(kickRests).to_be_greater_than(100);
		expect(swaps).to_be_greater_than(100);
		expect(bassNotes).to_be_greater_than(10000);
		expect(kickNotes).to_be_greater_than(10000);
	});

	it("puts its Bass's tonic within 6 semitones of C1, the Bass Register's shape an octave down (spec §4.5, §10.6)", _ {
		const Register reg = melodic.parts[kBassPart].registerOrDefault();
		expect(static_cast<int32_t>(reg.anchor)).to_equal(24);
		expect(static_cast<int32_t>(reg.spanLow)).to_equal(-5);
		expect(static_cast<int32_t>(reg.spanHigh)).to_equal(12);
	});

	context("its Arp (spec §4.5, §5.2, §8.1)", _ {
		it("is a Lead Arp on 16ths, Up or Up-down over one or two octaves, not gated, with a 16-bar Sweep", _ {
			const PartSpec& arp = melodic.parts[kArpPart];
			expect(arp.flavour == Flavour::Arp).to_be_true();
			expect(std::string(arp.name)).to_equal(std::string("ARP"));
			expect(arp.densityGate).to_equal(0);
			expect(arp.drift.sweepBars).to_equal(16);
			expect(arp.arp.walks.size()).to_equal(arp.pool.size());
			std::set<int32_t> octaves;
			for (size_t i = 0; i < arp.pool.size(); i++) {
				const ArpWalk& walk = arp.arp.walks[i];
				expect(walk.order == ArpOrder::Up || walk.order == ArpOrder::UpDown).to_be_true();
				octaves.insert(walk.octaves);
				// On 16ths: every pattern has neighbouring onsets.
				const Pattern& pattern = arp.pool[i];
				bool sixteenths = false;
				for (int32_t s = 0; s + 1 < pattern.length; s++) {
					sixteenths = sixteenths || (pattern.importance[s] > 0 && pattern.importance[s + 1] > 0);
				}
				expect(sixteenths).to_be_true();
			}
			expect(octaves == std::set<int32_t>{1, 2}).to_be_true();
		});

		it("plays only the chord's notes, in its Register, one note at a time, across whole pieces", _ {
			int32_t arpNotes = 0;
			for (Scale scale : melodic.facts.scales) {
				for (const Recipe& recipe : sweep(scale)) {
					const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], melodic.harmony.extensionFloor,
					                                     melodic.harmony.extensionCeiling);
					const Register reg = melodic.parts[kArpPart].registerOrDefault();
					const int32_t tonic = tonicNote(reg, recipe.keyRoot);
					for (uint32_t seed = 1; seed <= 2; seed++) {
						const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
						expect(walked.dropped).to_equal(0U);
						expect(walked.evicted).to_equal(0U);
						for (const Event& e : walked.noteOns) {
							if (e.part != kArpPart) {
								continue;
							}
							arpNotes++;
							expect(e.pitch >= tonic + reg.spanLow && e.pitch <= tonic + reg.spanHigh).to_be_true();
							const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
							const Chord chord = voiceChord(scale, walked.steps[step].chord, level);
							bool tone = false;
							for (int32_t i = 0; i < chord.size; i++) {
								tone = tone || (semitonesOf(chord.steps[i], scale) + recipe.keyRoot - e.pitch) % 12 == 0;
							}
							expect(tone).to_be_true();
						}
					}
				}
			}
			expect(arpNotes).to_be_greater_than(10000);
		});
	});

	it("keeps only the Pad, the Arp and the Closed hat in a Break, with the FX part's section rules", _ {
		int32_t breakNotes = 0;
		int32_t padNotes = 0;
		int32_t arpNotes = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Walked walked = walk(melodicRecipe(Scale::Minor), seed, 96 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break || walked.steps[step].sectionStart) {
					continue;
				}
				breakNotes++;
				padNotes += e.part == kPadPart ? 1 : 0;
				arpNotes += e.part == kArpPart ? 1 : 0;
				const bool kept = e.part == kPadPart || e.part == kArpPart || e.part == kFxPart
				                  || (e.part == 0 && e.lane == Lane::ClosedHat);
				expect(kept).to_be_true();
			}
		}
		expect(breakNotes).to_be_greater_than(100);
		expect(padNotes).to_be_greater_than(0);
		expect(arpNotes).to_be_greater_than(0);
	});

	context("its Pad (spec §5.2, §7.4, §8.1)", _ {
		it("is a Sustain Pad, not gated, with an 8-bar Brightness Sweep and no doubled root", _ {
			const PartSpec& pad = melodic.parts[kPadPart];
			expect(pad.flavour == Flavour::Sustain).to_be_true();
			expect(pad.densityGate).to_equal(0);
			expect(pad.drift.sweepBars).to_equal(8);
			expect(pad.sustain.pedal).to_be_false();
			expect(pad.sustain.doubleRoot).to_be_false();
		});

		it("re-strikes the chord's notes at each chord change, holds them while it stays, and never sounds 5 at once", _ {
			int32_t changes = 0;
			for (Scale scale : melodic.facts.scales) {
				for (const Recipe& recipe : sweep(scale)) {
					for (uint32_t seed = 1; seed <= 2; seed++) {
						const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
						expect(walked.dropped).to_equal(0U);
						expect(walked.evicted).to_equal(0U);
						const Register reg = melodic.parts[kPadPart].registerOrDefault();
						const int32_t tonic = tonicNote(reg, recipe.keyRoot);
						std::map<int32_t, std::set<int32_t>> onsAt; // step → pitches the Pad starts there
						std::set<int32_t> sounding;
						size_t most = 0;
						for (const Event& e : walked.notes) {
							if (e.part != kPadPart) {
								continue;
							}
							if (e.kind == EventKind::NoteOff) {
								sounding.erase(e.pitch);
								continue;
							}
							expect(e.tick % kBarTicks).to_equal(0);
							expect(e.pitch >= tonic + reg.spanLow && e.pitch <= tonic + reg.spanHigh).to_be_true();
							const int32_t step = stepOf(e);
							const Walked::Step& at = walked.steps[std::min<size_t>(step, walked.steps.size() - 1)];
							const bool line = step == 0 || at.sectionStart
							                  || at.chord != walked.steps[step - 1].chord;
							// Off a chord or Section line a note-on is one Density brings in: never a held note again.
							expect(line || sounding.count(e.pitch) == 0).to_be_true();
							sounding.insert(e.pitch);
							most = std::max(most, sounding.size());
							onsAt[step].insert(e.pitch);
						}
						expect(most).to_be_less_than(size_t{5});
						for (int32_t n : walked.changes()) {
							if (onsAt.count(n) == 0) {
								continue; // kept out by a section rule, a Rest or the Gap
							}
							changes++;
							// Its notes are the new chord's: the root always (an anchor), the rest as Density allows.
							const int32_t root = (semitonesOf(walked.steps[n].chord, scale) + recipe.keyRoot) % 12;
							bool hasRoot = false;
							for (int32_t pitch : onsAt[n]) {
								hasRoot = hasRoot || pitch % 12 == root;
							}
							expect(hasRoot).to_be_true();
						}
					}
				}
			}
			expect(changes).to_be_greater_than(200);
		});
	});

	context("its palette (spec §10.1, §18.3)", _ {
		it("gives the bass, the Pad and the Arp at least 3 presets each, on the factory card, and the Arp none of the acid's", _ {
			FakeCard card = harness::factoryCardWithPack();
			const Genre& techno = genres::technoAcid;
			expect(melodic.palette != nullptr && melodic.palette != techno.palette).to_be_true();
			std::set<std::string> acid;
			for (const PartSpec& part : techno.parts) {
				if (part.lanes.empty()) {
					for (const PaletteEntry& entry : techno.palette->partEntries(part)) {
						acid.insert(entry.path);
					}
				}
			}
			int32_t synthParts = 0;
			for (const PartSpec& part : melodic.parts) {
				if (!part.lanes.empty()) {
					continue;
				}
				synthParts++;
				// At least 3 before the ear check trimmed them (spec §10.1, #120 round 1), never none.
				auto entries = melodic.palette->partEntries(part);
				expect(entries.size()).to_be_greater_than(size_t{0});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
					if (part.role == Role::Lead) {
						expect(acid.count(entry.path)).to_equal(size_t{0});
					}
				}
			}
			expect(synthParts).to_equal(3);
		});

		it("shares Techno's drum and FX lanes", _ {
			expect(melodic.palette->lanes.data() == genres::technoAcid.palette->lanes.data()).to_be_true();
			expect(melodic.palette->lanes.size()).to_equal(genres::technoAcid.palette->lanes.size());
		});
	});
});

CPPSPEC_SPEC(melodic_techno)
