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

/// Records, on each decided step, whether the kick is planned there, as the Bass's rule reads it, and whether the
/// Bass is.
struct PlannedKick : harness::DecidedHook<PlannedKick> {
	std::set<int32_t> steps;
	std::set<int32_t> bass;
	void decided(const Generator& g, int32_t step, int32_t density) {
		if (g.testTargetPlanned({kDrumsPart, Lane::Kick}, step, density)) {
			steps.insert(step);
		}
		if (g.testTargetPlanned({kBassPart}, step, density)) {
			bass.insert(step);
		}
	}
};

/// AvoidKick's genre with a Bass that ratchets every note (2, 3 or 4 hits), its 16ths leaning by `Lean`, on a gate of
/// `Gate` ticks (the default 16, or one under the 12-tick lookahead).
template <int8_t Lean, uint16_t Gate = 16>
struct RatchetBass {
	static inline const PartSpec parts[] = {
	    AvoidKick<100>::parts[0],
	    [] {
		    PartSpec bass = AvoidKick<100>::parts[1];
		    bass.loop.gateTicks = Gate;
		    bass.loop.slide = 300;
		    bass.expression = {.lean = {Lean, Lean, Lean, Lean},
		                       .ornaments = {.rateFloor = 1000, .rateCeiling = 1000, .ratchet = {1, 1, 1}}};
		    return bass;
	    }(),
	};
	static inline const Genre genre = [] {
		Genre g = AvoidKick<100>::genre;
		g.parts = parts;
		return g;
	}();
};

/// A kick on each bar's first step (with `Mid`, on its step 8), and a source of `Flavour` whose notes, from step 2,
/// would last past the next bar line: a Stab (to the bar line at most, its own rule) or a Loop with a long gate. Avoid
/// Kick at `Strength`.
template <Flavour F, int8_t Strength, bool Mid = false>
struct LongSource {
	static constexpr Pattern kDownbeat[] = {{"X...............", "................", 0}};
	static constexpr Pattern kMid[] = {{"........X.......", "................", 0}};
	static constexpr Pattern kOnTwo[] = {{"..X.............", "................", 0}};
	static constexpr LaneSpec lanes[] = {
	    {.lane = Lane::Kick, .pool = Mid ? std::span<const Pattern>(kMid) : std::span<const Pattern>(kDownbeat)}};
	static constexpr InterplayRule rules[] = {
	    {.kind = RuleKind::Avoid, .target = {kDrumsPart, Lane::Kick}, .strength = Strength}};
	static constexpr LineWeight kRoot[] = {{0, 1}};
	static constexpr PartSpec parts[] = {
	    {.role = Role::Drums, .lanes = lanes},
	    {.role = F == Flavour::Stab ? Role::Chords : Role::Bass,
	     .flavour = F,
	     .pool = kOnTwo,
	     .rules = rules,
	     .loop = {.degrees = kRoot, .slide = 0, .gateTicks = 30 * kTicksPerStep},
	     .stab = {.gateTicks = 30 * kTicksPerStep}},
	};
	static inline const Genre genre = [] {
		Genre g = AvoidKick<100>::genre;
		g.parts = parts;
		g.fills = {};
		return g;
	}();
};

/// AvoidKick's genre with a Bass on the root alone, so every slide ties onto the next note, sliding at `Slide` with a
/// 20-tick gate.
template <int16_t Slide>
struct TiedBass {
	static constexpr LineWeight kRoot[] = {{0, 1}};
	static inline const PartSpec parts[] = {
	    AvoidKick<100>::parts[0],
	    [] {
		    PartSpec bass = AvoidKick<100>::parts[1];
		    bass.loop = {.degrees = kRoot,
		                 .draws = {.leap = 0, .octaveUp = 0, .accent = 0},
		                 .slide = Slide,
		                 .mutation = 0,
		                 .gateTicks = 20};
		    return bass;
	    }(),
	};
	static inline const Genre genre = [] {
		Genre g = AvoidKick<100>::genre;
		g.parts = parts;
		return g;
	}();
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
							// Into a later step only when it is free, every step it spans (a tie spans several).
							for (int32_t later = step + 1; stepTick(later) < note.off && later < 40 * kStepsPerBar;
							     later++) {
								expect(planned.steps.count(later)).to_equal(size_t{0});
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

	it("measures a tied note's plain gate from its last segment, and leaves one ending before the avoided step", _ {
		// The note sounding on the step before an avoided step ends 20 ticks after that step's line: a tie (slide 1000)
		// cut back from its last segment, or (slide 500) one whose last segment didn't slide and already ends there.
		auto check = [&](const Genre& genre) {
			int32_t tied = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				PlannedKick planned;
				Result result = play(avoidRun(genre, seed, 50, 25, 25), 24);
				for (const Held& note : heldNotes(result.events, kBassPart)) {
					for (int32_t kick : planned.steps) {
						const int32_t before = stepTick(kick - 1);
						if (planned.bass.count(kick - 1) != 0 && note.on <= before && note.off > before
						    && kick < 24 * kStepsPerBar) {
							tied += note.on < before ? 1 : 0;
							expect(note.off).to_equal(before + 20);
						}
					}
				}
			}
			expect(tied).to_be_greater_than(0);
		};
		check(TiedBass<1000>::genre);
		check(TiedBass<500>::genre);
	});

