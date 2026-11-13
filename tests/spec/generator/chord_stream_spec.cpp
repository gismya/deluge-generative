// The Chord-graph walk in the Generator (spec §7.4, §9.7): when chords change, the walk rules the Arc drives (the
// piece's first chord, Drops, cadences counted from each Section start, a phrase's last chord), the Loop bass
// following the chord root, key changes keeping the walk's place, and Techno's empty Harmony slot staying home.
// The genres here are harness-only: no shipping genre walks a graph yet.

#include "chord_moves.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <memory>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kStepsPerPhrase = 8 * kStepsPerBar;

/// A Bass that plays the chord root on every step, so each note's pitch class names the chord.
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr DegreeWeight kRootOnly[] = {{0, 1}};
constexpr PartSpec kRootBass[] = {{
    .role = Role::Bass,
    .flavour = Flavour::Loop,
    .name = "BASS",
    .pool = kEveryStep,
    .loop = {.degrees = kRootOnly, .octaveUp = 0, .octaveDown = 0, .slide = 0},
}};

constexpr ArcSpec kArc{.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}};

using chord_specs::kMelodicMoves;

constexpr ChordGraph kTwoBars{.homePull = 10, .moves = kMelodicMoves, .cadenceTarget = 6, .cadencePeriod = 4};
constexpr Genre kWalking{.name = "walking", .parts = kRootBass, .harmony = {.graph = &kTwoBars}, .arc = kArc};

constexpr ChordGraph kSixteenBars{.homePull = 10, .moves = kMelodicMoves, .cadenceTarget = 6, .barsPerChord = 16};
constexpr Genre kSlow{.name = "slow", .parts = kRootBass, .harmony = {.graph = &kSixteenBars}, .arc = kArc};
/// A Build shorter than the phrase starts mid-phrase (spec §9.3), inside a 16-bar chord.
constexpr WeightedBars kHalfPhraseBuild[] = {{4, 1}};
constexpr Genre kSlowShortBuild{
    .name = "slow, short Build",
    .parts = kRootBass,
    .harmony = {.graph = &kSixteenBars},
    .arc = {.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}, .buildBars = kHalfPhraseBuild},
};

/// Home stays (repeat 100) or steps to V and back, so only the phrase rules keep it off home: a phrase's last chord
/// can't be home, and leaves it for V. No cadence target.
constexpr ChordMove kHomeOrV[] = {{0, 4, 20}, {4, 0, 100}};
constexpr ChordGraph kFourBars{.moves = kHomeOrV, .repeatWeight = 100, .barsPerChord = 4};
constexpr WeightedBars kQuarterPhraseBuild[] = {{2, 1}};
constexpr Genre kFourBarsShortBuild{
    .name = "four-bar chords, 2-bar Build",
    .parts = kRootBass,
    .harmony = {.graph = &kFourBars},
    .arc = {.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}, .buildBars = kQuarterPhraseBuild},
};
constexpr ChordGraph kEightBars{.moves = kHomeOrV, .repeatWeight = 100, .barsPerChord = 8};
constexpr Genre kPhraseChordsShortBuild{
    .name = "phrase-long chords, 4-bar Build",
    .parts = kRootBass,
    .harmony = {.graph = &kEightBars},
    .arc = {.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}, .buildBars = kHalfPhraseBuild},
};

/// Home goes to III; only the Drop cadence also offers V, the target. Off home, home pulls hard, but a phrase's last
/// chord can't be home, and III, with nothing else weighing, stays. 16-bar Builds, so a forced Groove can end one
/// early.
constexpr ChordMove kHomeToIII[] = {{0, 2, 40}};
constexpr ChordGraph kDropOnlyCadence{.homePull = 100, .moves = kHomeToIII, .cadenceTarget = 4};
constexpr WeightedBars kTwoPhraseBuild[] = {{16, 1}};
constexpr Genre kForcedDrops{
    .name = "forced Drops",
    .parts = kRootBass,
    .harmony = {.graph = &kDropOnlyCadence},
    .arc = {.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}, .buildBars = kTwoPhraseBuild},
};

