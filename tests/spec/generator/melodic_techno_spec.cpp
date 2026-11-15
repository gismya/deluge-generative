// Melodic techno, first cut (spec §4.5, #226): a Techno subgenre whose Chord graph walks under a rolling bass and an
// interim pluck lead. Run straight through the core: the chord walk on 2-bar lines, home on Drops, the cadence
// (♭VII, or V in Harmonic Minor), the bass off the four-on-the-floor kick's steps, the lead's rhythm and Degree set,
// the Break keeps.

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
constexpr int32_t kLeadPart = 2;
constexpr int32_t kFxPart = 3;

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
	it("is a Techno subgenre of four parts: Drums, Bass, Lead and FX (spec §4.5)", _ {
		expect(std::string(melodic.name)).to_equal(std::string("techno-melodic"));
		expect(std::string(melodic.oledName)).to_equal(std::string("MELODIC TECHNO"));
		expect(std::string(melodic.shortName)).to_equal(std::string("MELODIC"));
		expect(melodic.parent == genres::techno.parent).to_be_true();
		expect(findGenre("techno-melodic") == &melodic).to_be_true();
		expect(partCount(melodic)).to_equal(4);
		expect(melodic.parts[0].role == Role::Drums).to_be_true();
		expect(melodic.parts[kBassPart].role == Role::Bass).to_be_true();
		expect(melodic.parts[kLeadPart].role == Role::Lead).to_be_true();
		expect(melodic.parts[kFxPart].role == Role::FX).to_be_true();
		expect(melodic.facts.tempoLowBpm).to_equal(122);
		expect(melodic.facts.tempoHighBpm).to_equal(128);
		std::set<Scale> scales(melodic.facts.scales.begin(), melodic.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor}).to_be_true();
		expect(melodic.harmony.extensionCeiling).to_equal(1);
		expect(loopNotesValid(melodic)).to_be_true();
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

	it("plays the lead in 8ths and dotted figures, never in neighbouring 16ths, and never slides", _ {
		const PartSpec& lead = melodic.parts[kLeadPart];
		expect(lead.flavour == Flavour::Loop).to_be_true();
		expect(lead.loop.slide.atZero).to_equal(0);
		expect(lead.loop.slide.atFull).to_equal(0);
		for (const Pattern& pattern : lead.pool) {
			for (int32_t i = 0; i < pattern.length; i++) {
				const bool both = pattern.importance[i] > 0 && pattern.importance[(i + 1) % pattern.length] > 0;
				expect(both).to_be_false();
			}
		}
		int32_t leadNotes = 0;
		for (const Recipe& recipe : sweep(Scale::Minor)) {
			for (uint32_t seed = 1; seed <= 3; seed++) {
				int32_t last = -100;
				for (const Event& e : walk(recipe, seed, 64 * kStepsPerBar).noteOns) {
					if (e.part == kLeadPart) {
						leadNotes++;
						expect(stepOf(e) - last).to_be_greater_than(1);
						last = stepOf(e);
					}
				}
			}
		}
		expect(leadNotes).to_be_greater_than(200);
	});

	it("keeps the lead on the key's Pentatonic degrees or the chord's root (spec §7.5)", _ {
		const PartSpec& lead = melodic.parts[kLeadPart];
		expect(lead.degreeSet == DegreeSet::Pentatonic).to_be_true();
		int32_t leadNotes = 0;
		for (Scale scale : melodic.facts.scales) {
			const Recipe recipe = melodicRecipe(scale);
			for (uint32_t seed = 1; seed <= 4; seed++) {
				Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
				for (const Event& e : walked.noteOns) {
					if (e.part != kLeadPart) {
						continue;
					}
					leadNotes++;
					const int32_t pitchClass = (e.pitch - recipe.keyRoot + 120) % 12;
					const uint8_t bits = kDegreeSets[static_cast<int32_t>(DegreeSet::Pentatonic)][static_cast<int32_t>(scale)];
					bool allowed = false;
					for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
						allowed = allowed || (((bits >> degree) & 1U) != 0 && semitonesOf(degree, scale) == pitchClass);
					}
					const int32_t chord = walked.steps[std::min<size_t>(stepOf(e), walked.steps.size() - 1)].chord;
					allowed = allowed || semitonesOf(chord, scale) % 12 == pitchClass;
					expect(allowed).to_be_true();
				}
			}
		}
		expect(leadNotes).to_be_greater_than(100);
	});

	it("keeps only the Bass, the Lead and the Closed hat in a Break, with the FX part's section rules", _ {
		int32_t breakNotes = 0;
		int32_t bassNotes = 0;
		int32_t leadNotes = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Walked walked = walk(melodicRecipe(Scale::Minor), seed, 96 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break || walked.steps[step].sectionStart) {
					continue;
				}
				breakNotes++;
				bassNotes += e.part == kBassPart ? 1 : 0;
				leadNotes += e.part == kLeadPart ? 1 : 0;
				const bool kept = e.part == kBassPart || e.part == kLeadPart || e.part == kFxPart
				                  || (e.part == 0 && e.lane == Lane::ClosedHat);
				expect(kept).to_be_true();
			}
		}
		expect(breakNotes).to_be_greater_than(100);
		expect(bassNotes).to_be_greater_than(0);
		expect(leadNotes).to_be_greater_than(0);
	});

	context("its palette (spec §10.1, §18.3)", _ {
		it("gives the bass and the lead at least 3 presets each, on the factory card, and the lead none of the acid's", _ {
			FakeCard card = harness::factoryCardWithPack();
			const Genre& techno = genres::techno;
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
				auto entries = melodic.palette->partEntries(part);
				expect(entries.size()).to_be_greater_than(size_t{2});
				for (const PaletteEntry& entry : entries) {
					expect(card.has(entry.path)).to_be_true();
					if (part.role == Role::Lead) {
						expect(acid.count(entry.path)).to_equal(size_t{0});
					}
				}
			}
			expect(synthParts).to_equal(2);
		});

		it("shares Techno's drum and FX lanes", _ {
			expect(melodic.palette->lanes.data() == genres::techno.palette->lanes.data()).to_be_true();
			expect(melodic.palette->lanes.size()).to_equal(genres::techno.palette->lanes.size());
		});
	});
});

CPPSPEC_SPEC(melodic_techno)
