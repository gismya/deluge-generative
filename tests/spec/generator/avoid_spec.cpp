// The Avoid interplay rule (spec §5.5): its against steps are the target's planned onsets, so at full strength the
// source never plays where the target is planned, whether the target plays there, is muted, Rested or cut. A note
// before an avoided step never sounds into it. genreValid() rejects a source anchor on a step the target can play.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <set>
#include <utility>
#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr int32_t kPhraseSteps = 4 * kStepsPerBar;

/// Kick patterns in every tier, thinned by Density, one of them 2 bars long.
constexpr Pattern kKicks[] = {
    {"X...X...X...X...", "................", 0},
    {"X..5X...X.3.X...", "................", 1},
    {"X...X.7.X..4X.2.", "................", 2},
    {"X..6X...X.5.X..3X...X.8.X...X1..", "................................", 3},
};
/// A Bass that would play every step, anchored only where no kick pattern plays (steps 1, 2, 5, 7 and 9 of a bar).
constexpr Pattern kBusy[] = {
    {"555555555X555555", "................", 0},
    {"959595959X959595", "................", 1},
    {"191919191X191919", "................", 2},
};
constexpr LineWeight kRoots[] = {{0, 2}, {2, 1}, {4, 1}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kKicks, .band = {0, 3}}, {.lane = Lane::Snare, .pool = kKicks}, // the roll lane
};
constexpr FillEntry kKickDrop[] = {{.kind = FillKind::Rest, .mask = {.lanes = laneBit(Lane::Kick)}, .beats = 4}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuild4[] = {{4, 1}};

/// Bass Avoid Kick at `Strength` with `Sensitivity`, or with no rule at all; sliding on every note (each a step long
/// plus the overlap) or with `Slide` 0 on none, its plain notes `Gate` ticks long.
template <int8_t Strength, int16_t Slide = 1000, uint16_t Gate = 2 * kTicksPerStep, int8_t Sensitivity = 0,
          bool HasRule = true>
struct AvoidKick {
	static constexpr InterplayRule rule[] = {{.kind = RuleKind::Avoid,
	                                          .target = {kDrumsPart, Lane::Kick},
	                                          .strength = Strength,
	                                          .sensitivity = Sensitivity}};
	static constexpr std::span<const InterplayRule> rules =
	    HasRule ? std::span<const InterplayRule>(rule) : std::span<const InterplayRule>();
	static constexpr PartSpec parts[] = {
	    {.role = Role::Drums, .lanes = kDrums},
	    {.role = Role::Bass,
	     .flavour = Flavour::Loop,
	     .pool = kBusy,
	     .band = {0, 2},
	     .rules = rules,
	     .loop = {.degrees = kRoots, .slide = Slide, .gateTicks = Gate}},
	};
	/// Two-phrase Grooves with a kick drop at every phrase's end, a Break that keeps everything, and a Build that cuts
	/// the kick for its last 2 bars, with no Gap.
	static constexpr Genre genre = {
	    .name = "avoid",
	    .facts = {.scales = kScales},
	    .phraseBars = 4,
	    .parts = parts,
	    .fills = kKickDrop,
	    .fillChance = 1000,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {2, 2},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuild4,
	            .breakKeeps = kEverything,
	            .buildCut = {.lanes = laneBit(Lane::Kick)},
	            .buildCutBars = 2,
	            .gap = GapLength::None,
	            .roll = {.lane = Lane::Snare},
	        },
	};
};

/// Records, on each decided step, whether the kick is planned there, as the Bass's rule reads it.
struct PlannedKick {
	std::set<int32_t> steps;
	static inline PlannedKick* current = nullptr;
	PlannedKick() {
		current = this;
		Generator::testDecided_ = [](const Generator& g, int32_t step, int32_t density) {
			if (g.testTargetPlanned({kDrumsPart, Lane::Kick}, step, density)) {
				current->steps.insert(step);
			}
		};
	}
	~PlannedKick() {
		Generator::testDecided_ = nullptr;
		current = nullptr;
	}
	PlannedKick(const PlannedKick&) = delete;
	PlannedKick& operator=(const PlannedKick&) = delete;
};