/// 16-bar chords that leave home only for the Drop cadence, to V, which goes straight home. Grooves go straight into a
/// 4-bar Build, so a Groove ends mid-phrase, often in the phrase after its last chord's line.
constexpr ChordGraph kSlowDropCadence{.homePull = 100, .cadenceTarget = 4, .barsPerChord = 16};
constexpr Genre kSlowGrooveIntoShortBuild{
    .name = "slow, Groove into a short Build",
    .parts = kRootBass,
    .harmony = {.graph = &kSlowDropCadence},
    .arc = {.introPhrases = 2, .groovePhrases = {3, 5}, .buildBars = kHalfPhraseBuild, .grooveToBuild = 1000},
};

/// Two-bar chords that stay home or step to V and back, so only a phrase's last chord keeps them off home.
constexpr ChordGraph kTwoBarsHomeOrV{.moves = kHomeOrV, .repeatWeight = 100};
constexpr Genre kTwoBarsHeld{
    .name = "two-bar chords, home or V",
    .parts = kRootBass,
    .harmony = {.graph = &kTwoBarsHomeOrV},
    .arc = kArc,
};

/// A graph that only ever leaves home for a cadence: home stays put (nothing else weighs), every 3rd chord since the
/// Section start and the last before a Drop go to V, and V goes straight home. One chord per phrase, so no phrase
/// rules apply, and a two-phrase Build, so its last chord isn't also its Section start.
constexpr ChordGraph kCadenceOnly{.homePull = 100, .cadenceTarget = 4, .cadencePeriod = 3, .barsPerChord = 8};
constexpr WeightedBars kPhraseBuild[] = {{16, 1}};
constexpr Genre kCadences{
    .name = "cadences",
    .parts = kRootBass,
    .harmony = {.graph = &kCadenceOnly},
    .arc = {.introPhrases = 2, .groovePhrases = {4, 5}, .breakPhrases = {4, 4}, .buildBars = kPhraseBuild},
};

Recipe recipeFor(const Genre& genre, Scale scale = Scale::Minor) {
	Recipe recipe = technoRecipe();
	recipe.genre = &genre;
	recipe.scale = scale;
	return recipe;
}

/// What each decided step played: the chord, where the Arc stood, and the Bass's note-ons.
struct Walked {
	struct Step {
		int32_t chord = 0;
		SectionKind section = SectionKind::Intro;
		bool sectionStart = false;
		bool drop = false;
		int32_t stepInPhrase = 0;
		int32_t phraseInSection = 0;
		std::vector<uint8_t> notes;
	};
	std::vector<Step> steps;
	std::unique_ptr<Generator> generator;

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

/// Decides `count` steps of `recipe` straight through the core, with `steer` pushing commands before each, as the
/// Generator stands after the step before.
Walked walk(const Recipe& recipe, uint32_t seed, int32_t count,
            const std::function<void(int32_t, const Generator&, SteeringQueue&)>& steer = {}) {
	Walked out;
	out.generator = std::make_unique<Generator>();
	Generator& generator = *out.generator;
	generator.start(recipe, seed, Takes{}, false);
	SteeringQueue queue;
	for (int32_t n = 0; n < count; n++) {
		if (steer) {
			steer(n, generator, queue);
		}
		generator.decideNextStep(queue, nullptr);
		const Arc& arc = generator.arc();
		Walked::Step step{generator.chordRoot(), arc.section(),      arc.sectionStartsHere(),
		                  arc.dropStartsHere(),  arc.stepInPhrase(), arc.phraseInSection()};
		Event event;
		while (generator.popDue(INT32_MAX, &event)) {
			if (event.kind == EventKind::NoteOn && event.tick < stepTick(n + 1)) {
				step.notes.push_back(event.pitch);
			}
		}
		out.steps.push_back(step);
	}
	return out;
}

} // namespace

