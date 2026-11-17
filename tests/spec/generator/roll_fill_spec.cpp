// The Roll fill (spec §8.7): a short Build-style ramp on one Drums lane, 16ths from its first step and the genre's
// top roll stage for its last quarter, climbing from normal to accent velocity and ending on the phrase line. It
// ignores Density and Complexity and plays no Lean, spread or Ornament, but obeys its lane's and part's gates, mute
// and keeps. Held, it plays 16ths for a beat, then the top stage until release. The Build roll wins on its own lane.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 2 * kStepsPerBar;
constexpr uint8_t kDrumsPart = 0;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kBackbeat[] = {{"....X.......X...", "................", 0}};
constexpr Pattern kOffBeats[] = {{"..9...9...9...9.", "................", 0}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuild2[] = {{2, 1}};

constexpr int32_t kNormal = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Normal)];
constexpr int32_t kAccent = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Accent)];

/// A Roll of `Beats` on `RollLane`, the only fill entry, at every phrase's end. The hat leans its & and ratchets 4
/// times whenever it can, gated at `HatGate` with its part at `PartGate`; the snare is the Build roll's lane. A Break
/// keeps only the kick.
template <Lane RollLane, uint8_t Beats, RollStage Top = RollStage::ThirtySecond, int16_t HatGate = 0,
          int16_t PartGate = 0>
struct RollGenre {
	static constexpr LaneSpec lanes[] = {
	    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
	    {.lane = Lane::Snare, .pool = kBackbeat},
	    {.lane = Lane::ClosedHat,
	     .pool = kOffBeats,
	     .densityGate = macroPoints(HatGate),
	     .expression = {.lean = {0, 0, 6, 0},
	                    .spread = 10,
	                    .ornaments = {.rateFloor = 1000, .rateCeiling = 1000, .ratchet = {0, 0, 1}}}},
	};
	static constexpr PartSpec parts[] = {{.role = Role::Drums, .lanes = lanes, .densityGate = macroPoints(PartGate)}};
	static constexpr FillEntry fills[] = {
	    {.kind = FillKind::Roll, .mask = {.lanes = laneBit(RollLane)}, .beats = Beats}};
	static constexpr Genre genre = {
	    .name = "roll",
	    .facts = {.scales = kScales},
	    .phraseBars = 2,
	    .parts = parts,
	    .fills = fills,
	    .fillChance = 1000,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {4, 4},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuild2,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	            .breakKeeps = {.lanes = laneBit(Lane::Kick)},
	            .buildKeeps = kEverything,
	            .buildCut = {},
	            .gap = GapLength::None,
	            .roll = {.lane = Lane::Snare, .top = Top},
	        },
	};
};

/// From the first Groove, at the Energy centre so velocities are the flavours' own.
Run rollRun(const Genre& genre, int32_t density = 50, int32_t complexity = 25) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Energy, kMacroFull / 2);
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, macroPoints(25));
	run.skipIntro = true;
	return run;
}

/// Whether `lane` plays a ramp on [from, from + steps): one hit per step, then `top` hits per step from `topFrom`
/// steps in, evenly spaced from the line; velocity from normal to accent over the first `climb` steps, then accent.
bool rampAt(const Result& result, Lane lane, int32_t from, int32_t steps, int32_t topFrom, int32_t top, int32_t climb) {
	for (int32_t i = 0; i < steps; i++) {
		const std::vector<Event> hits = ons(result, kDrumsPart, lane, from + i, from + i + 1);
		const int32_t count = i < topFrom ? 1 : top;
		const int32_t base = i < climb ? kNormal + (kAccent - kNormal) * i / (climb - 1) : kAccent;
		if (static_cast<int32_t>(hits.size()) != count) {
			return false;
		}
		for (int32_t k = 0; k < count; k++) {
			if (hits[k].tick != stepTick(from + i) + k * (kTicksPerStep / count) || hits[k].velocity != base) {
				return false;
			}
		}
	}
	return true;
}

/// The hat's ramp for an automatic Roll of `beats` ending on the phrase line `end`, at top stage `top` hits a step.
bool autoRollEndingAt(const Result& result, int32_t end, int32_t beats, int32_t top) {
	const int32_t steps = beats * kStepsPerBeat;
	return rampAt(result, Lane::ClosedHat, end - steps, steps, steps * 3 / 4, top, steps);
}

} // namespace