Run avoidRun(const Genre& genre, uint32_t seed, int32_t density, int32_t complexity, int32_t motion) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, macroPoints(motion));
	run.seed = seed;
	return run;
}

/// The steps Bass note-ons start on in the first `bars`.
std::set<int32_t> bassSteps(const Result& result) {
	std::set<int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == kBassPart) {
			out.insert(e.tick / kTicksPerStep);
		}
	}
	return out;
}

/// Whether `step` is in a Build.
bool inBuild(const Result& result, int32_t step) {
	SectionKind playing = SectionKind::Intro;
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.tick > stepTick(step)) {
			break;
		}
		playing = e.section;
	}
	return playing == SectionKind::Build;
}

bool subset(const std::set<int32_t>& inner, const std::set<int32_t>& outer) {
	for (int32_t step : inner) {
		if (outer.count(step) == 0) {
			return false;
		}
	}
	return true;
}

} // namespace

// clang-format off
describe avoid("The Avoid rule", $ {
	it("at full strength plays no Bass note on, or sounding into, a step the kick is planned on", _ {
		int32_t pieces = 0;
		// Planned kicks the stream doesn't play, by cause: cut (in a Build), Rested (in a phrase's last 4 beats, the
		// kick drop) and, in a muted piece, muted (anywhere else). The harness drops muted note-ons, as Perform does.
		int32_t muteMissing = 0;
		int32_t cutMissing = 0;
		int32_t restMissing = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			for (int32_t density : {0, 20, 35, 50}) {
				for (int32_t complexity : {0, 25, 50}) {
					for (bool muted : {false, true}) {
						PlannedKick planned;
						Run run = avoidRun(AvoidKick<100>::genre, seed, density, complexity, 25 + 10 * (seed % 3));
						if (muted) {
							run.steer = steerAt({{8 * kStepsPerBar, mute(kDrumsPart)}});
						}
						Result result = play(run, 40);
						pieces++;
						std::vector<Held> notes = heldNotes(result.events, kBassPart);
						expect(notes.empty()).to_be_false();
						for (const Held& note : notes) {
							const int32_t step = note.on / kTicksPerStep;
							expect(planned.steps.count(step)).to_equal(size_t{0});
							// Into the next step only when it is free.
							if (note.off > stepTick(step + 1)) {
								expect(planned.steps.count(step + 1)).to_equal(size_t{0});
							}
						}
						for (int32_t step : planned.steps) {
							// The step decided ahead of the stream's end never plays.
							if (step >= 40 * kStepsPerBar || !ons(result, kDrumsPart, Lane::Kick, step, step + 1).empty()) {
								continue;
							}
							if (inBuild(result, step)) {
								cutMissing++;
							}
							else if (step % kPhraseSteps >= kPhraseSteps - 4 * kStepsPerBeat) {
								restMissing++;
							}
							else {
								expect(muted).to_be_true(); // nothing else silences a planned kick here
								muteMissing++;
							}
						}
					}
				}
			}
		}
		expect(pieces).to_equal(144);
		// The Bass held its holes for kicks it couldn't hear, for each cause.
		expect(muteMissing).to_be_greater_than(0);
		expect(cutMissing).to_be_greater_than(0);
		expect(restMissing).to_be_greater_than(0);
	});

	it("slides a Bass note into the next step only where the kick leaves it free", _ {
		Result result = play(avoidRun(AvoidKick<100>::genre, 1, 50, 25, 0), 8);
		int32_t slid = 0;
		for (const Held& note : heldNotes(result.events, kBassPart)) {
			slid += note.off > stepTick(note.on / kTicksPerStep + 1) ? 1 : 0;
		}
		expect(slid).to_be_greater_than(0);
	});

	it("plays a slid Bass note before an avoided step plain, as long as its gate", _ {
		// A gate shorter than a step: cut on the line, the slide would still sound 24 ticks.
		using Short = AvoidKick<100, 1000, 14>;
		int32_t before = 0;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			PlannedKick planned;
			Result result = play(avoidRun(Short::genre, seed, 50, 25, 25), 24);
			for (const Held& note : heldNotes(result.events, kBassPart)) {
				if (planned.steps.count(note.on / kTicksPerStep + 1) != 0) {
					before++;
					expect(note.off - note.on).to_equal(14);
				}
			}
		}
		expect(before).to_be_greater_than(0);
	});

	it("at effective strength 0 plays exactly what no rule plays", _ {
		// Strength 50 with sensitivity +100 is 0 at Complexity 0, the genre's default being 25.
		for (uint32_t seed = 1; seed <= 4; seed++) {
			Result zero = play(avoidRun(AvoidKick<50, 1000, 2 * kTicksPerStep, 100>::genre, seed, 50, 0, 25), 24);
			Result none =
			    play(avoidRun(AvoidKick<50, 1000, 2 * kTicksPerStep, 100, false>::genre, seed, 50, 0, 25), 24);
			expect(zero.events == none.events).to_be_true();
		}
	});

	it("drops a keyed share below full strength, and a higher strength only ever drops more", _ {
		// No slides, so no tie hides a note-on.
		size_t total[4] = {};
		for (uint32_t seed = 1; seed <= 10; seed++) {
			const std::set<int32_t> byStrength[] = {
			    bassSteps(play(avoidRun(AvoidKick<0, 0>::genre, seed, 50, 25, 0), 8)),
			    bassSteps(play(avoidRun(AvoidKick<30, 0>::genre, seed, 50, 25, 0), 8)),
			    bassSteps(play(avoidRun(AvoidKick<60, 0>::genre, seed, 50, 25, 0), 8)),
			    bassSteps(play(avoidRun(AvoidKick<100, 0>::genre, seed, 50, 25, 0), 8)),
			};
			for (size_t i = 0; i < std::size(byStrength); i++) {
				total[i] += byStrength[i].size();
				expect(i == 0 || subset(byStrength[i], byStrength[i - 1])).to_be_true();
			}
		}
		for (size_t i = 1; i < std::size(total); i++) {
			expect(total[i]).to_be_less_than(total[i - 1]);
		}
	});

	it("is rejected by genreValid() where the source anchors a step any target pattern can play", _ {
		expect(avoidRulesValid(AvoidKick<100>::genre)).to_be_true();
		// An anchor on step 3: the tier 1 kick plays it at Density.
		static constexpr Pattern kOnThree[] = {{"...X............", "................", 0}};
		// An anchor on step 13 of a 16-step Bass: the 2-bar kick's step 29 is the same step in the bar.
		static constexpr Pattern kOnThirteen[] = {{".............X..", "................", 0}};
		// An anchor on step 1 of a 2-bar Bass's second bar: no kick plays a bar's step 1.
		static constexpr Pattern kOnOne[] = {{".................X..............", "................................", 0}};
		auto withBass = [](std::span<const Pattern> pool) {
			static PartSpec parts[2];
			parts[0] = AvoidKick<100>::parts[0];
			parts[1] = AvoidKick<100>::parts[1];
			parts[1].pool = pool;
			Genre genre = AvoidKick<100>::genre;
			genre.parts = parts;
			return avoidRulesValid(genre);
		};
		expect(withBass(kOnThree)).to_be_false();
		expect(withBass(kOnThirteen)).to_be_false();
		expect(withBass(kOnOne)).to_be_true();
		// A Lock rule's source may anchor anywhere.
		static constexpr InterplayRule kLock[] = {{.kind = RuleKind::Lock, .target = {kDrumsPart, Lane::Kick}}};
		static PartSpec locked[2] = {AvoidKick<100>::parts[0], AvoidKick<100>::parts[1]};
		locked[1].pool = kOnThree;
		locked[1].rules = kLock;
		Genre genre = AvoidKick<100>::genre;
		genre.parts = locked;
		expect(avoidRulesValid(genre)).to_be_true();
	});
});

CPPSPEC_SPEC(avoid)