// clang-format off
describe chord_stream("The Chord-graph walk in the Generator", $ {
	it("starts the piece on the home chord and changes chord only on the harmonic rhythm's bar lines", _ {
		for (uint32_t seed = 1; seed <= 8; seed++) {
			Walked walked = walk(recipeFor(kWalking), seed, 64 * kStepsPerBar);
			expect(walked.steps[0].chord).to_equal(kHomeChord);
			std::set<int32_t> chords;
			for (int32_t n : walked.changes()) {
				const Walked::Step& step = walked.steps[n];
				expect(step.stepInPhrase % (2 * kStepsPerBar) == 0 || (step.sectionStart && step.stepInPhrase % kStepsPerBar == 0))
				    .to_be_true();
				chords.insert(step.chord);
			}
			expect(chords.size()).to_be_greater_than(size_t{2});
		}
	});

	it("lands every chord of a harmonic rhythm longer than the phrase on a phrase line, and holds it across one", _ {
		for (uint32_t seed = 1; seed <= 8; seed++) {
			Walked walked = walk(recipeFor(kSlow), seed, 64 * kStepsPerBar);
			const std::vector<int32_t> changes = walked.changes();
			expect(changes.empty()).to_be_false();
			for (int32_t n : changes) {
				const Walked::Step& step = walked.steps[n];
				expect(step.stepInPhrase).to_equal(0);
				expect(step.sectionStart || step.phraseInSection % 2 == 0).to_be_true();
			}
		}
	});

	it("changes a chord longer than the phrase only on phrase lines when a Build starts mid-phrase", _ {
		int32_t midPhraseBuilds = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			Walked walked = walk(recipeFor(kSlowShortBuild), seed, 96 * kStepsPerBar);
			for (int32_t n : walked.changes()) {
				expect(walked.steps[n].stepInPhrase).to_equal(0);
			}
			for (const Walked::Step& step : walked.steps) {
				midPhraseBuilds += step.sectionStart && step.stepInPhrase != 0 ? 1 : 0;
				if (step.drop) {
					expect(step.chord).to_equal(kHomeChord);
				}
			}
		}
		expect(midPhraseBuilds).to_be_greater_than(0);
	});

	it("plays home on every Drop", _ {
		int32_t drops = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			for (const Walked::Step& step : walk(recipeFor(kWalking), seed, 96 * kStepsPerBar).steps) {
				if (step.drop) {
					drops++;
					expect(step.chord).to_equal(kHomeChord);
				}
			}
		}
		expect(drops).to_be_greater_than(0);
	});

	it("never plays home on a phrase's last chord when the phrase holds several", _ {
		for (uint32_t seed = 1; seed <= 8; seed++) {
			for (const Walked::Step& step : walk(recipeFor(kWalking), seed, 64 * kStepsPerBar).steps) {
				if (step.stepInPhrase >= 6 * kStepsPerBar) {
					expect(step.chord != kHomeChord).to_be_true();
				}
			}
		}
	});

	it("works out a phrase's last chord from the next chord line, a Section start included", _ {
		// 4-bar chords: a 2-bar Build at bar 6 makes the bar-6 chord the phrase's last, and frees the bar-4 one.
		int32_t freed = 0;
		int32_t builds = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked = walk(recipeFor(kFourBarsShortBuild), seed, 128 * kStepsPerBar);
			for (size_t n = 0; n < walked.steps.size(); n++) {
				const Walked::Step& step = walked.steps[n];
				if (step.sectionStart && step.section == SectionKind::Build) {
					expect(step.stepInPhrase).to_equal(6 * kStepsPerBar);
					expect(step.chord != kHomeChord).to_be_true();
					freed += walked.steps[n - 2 * kStepsPerBar].chord == kHomeChord ? 1 : 0;
					builds++;
				}
			}
		}
		expect(builds).to_be_greater_than(4);
		expect(freed).to_be_greater_than(0);
	});

	it("applies the phrase rules to a phrase that a Section start splits into two chords", _ {
		// One chord per phrase, but a 4-bar Build at bar 4 makes two: the second is the phrase's last.
		int32_t builds = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			for (const Walked::Step& step : walk(recipeFor(kPhraseChordsShortBuild), seed, 128 * kStepsPerBar).steps) {
				if (step.section == SectionKind::Build) {
					expect(step.chord != kHomeChord).to_be_true();
					builds += step.sectionStart ? 1 : 0;
				}
			}
		}
		expect(builds).to_be_greater_than(4);
	});

	it("leans to the cadence target before a Drop that a forced Groove brings early", _ {
		// Forced a bar into a 16-bar Build, the Groove lands on the Build's second phrase line as a Drop.
		int32_t forced = 0;
		int32_t toTarget = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked =
			    walk(recipeFor(kForcedDrops), seed, 128 * kStepsPerBar, [](int32_t, const Generator& generator, SteeringQueue& queue) {
				    const Arc& arc = generator.arc();
				    if (arc.section() == SectionKind::Build && arc.stepInSection() == kStepsPerBar) {
					    queue.push(force(SectionKind::Groove));
				    }
			    });
			for (size_t n = 1; n < walked.steps.size(); n++) {
				const Walked::Step& before = walked.steps[n - 1];
				if (walked.steps[n].drop && before.section == SectionKind::Build && before.phraseInSection == 0) {
					expect(before.chord == 2 || before.chord == 4).to_be_true();
					toTarget += before.chord == 4 ? 1 : 0;
					forced++;
				}
			}
		}
		expect(forced).to_be_greater_than(4);
		expect(toTarget).to_be_greater_than(0);
	});

	it("leans to the cadence target on a long chord whose Groove ends in a later phrase, into a short Build", _ {
		// The last chord before the Drop is the one on the Groove's last even phrase line: the Build starts mid-phrase
		// and draws none, whichever of the chord's two phrases it starts in. Motion 25 and below, so no Groove after a
		// Drop ends in its first two phrases, where the last chord is the Drop's home.
		int32_t drops = 0;
		int32_t laterPhrase = 0;
		for (int32_t points : {1, 10, 25}) {
			for (uint32_t seed = 1; seed <= 8; seed++) {
				Recipe recipe = recipeFor(kSlowGrooveIntoShortBuild);
				recipe.macros.set(Macro::Motion, macroPoints(points));
				const Walked walked = walk(recipe, seed, 192 * kStepsPerBar);
				for (size_t n = 1; n < walked.steps.size(); n++) {
					if (!walked.steps[n].drop) {
						continue;
					}
					expect(walked.steps[n - 1].chord).to_equal(4);
					drops++;
					const Walked::Step& groove = walked.steps[n - 4 * kStepsPerBar - 1];
					laterPhrase += groove.section == SectionKind::Groove && groove.phraseInSection % 2 == 1 ? 1 : 0;
				}
			}
		}
		expect(drops).to_be_greater_than(20);
		expect(laterPhrase).to_be_greater_than(4);
	});

	it("keeps a phrase's last chord off home once a Section held past its length is released", _ {
		// The Intro is held for 4 phrases and released in bar 1: its last phrase still has a last chord.
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked =
			    walk(recipeFor(kTwoBarsHeld), seed, 48 * kStepsPerBar, [](int32_t n, const Generator&, SteeringQueue& queue) {
				    if (n == 1 || n == 4 * kStepsPerPhrase + kStepsPerBar + 5) {
					    queue.push({CommandKind::SetHold, 0, n == 1 ? 1 : 0});
				    }
			    });
			expect(walked.steps[5 * kStepsPerPhrase].sectionStart).to_be_true();
			for (const Walked::Step& step : walked.steps) {
				if (step.stepInPhrase >= 6 * kStepsPerBar) {
					expect(step.chord != kHomeChord).to_be_true();
				}
			}
		}
	});

	it("counts cadences from each Section start, and leans to the cadence target before a Drop", _ {
		int32_t cadences = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walked walked = walk(recipeFor(kCadences), seed, 96 * kStepsPerBar);
			for (const Walked::Step& step : walked.steps) {
				if (step.stepInPhrase != 0) {
					continue;
				}
				// A Build's second phrase holds the last chord before its Drop.
				const bool cadence = step.section == SectionKind::Build ? step.phraseInSection == 1
				                                                        : (step.phraseInSection + 1) % 3 == 0;
				expect(step.chord).to_equal(cadence ? 4 : kHomeChord);
				cadences += cadence ? 1 : 0;
			}
		}
		expect(cadences).to_be_greater_than(8);
	});

	it("moves the Loop bass with the chord root", _ {
		Recipe recipe = recipeFor(kWalking);
		Walked walked = walk(recipe, 3, 32 * kStepsPerBar);
		int32_t notes = 0;
		for (const Walked::Step& step : walked.steps) {
			const int32_t root =
			    (recipe.keyRoot + kScaleSemitones[static_cast<int32_t>(recipe.scale)][step.chord]) % 12;
			for (uint8_t pitch : step.notes) {
				expect(pitch % 12).to_equal(root);
				notes++;
			}
		}
		expect(notes).to_be_greater_than(100);
	});

	it("keeps the walk's place across a key change", _ {
		// A transposition keeps the scale, so every chord is the same degree as without it (spec §9.7).
		Walked plain = walk(recipeFor(kWalking), 5, 48 * kStepsPerBar);
		Walked moved = walk(recipeFor(kWalking), 5, 48 * kStepsPerBar, [](int32_t n, const Generator&, SteeringQueue& queue) {
			if (n == 3 * kStepsPerBar) {
				queue.push({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Minor)});
			}
		});
		expect(moved.generator->recipe().keyRoot).to_equal(uint8_t{2});
		for (size_t n = 0; n < plain.steps.size(); n++) {
			expect(moved.steps[n].chord).to_equal(plain.steps[n].chord);
		}
	});

	it("voices the playing chord for the parts that read it", _ {
		Walked walked = walk(recipeFor(kWalking), 2, 32 * kStepsPerBar);
		const int32_t chord = walked.steps.back().chord;
		const Chord voiced = walked.generator->chord();
		expect(static_cast<int32_t>(voiced.steps[0])).to_equal(chord);
		expect(walked.changes().empty()).to_be_false();
	});

	it("keeps the walk through a jump, so the chord after it is the one an unbroken run plays", _ {
		// A jump skips to the next bar line, so it crosses no chord line (spec §12.4).
		const int32_t end = 40 * kStepsPerBar;
		Walked plain = walk(recipeFor(kWalking), 7, end);
		Generator jumped;
		jumped.start(recipeFor(kWalking), 7, Takes{}, false);
		SteeringQueue queue;
		for (int32_t n = 0; n < 5 * kStepsPerBar + 3; n++) {
			jumped.decideNextStep(queue, nullptr);
		}
		jumped.flush(stepTick(jumped.nextStep()), Generator::FlushFor::Jump);
		jumped.skipTo(6 * kStepsPerBar);
		while (jumped.nextStep() < end) {
			jumped.decideNextStep(queue, nullptr);
			Event event;
			while (jumped.popDue(INT32_MAX, &event)) {}
			expect(jumped.chordRoot()).to_equal(plain.steps[jumped.nextStep() - 1].chord);
		}
	});

	it("keeps Techno, with its empty Harmony slot, on the home chord", _ {
		for (const Walked::Step& step : walk(technoRecipe(), 4, 96 * kStepsPerBar).steps) {
			expect(step.chord).to_equal(kHomeChord);
		}
	});
});

CPPSPEC_SPEC(chord_stream)
