// The Chord-graph walk in the Generator (spec §7.4, §9.7): when chords change, the walk rules the Arc drives (the
// piece's first chord, Drops, cadences counted from each Section start, a phrase's last chord), the Loop bass
// following the chord root, key changes keeping the walk's place, and Techno's empty Harmony slot staying home.
// The genres here are harness-only: no shipping genre walks a graph yet.

#include "harness.h"

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

/// A graph shaped like Melodic techno's (spec §4.5).
constexpr ChordMove kMoves[] = {
    {0, 5, 30}, // i–♭VI
    {5, 6, 30}, // ♭VI–♭VII
    {6, 0, 30}, // ♭VII–i
    {0, 3, 20}, // i–iv
    {3, 5, 20}, // iv–♭VI
    {0, 6, 20}, // i–♭VII
};
constexpr ArcSpec kArc{.introPhrases = 2, .groovePhrases = {3, 4}, .breakPhrases = {3, 3}};

constexpr ChordGraph kTwoBars{.homePull = 10, .moves = kMoves, .cadenceTarget = 6, .cadencePeriod = 4};
constexpr Genre kWalking{.name = "walking", .parts = kRootBass, .harmony = {.graph = &kTwoBars}, .arc = kArc};

constexpr ChordGraph kSixteenBars{.homePull = 10, .moves = kMoves, .cadenceTarget = 6, .barsPerChord = 16};
constexpr Genre kSlow{.name = "slow", .parts = kRootBass, .harmony = {.graph = &kSixteenBars}, .arc = kArc};

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

/// Decides `count` steps of `recipe` straight through the core, with `steer` pushing commands before each.
Walked walk(const Recipe& recipe, uint32_t seed, int32_t count,
            const std::function<void(int32_t, SteeringQueue&)>& steer = {}) {
	Walked out;
	out.generator = std::make_unique<Generator>();
	Generator& generator = *out.generator;
	generator.start(recipe, seed, Takes{}, false);
	SteeringQueue queue;
	for (int32_t n = 0; n < count; n++) {
		if (steer) {
			steer(n, queue);
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
		Walked moved = walk(recipeFor(kWalking), 5, 48 * kStepsPerBar, [](int32_t n, SteeringQueue& queue) {
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

	it("walks on through a jump, so the chord after it is the one an unbroken run plays", _ {
		const int32_t end = 40 * kStepsPerBar;
		Walked plain = walk(recipeFor(kWalking), 7, end);
		Generator jumped;
		jumped.start(recipeFor(kWalking), 7, Takes{}, false);
		SteeringQueue queue;
		for (int32_t n = 0; n < 5 * kStepsPerBar + 3; n++) {
			jumped.decideNextStep(queue, nullptr);
		}
		jumped.flush(stepTick(jumped.nextStep()), Generator::FlushFor::Jump);
		jumped.skipTo(24 * kStepsPerBar);
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
