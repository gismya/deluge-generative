// Melodic techno, first cut (spec §4.5, #226): a Techno subgenre whose Chord graph walks under a rolling bass and an
// interim pluck lead. Run straight through the core: the chord walk on 2-bar lines, home on Drops, the cadence
// (♭VII, or V in Harmonic Minor), the bass off the kick's steps, the lead's rhythm and Degree set, the Break keeps.

#include "fake_card.h"
#include "generative/core/palette.h"
#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace harness;

namespace {

const Genre& melodic = genres::technoMelodic;

constexpr int32_t kBassPart = 1;
constexpr int32_t kLeadPart = 2;
constexpr int32_t kFxPart = 3;

/// What each decided step was: the chord, where the Arc stood, and the note-ons it started.
struct Walked {
	struct Step {
		int32_t chord = 0;
		SectionKind section = SectionKind::Intro;
		bool sectionStart = false;
		bool drop = false;
		int32_t stepInPhrase = 0;
	};
	std::vector<Step> steps;
	std::vector<Event> noteOns;

	/// The steps where the chord differs from the step before.
	[[nodiscard]] std::vector<int32_t> changes() const {
		std::vector<int32_t> out;
		for (size_t n = 1; n < steps.size(); n++) {
			if (steps[n].chord != steps[n - 1].chord) {
				out.push_back(static_cast<int32_t>(n));
			}
		}
		return out;
	}
};

Recipe melodicRecipe(Scale scale, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(melodic, entropy);
	recipe.keyRoot = 9; // A
	recipe.scale = scale;
	return recipe;
}

/// Decides `count` steps of `recipe` straight through the core.
Walked walk(const Recipe& recipe, uint32_t seed, int32_t count) {
	Walked out;
	auto generator = std::make_unique<Generator>();
	generator->start(recipe, seed, Takes{}, false);
	SteeringQueue queue;
	for (int32_t n = 0; n < count; n++) {
		generator->decideNextStep(queue, nullptr);
		const Arc& arc = generator->arc();
		out.steps.push_back(
		    {generator->chordRoot(), arc.section(), arc.sectionStartsHere(), arc.dropStartsHere(), arc.stepInPhrase()});
		Event event;
		while (generator->popDue(INT32_MAX, &event)) {
			if (event.kind == EventKind::NoteOn) {
				out.noteOns.push_back(event);
			}
		}
	}
	return out;
}

/// The step a note-on belongs to: the nearest grid line, since Lean moves it at most half a step.
int32_t stepOf(const Event& event) {
	return (event.tick + kTicksPerStep / 2) / kTicksPerStep;
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

	it("inherits the Techno parent's drums, fills, Arc and FX", _ {
		const Genre& parent = *melodic.parent;
		expect(melodic.parts[0].lanes.data() == parent.parts[0].lanes.data()).to_be_true();
		expect(melodic.parts[kFxPart].lanes.data() == parent.parts.back().lanes.data()).to_be_true();
		expect(melodic.fills.data() == parent.fills.data()).to_be_true();
		expect(melodic.arc.groovePhrases.low).to_equal(parent.arc.groovePhrases.low);
		expect(melodic.arc.buildBars.data() == parent.arc.buildBars.data()).to_be_true();
		expect(melodic.arc.roll.lane == parent.arc.roll.lane).to_be_true();
	});

	it("changes chord only on 2-bar lines, through the graph's moves, home or the cadence target", _ {
		const ChordGraph& graph = *melodic.harmony.graph;
		expect(graph.barsPerChord).to_equal(2);
		expect(graph.cadenceTarget).to_equal(6); // ♭VII
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
		expect(share(Scale::Minor, 6)).to_be_greater_than(50);         // ♭VII
		expect(share(Scale::HarmonicMinor, 4)).to_be_greater_than(50); // V
		expect(share(Scale::HarmonicMinor, 6)).to_be_less_than(share(Scale::HarmonicMinor, 4));
	});

	it("never starts a bass note on a kick step, the beats the four-on-the-floor kick holds", _ {
		int32_t bassNotes = 0;
		int32_t kickBeats = 0;
		for (Scale scale : melodic.facts.scales) {
			for (const Recipe& recipe : sweep(scale)) {
				for (uint32_t seed = 1; seed <= 3; seed++) {
					const Walked walked = walk(recipe, seed, 64 * kStepsPerBar);
					for (const Event& e : walked.noteOns) {
						if (e.part == 0 && e.lane == Lane::Kick && e.tick % kTicksPerStep == 0) {
							kickBeats += stepOf(e) % kStepsPerBeat == 0 ? 1 : 0;
						}
						if (e.part == kBassPart) {
							bassNotes++;
							expect(stepOf(e) % kStepsPerBeat != 0).to_be_true();
						}
					}
				}
			}
		}
		expect(bassNotes).to_be_greater_than(1000);
		expect(kickBeats).to_be_greater_than(1000); // the kick holds the beats the bass leaves
		for (const Pattern& pattern : melodic.parts[kBassPart].pool) {
			for (int32_t i = 0; i < pattern.length; i += kStepsPerBeat) {
				expect(pattern.importance[i]).to_equal(0);
			}
		}
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
		for (uint32_t seed = 1; seed <= 6; seed++) {
			const Walked walked = walk(melodicRecipe(Scale::Minor), seed, 96 * kStepsPerBar);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break || walked.steps[step].sectionStart) {
					continue;
				}
				breakNotes++;
				const bool kept = e.part == kBassPart || e.part == kLeadPart || e.part == kFxPart
				                  || (e.part == 0 && e.lane == Lane::ClosedHat);
				expect(kept).to_be_true();
			}
		}
		expect(breakNotes).to_be_greater_than(100);
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