	it("keeps a ratchet's hits inside their 16th before an avoided step, never ending one before it starts", _ {
		auto check = [&](const Genre& genre) {
			int32_t before = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				PlannedKick planned;
				Result result = play(avoidRun(genre, seed, 50, 25, 25), 24);
				for (const Held& note : heldNotes(result.events, kBassPart)) {
					if (note.on >= stepTick(23 * kStepsPerBar)) {
						continue; // the stop at the end ends what is decided ahead of it at once
					}
					expect(note.off).to_be_greater_than(note.on);
					expect(note.off).to_be_less_than(INT32_MAX);
					// A hit on the step before a planned kick ends by its line.
					const int32_t next = note.on / kTicksPerStep + 1;
					if (planned.steps.count(next) != 0 && next < 24 * kStepsPerBar) {
						before++;
						expect(note.off).to_be_less_than(stepTick(next) + 1);
					}
				}
			}
			expect(before).to_be_greater_than(0);
		};
		check(RatchetBass<0>::genre);
		check(RatchetBass<6>::genre);
		check(RatchetBass<-6>::genre);
		// A gate under the lookahead: the plain gate ends at the decision tick, and no later hit before it starts.
		check(RatchetBass<0, 8>::genre);
		check(RatchetBass<-6, 8>::genre);
	});

	it("ends a Stab source's notes on an avoided step's line", _ {
		PlannedKick planned;
		// The kick on step 8: without the rule a stab from step 2 lasts to the bar line; with it, to step 8's line.
		auto ends = [](const Genre& genre) {
			std::set<int32_t> out;
			for (const Held& note : heldNotes(play(avoidRun(genre, 1, 50, 25, 25), 16).events, kBassPart)) {
				if (note.on < stepTick(15 * kStepsPerBar)) {
					out.insert((note.off - note.on) / kTicksPerStep);
				}
			}
			return out;
		};
		expect(ends(LongSource<Flavour::Stab, 0, true>::genre) == std::set<int32_t>{14}).to_be_true();
		expect(ends(LongSource<Flavour::Stab, 100, true>::genre) == std::set<int32_t>{6}).to_be_true();
	});

	it("lets no note held back over a Section line enter late on an avoided line", _ {
		// A Loop note from step 2 with a 30-step gate is cut on the next bar line, a kick step. On a Section line the
		// cut holds it back first: with no rule it enters late there, with Avoid it doesn't.
		auto lateOnLines = [](const Genre& genre) {
			Result result = play(avoidRun(genre, 1, 50, 25, 25), 32);
			int32_t late = 0;
			for (const Event& e : only(result.events, EventKind::Section)) {
				late += ons(result, kBassPart, Lane::None, e.tick / kTicksPerStep, e.tick / kTicksPerStep + 1).size();
			}
			return late;
		};
		expect(lateOnLines(LongSource<Flavour::Loop, 0>::genre)).to_be_greater_than(0);
		expect(lateOnLines(LongSource<Flavour::Loop, 100>::genre)).to_equal(0);
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

	it("compares a kick pattern shorter than a bar by where it repeats in the bar, and rejects Avoid on a Sustain", _ {
		// Patterns are written 16 or 32 steps long; the check still reads any length that divides the bar.
		auto shortened = [](int32_t length) {
			Pattern pattern{"X...............", "................", 0};
			pattern.length = static_cast<uint8_t>(length);
			return pattern;
		};
		static const Pattern kFourStep[] = {shortened(4)};  // a kick on every beat
		static const Pattern kEightStep[] = {shortened(8)}; // on steps 0 and 8
		static const Pattern kTwelveStep[] = {shortened(12)}; // drifts across the bar
		static constexpr Pattern kBassOn6[] = {{"......X.........", "................", 0}};
		static constexpr Pattern kBassOn4[] = {{"....X...........", "................", 0}};
		static constexpr Pattern kBassOn8[] = {{"........X.......", "................", 0}};
		auto valid = [](std::span<const Pattern> kick, std::span<const Pattern> bass) {
			static LaneSpec lanes[1];
			lanes[0] = {.lane = Lane::Kick, .pool = kick};
			static PartSpec parts[2];
			parts[0] = {.role = Role::Drums, .lanes = lanes};
			parts[1] = AvoidKick<100>::parts[1];
			parts[1].pool = bass;
			Genre genre = AvoidKick<100>::genre;
			genre.parts = parts;
			return avoidRulesValid(genre);
		};
		expect(valid(kFourStep, kBassOn6)).to_be_true();
		expect(valid(kFourStep, kBassOn4)).to_be_false();
		expect(valid(kEightStep, kBassOn4)).to_be_true();
		expect(valid(kEightStep, kBassOn8)).to_be_false();
		expect(valid(kTwelveStep, kBassOn6)).to_be_false();
		// A Sustain lists no Avoid rule.
		static PartSpec pad[2] = {AvoidKick<100>::parts[0], {.role = Role::Pad, .flavour = Flavour::Sustain,
		                                                      .rules = AvoidKick<100>::rules}};
		Genre genre = AvoidKick<100>::genre;
		genre.parts = pad;
		expect(avoidRulesValid(genre)).to_be_false();
		// Nor does an Avoid source that may flam: its grace hit would start inside the avoided 16th.
		static PartSpec flams[2] = {AvoidKick<100>::parts[0], AvoidKick<100>::parts[1]};
		flams[1].expression.ornaments = {.rateFloor = 100, .rateCeiling = 100, .flam = 1};
		genre.parts = flams;
		expect(avoidRulesValid(genre)).to_be_false();
	});
});

CPPSPEC_SPEC(avoid)