// clang-format off
describe roll_fill("The Roll fill", $ {
	it("ramps 16ths to the top stage for its last quarter, normal to accent, ending on the phrase line", _ {
		auto check = [&](const Genre& genre, int32_t beats, int32_t top) {
			Result result = play(rollRun(genre), 4 * 2);
			for (int32_t end : {kPhraseSteps, 2 * kPhraseSteps, 3 * kPhraseSteps}) {
				expect(autoRollEndingAt(result, end, beats, top)).to_be_true();
				// The hat's own pattern up to the Roll, and again from the phrase line.
				const int32_t start = end - beats * kStepsPerBeat;
				expect(ons(result, kDrumsPart, Lane::ClosedHat, start - 2, start).size()).to_be_greater_than(size_t{0});
				expect(ons(result, kDrumsPart, Lane::ClosedHat, end, end + 2).empty()).to_be_true();
				expect(ons(result, kDrumsPart, Lane::ClosedHat, end + 2, end + 3).empty()).to_be_false();
			}
		};
		check(RollGenre<Lane::ClosedHat, 1>::genre, 1, 2);
		check(RollGenre<Lane::ClosedHat, 2>::genre, 2, 2);
		check(RollGenre<Lane::ClosedHat, 4>::genre, 4, 2);
		check(RollGenre<Lane::ClosedHat, 4, RollStage::SixtyFourth>::genre, 4, 4);
		check(RollGenre<Lane::ClosedHat, 2, RollStage::Sixteenth>::genre, 2, 1);
	});

	it("ignores Density thinning and Complexity", _ {
		for (int32_t density : {0, 50}) {
			for (int32_t complexity : {0, 50}) {
				Result result = play(rollRun(RollGenre<Lane::ClosedHat, 2>::genre, density, complexity), 4);
				expect(autoRollEndingAt(result, kPhraseSteps, 2, 2)).to_be_true();
			}
		}
	});

	it("is silent on a lane gated out, a part gated out, a muted part and a lane the keeps leave out", _ {
		auto hatsIn = [](const Result& result, int32_t from, int32_t to) {
			int32_t sounding = 0;
			for (const Event& e : ons(result, kDrumsPart, Lane::ClosedHat, from, to)) {
				sounding += e.muted ? 0 : 1;
			}
			return sounding;
		};
		const int32_t end = 2 * kPhraseSteps;
		expect(hatsIn(play(rollRun(RollGenre<Lane::ClosedHat, 2, RollStage::ThirtySecond, 30>::genre, 20), 4), 0, end))
		    .to_equal(0);
		expect(hatsIn(play(rollRun(RollGenre<Lane::ClosedHat, 2, RollStage::ThirtySecond, 0, 30>::genre, 20), 4), 0, end))
		    .to_equal(0);
		Run muted = rollRun(RollGenre<Lane::ClosedHat, 2>::genre);
		muted.steer = steerAt({{0, mute(kDrumsPart, true)}});
		expect(hatsIn(play(muted, 4), 0, end)).to_equal(0);
		// Held through a Break that keeps only the kick: no hat, and the kick plays on.
		Run held = rollRun(RollGenre<Lane::ClosedHat, 2>::genre);
		held.steer = steerAt({{4 * kPhraseSteps - 2, force(SectionKind::Break)},
		                      {4 * kPhraseSteps + 4, {CommandKind::SetFill, 0, kFillHeldEntry}}});
		Result kept = play(held, 6 * 2);
		const int32_t breakStart = sectionStart(kept, SectionKind::Break) / kTicksPerStep;
		expect(breakStart).to_be_greater_than(0);
		expect(hatsIn(kept, breakStart, breakStart + kPhraseSteps)).to_equal(0);
		expect(ons(kept, kDrumsPart, Lane::Kick, breakStart + 8, breakStart + 9).size()).to_equal(size_t{1});
	});

	it("held, plays 16ths for a beat climbing to accent, then the top stage until release", _ {
		// A 1-beat automatic Roll, so the phrase's own ramp starts well after the release.
		Run run = rollRun(RollGenre<Lane::ClosedHat, 1>::genre);
		const int32_t press = kPhraseSteps + 3;
		const int32_t release = press + 12;
		run.steer = steerAt({{press, {CommandKind::SetFill, 0, kFillFreshPick}},
		                     {release, {CommandKind::SetFill, 0, kFillReleased}}});
		Result result = play(run, 4 * 2);
		// The first step with a single hit starts it: the hat's own plays 4 ratchet hits on each &.
		int32_t from = press;
		while (from < release && ons(result, kDrumsPart, Lane::ClosedHat, from, from + 1).size() != 1) {
			from++;
		}
		int32_t to = from;
		while (to < release + 4 && !ons(result, kDrumsPart, Lane::ClosedHat, to, to + 1).empty()
		       && (to < from + kStepsPerBeat || ons(result, kDrumsPart, Lane::ClosedHat, to, to + 1).size() == 2)) {
			to++;
		}
		expect(from).to_be_less_than(press + 2);
		expect(to - from).to_be_greater_than(release - press - 2);
		expect(rampAt(result, Lane::ClosedHat, from, to - from, kStepsPerBeat, 2, kStepsPerBeat)).to_be_true();
		// It ends with the release, before the phrase's own Roll.
		expect(to).to_be_less_than(release + 3);
	});

	it("gives way to the Build roll on its own lane, and plays on the roll lane outside it", _ {
		using OnSnare = RollGenre<Lane::Snare, 2>;
		Result automatic = play(rollRun(OnSnare::genre), 4);
		expect(rampAt(automatic, Lane::Snare, kPhraseSteps - 8, 8, 6, 2, 8)).to_be_true();
		// Held through the Build: the snare plays the Build roll exactly as without it, once the roll has started.
		Run plainRun = rollRun(OnSnare::genre, 50, 50); // at Complexity 50 the roll starts on the Build's first step
		plainRun.steer = steerAt({{4 * kPhraseSteps - 2, force(SectionKind::Build)}});
		Result plain = play(plainRun, 6 * 2);
		Run heldRun = plainRun;
		heldRun.steer = steerAt({{4 * kPhraseSteps - 2, force(SectionKind::Build)},
		                         {4 * kPhraseSteps + 2, {CommandKind::SetFill, 0, kFillHeldEntry}}});
		Result held = play(heldRun, 6 * 2);
		const int32_t build = sectionStart(plain, SectionKind::Build) / kTicksPerStep;
		expect(build).to_be_greater_than(0);
		expect(sectionStart(held, SectionKind::Build) / kTicksPerStep).to_equal(build);
		expect(ons(held, kDrumsPart, Lane::Snare, build, build + 2 * kStepsPerBar)
		       == ons(plain, kDrumsPart, Lane::Snare, build, build + 2 * kStepsPerBar)).to_be_true();
	});
});

CPPSPEC_SPEC(roll_fill)
