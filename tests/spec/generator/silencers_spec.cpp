// Silencers meeting on one lane (spec §5.6, §5.7, §8.3, §8.7, §9.5, §12.5, §13.5): a held fill against the Break's
// keeps, a Replace pair whose replacing lane is rested, gated, silenced or rolling, a gated note held back under a
// mute, a Sustain under a Rest, the MUTE pad's dim in the cut and the Gap, and a rolling lane, which counts as
// sounding whatever its gate. Each spec reads the steps one at a time, so a note-on is read against the step that
// decided it.

#include "harness.h"

#include "cppspec.hpp"

#include <functional>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kOnTheAnds[] = {{"..X...X...X...X.", "................", 0}};
constexpr Pattern kSnareRun[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
/// A Bass note every 2 bars, lasting 4: one is always sounding when a gate closes on a bar line.
constexpr Pattern kEveryTwoBars[] = {{"X...............................", "................................", 0}};
constexpr LineWeight kRoot[] = {{0, 1}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
    {.lane = Lane::Snare, .fillPool = kSnareRun}, // plays only a Swap's fill
    {.lane = Lane::ClosedHat, .pool = kEveryStep},
    // Replaces the closed hat where it lands (the default pair), and is the roll lane.
    {.lane = Lane::OpenHat, .pool = kOnTheAnds, .densityGate = macroPoints(30)},
};
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kEveryTwoBars,
        .densityGate = macroPoints(30),
        .loop = {.degrees = kRoot, .draws = {.octaveUp = 0, .accent = 0}, .slide = 0, .gateTicks = 64 * kTicksPerStep},
    },
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD", .densityGate = macroPoints(30)},
};
constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr uint8_t kPadPart = 2;
constexpr Scale kScales[] = {Scale::Minor};

/// FILL's held entry: a Rest of the open hat, the Bass and the Pad, or a Swap of the snare.
constexpr FillEntry kRest[] = {
    {.kind = FillKind::Rest,
     .mask = {.parts = partBit(kBassPart) | partBit(kPadPart), .lanes = laneBit(Lane::OpenHat)}}};
constexpr FillEntry kSwap[] = {{.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare)}}};
constexpr WeightedBars kBuild8[] = {{8, 1}};

/// Intro, Groove and Break a phrase each at Motion 25, then an 8-bar Build: Groove at step 128, Break at 256, Build at
/// 384, its last bar cut (the kick and the closed hat) and its last beat the Gap, and the Drop at 512. The Break and
/// Build keep the kick and the closed hat; the open hat rolls in the Build. No automatic fills, flat macros.
constexpr Genre silencerGenre(std::span<const FillEntry> held) {
	return {
	    .name = "silencers",
	    .facts = {.scales = kScales},
	    .parts = kParts,
	    .fills = held,
	    .fillChance = 0,
	    .sectionEndChance = 0,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {1, 1},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuild8,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	            .breakKeeps = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::ClosedHat)},
	            .buildCut = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::ClosedHat)},
	            .buildCutBars = 1,
	            .gap = GapLength::Beat,
	            .roll = {.lane = Lane::OpenHat},
	        },
	};
}
constexpr Genre kRestGenre = silencerGenre(kRest);
constexpr Genre kSwapGenre = silencerGenre(kSwap);
/// The same, with a Build that keeps only the Pad: on DRUMS only the roll plays there.
constexpr Genre kRollOnlyGenre = [] {
	Genre genre = silencerGenre(kRest);
	genre.arc.buildKeeps = {.parts = partBit(kPadPart)};
	return genre;
}();

constexpr int32_t kGroove = kPhraseSteps;
constexpr int32_t kBreak = 2 * kPhraseSteps;
constexpr int32_t kBuild = 3 * kPhraseSteps;
constexpr int32_t kCut = 4 * kPhraseSteps - kStepsPerBar;
constexpr int32_t kGap = 4 * kPhraseSteps - kStepsPerBeat;
constexpr int32_t kDrop = 4 * kPhraseSteps;

/// One decided step: the note-ons it buffered and the parts the Arc silenced on it (partSilenced(), the dim).
struct Decided {
	std::vector<Event> ons;
	uint8_t silent = 0;
	SectionKind section = SectionKind::Intro;
	[[nodiscard]] int32_t count(uint8_t part, Lane lane = Lane::None) const {
		int32_t n = 0;
		for (const Event& e : ons) {
			n += e.part == part && (lane == Lane::None || e.lane == lane) ? 1 : 0;
		}
		return n;
	}
	[[nodiscard]] bool silenced(uint8_t part) const { return (silent & partBit(part)) != 0; }
	/// Whether every note-on of `part` here is flagged muted (spec §12.5), and there is one.
	[[nodiscard]] bool allMuted(uint8_t part) const {
		int32_t muted = 0;
		for (const Event& e : ons) {
			muted += e.part == part && e.muted ? 1 : 0;
		}
		return muted > 0 && muted == count(part);
	}
};

/// Complexity none, middle and full: it sets when the roll starts and how fast it gets (spec §8.3).
constexpr int32_t kComplexities[] = {0, 25, 50};

/// Decides `steps` steps of `genre` at `density`, with `steer` called before each, taking each step's events as it is
/// decided. Index n is step n. At `complexity` points, or the Recipe's if negative.
std::vector<Decided> decide(const Genre& genre, int32_t density, int32_t steps,
                            const std::function<void(int32_t, SteeringQueue&)>& steer = {}, int32_t complexity = -1) {
	Generator generator;
	SteeringQueue queue;
	Recipe recipe = newRecipe(genre, 0);
	recipe.macros.set(Macro::Density, macroPoints(density));
	recipe.macros.set(Macro::Motion, macroPoints(25));
	if (complexity >= 0) {
		recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	}
	generator.start(recipe, 1, {});
	std::vector<Decided> out;
	for (int32_t step = 0; step < steps; step++) {
		if (steer) {
			steer(step, queue);
		}
		generator.decideNextStep(queue, nullptr);
		Decided decided;
		decided.section = generator.arc().section();
		for (int32_t p = 0; p < partCount(genre); p++) {
			decided.silent |= generator.partSilenced(p) ? partBit(p) : 0;
		}
		Event e;
		while (generator.popDue(INT32_MAX, &e)) {
			if (e.kind == EventKind::NoteOn) {
				decided.ons.push_back(e);
			}
		}
		out.push_back(decided);
	}
	return out;
}

/// Note-ons of `part` (on `lane`, unless Lane::None) decided on the steps from `from` up to `to`.
int32_t countOf(const std::vector<Decided>& steps, uint8_t part, Lane lane, int32_t from, int32_t to) {
	int32_t n = 0;
	for (int32_t s = from; s < to; s++) {
		n += steps[s].count(part, lane);
	}
	return n;
}

/// Steps from `from` up to `to` where `part` shows silenced.
int32_t silencedSteps(const std::vector<Decided>& steps, uint8_t part, int32_t from, int32_t to) {
	int32_t n = 0;
	for (int32_t s = from; s < to; s++) {
		n += steps[s].silenced(part) ? 1 : 0;
	}
	return n;
}

Command fill(int32_t value) {
	return {CommandKind::SetFill, 0, value};
}
Command density(int32_t points) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), macroPoints(points)};
}

} // namespace

// clang-format off
describe silencers("Silencers on one lane", $ {
	it("start where the test genre says: Groove, Break, Build and the Drop", _ {
		const auto steps = decide(kRestGenre, 50, kDrop + 1);
		expect(steps[kGroove].section).to_equal(SectionKind::Groove);
		expect(steps[kBreak].section).to_equal(SectionKind::Break);
		expect(steps[kBuild].section).to_equal(SectionKind::Build);
		expect(steps[kDrop].section).to_equal(SectionKind::Groove);
	});

	it("let a held Swap play nothing on a lane the Break doesn't keep", _ {
		const auto steps = decide(kSwapGenre, 50, kBuild + kStepsPerBar,
		                          steerAt({{kGroove + 64, fill(kFillHeldEntry)}, {kBuild + 8, fill(kFillReleased)}}));
		// The held fill starts on the step decided after the press.
		expect(countOf(steps, kDrumsPart, Lane::Snare, kGroove, kGroove + 64)).to_equal(0);
		expect(countOf(steps, kDrumsPart, Lane::Snare, kGroove + 64, kBreak)).to_equal(64);
		expect(countOf(steps, kDrumsPart, Lane::Snare, kBreak, kBuild + 8)).to_equal(0);
		expect(countOf(steps, kDrumsPart, Lane::Kick, kBreak, kBuild)).to_equal(32);
		expect(silencedSteps(steps, kDrumsPart, kGroove, kBuild)).to_equal(0);
	});

	it("let a held Rest on a lane the Break doesn't keep leave the kept lanes as the Break plays them", _ {
		const auto steps = decide(kRestGenre, 50, kBuild,
		                          steerAt({{kGroove + 64, fill(kFillHeldEntry)}, {kBreak + 64, fill(kFillReleased)}}));
		// The rested open hat replaces nothing: the closed hat plays every step, in the Groove under the Rest and in
		// the Break, where the open hat isn't kept.
		expect(countOf(steps, kDrumsPart, Lane::OpenHat, kGroove + 64, kBuild)).to_equal(0);
		expect(countOf(steps, kDrumsPart, Lane::ClosedHat, kGroove + 64, kBreak)).to_equal(64);
		expect(countOf(steps, kDrumsPart, Lane::ClosedHat, kBreak, kBuild)).to_equal(128);
		expect(countOf(steps, kBassPart, Lane::None, kGroove + 64, kBuild)).to_equal(0);
		// A Rest doesn't dim the Bass's pad (spec §13.5); the Break, which doesn't keep it, does.
		expect(silencedSteps(steps, kBassPart, kGroove + 64, kBreak)).to_equal(0);
		expect(silencedSteps(steps, kBassPart, kBreak, kBuild)).to_equal(kPhraseSteps);
	});

	it("let a gated-out replacing lane replace nothing", _ {
		const auto steps = decide(kRestGenre, 50, kBreak, steerAt({{kGroove + 4, density(20)}}));
		expect(countOf(steps, kDrumsPart, Lane::OpenHat, kGroove, kGroove + 16)).to_equal(4);
		expect(countOf(steps, kDrumsPart, Lane::ClosedHat, kGroove, kGroove + 16)).to_equal(12);
		expect(countOf(steps, kDrumsPart, Lane::OpenHat, kGroove + 16, kBreak)).to_equal(0);
		expect(countOf(steps, kDrumsPart, Lane::ClosedHat, kGroove + 16, kBreak)).to_equal(112);
		expect(silencedSteps(steps, kDrumsPart, kGroove, kBreak)).to_equal(0);
	});

	it("let a rolling replacing lane replace where it hits, whatever its gate, at every Complexity", _ {
		// The steps before the cut where the open hat hits, by Complexity and Density. The higher the Complexity, the
		// earlier the roll starts; at full it rolls from the Build's start. With its gate open (Density 50) the open hat
		// plays its own pattern before that too; closed (20), only the roll.
		const int32_t kHits[3][2] = {{40, 16}, {48, 36}, {48, 48}};
		for (int32_t c = 0; c < 3; c++) {
			for (int32_t d = 0; d < 2; d++) {
				const auto steps = decide(kRestGenre, d == 0 ? 50 : 20, kDrop, {}, kComplexities[c]);
				int32_t hits = 0;
				int32_t closed = 0;
				for (int32_t s = kBuild; s < kCut; s++) {
					const int32_t rolls = steps[s].count(kDrumsPart, Lane::OpenHat);
					hits += rolls > 0 ? 1 : 0;
					closed += steps[s].count(kDrumsPart, Lane::ClosedHat);
					expect(rolls > 0 && steps[s].count(kDrumsPart, Lane::ClosedHat) > 0).to_be_false();
				}
				expect(hits).to_equal(kHits[c][d]);
				expect(hits + closed).to_equal(kCut - kBuild);
			}
		}
	});

	it("hold back a gated note under a mute, and bring it in late on the reopening line, flagged muted", _ {
		// The Bass's note from step 128 lasts to 192. A mute lands on 144 with the gate closing there; the gate
		// reopens on 176, where what it held back enters late, still muted. The unmute lands on 208.
		const auto steps = decide(kRestGenre, 50, kBreak,
		                          steerAt({{kGroove + 2, {CommandKind::SetMute, kBassPart, kPress}},
		                                   {kGroove + 4, density(20)},
		                                   {kGroove + 36, density(50)},
		                                   {kGroove + 66, {CommandKind::SetMute, kBassPart, kPress}}}));
		expect(countOf(steps, kBassPart, Lane::None, kGroove, kGroove + 1)).to_equal(1);
		expect(countOf(steps, kBassPart, Lane::None, kGroove + 1, kGroove + 48)).to_equal(0);
		expect(steps[kGroove + 48].count(kBassPart)).to_equal(1);
		expect(steps[kGroove + 48].allMuted(kBassPart)).to_be_true();
		expect(silencedSteps(steps, kBassPart, kGroove + 16, kGroove + 48)).to_equal(32);
		expect(silencedSteps(steps, kBassPart, kGroove + 48, kBreak)).to_equal(0);
		expect(steps[kGroove + 64].count(kBassPart)).to_equal(1);
		expect(steps[kGroove + 64].allMuted(kBassPart)).to_be_true();
		expect(steps[kGroove + 96].count(kBassPart)).to_equal(1);
		expect(steps[kGroove + 96].allMuted(kBassPart)).to_be_false();
	});

	it("bring a Sustain back after a Rest on the first bar line it isn't rested, and its gate on the one it reopens", _ {
		// A Rest from step 134 to 148: the Pad's voicing ends as it starts and comes back on 160.
		const auto rested = decide(kRestGenre, 50, kBreak,
		                           steerAt({{kGroove + 6, fill(kFillHeldEntry)}, {kGroove + 21, fill(kFillReleased)}}));
		expect(countOf(rested, kPadPart, Lane::None, kGroove, kGroove + 1)).to_be_greater_than(0);
		expect(countOf(rested, kPadPart, Lane::None, kGroove + 1, kGroove + 32)).to_equal(0);
		expect(countOf(rested, kPadPart, Lane::None, kGroove + 32, kGroove + 33)).to_be_greater_than(0);
		expect(silencedSteps(rested, kPadPart, kGroove, kBreak)).to_equal(0);
		// The same Rest with the gate closing on 144 and reopening on 176: the voicing comes back there.
		const auto gated = decide(kRestGenre, 50, kBreak,
		                          steerAt({{kGroove + 6, fill(kFillHeldEntry)}, {kGroove + 12, density(20)},
		                                   {kGroove + 21, fill(kFillReleased)}, {kGroove + 36, density(50)}}));
		expect(countOf(gated, kPadPart, Lane::None, kGroove + 1, kGroove + 48)).to_equal(0);
		expect(countOf(gated, kPadPart, Lane::None, kGroove + 48, kGroove + 49)).to_be_greater_than(0);
		expect(silencedSteps(gated, kPadPart, kGroove + 16, kGroove + 48)).to_equal(32);
	});

	it("dim in the cut only the parts with no lane sounding, and every part in the Gap", _ {
		const auto open = decide(kRestGenre, 50, kDrop);
		expect(silencedSteps(open, kDrumsPart, kBuild, kGap)).to_equal(0); // the roll sounds in the cut
		expect(silencedSteps(open, kBassPart, kBuild, kGap)).to_equal(kGap - kBuild);
		for (uint8_t part : {kDrumsPart, kBassPart, kPadPart}) {
			expect(silencedSteps(open, part, kGap, kDrop)).to_equal(kDrop - kGap);
		}
		// With the open hat's gate closed, the roll still plays in the cut, where nothing else on DRUMS may: a rolling
		// lane sounds whatever its gate, so DRUMS doesn't dim. At every Complexity it plays 32nds there, the cut's 12
		// steps before the Gap: Complexity moves only the roll's start.
		for (int32_t complexity : kComplexities) {
			const auto closed = decide(kRestGenre, 20, kDrop, {}, complexity);
			expect(countOf(closed, kDrumsPart, Lane::None, kCut, kGap)).to_equal(24);
			expect(countOf(closed, kDrumsPart, Lane::OpenHat, kCut, kGap)).to_equal(24);
			expect(silencedSteps(closed, kDrumsPart, kBuild, kGap)).to_equal(0);
		}
	});

	it("count a rolling lane as sounding whatever its gate, for the dim and the Arc plan, at every Complexity", _ {
		// The Build keeps nothing on DRUMS but the roll, and the open hat's gate stays closed at Density 20: DRUMS
		// dims in the Build until the roll starts, and the plan shows it playing there. The roll starts a quarter of
		// the Build before the Drop at Complexity 0, and earlier as it rises, from the Build's start at full.
		const int32_t kRollStarts[] = {kDrop - 32, kDrop - 80, kBuild};
		for (int32_t c = 0; c < 3; c++) {
			const auto steps = decide(kRollOnlyGenre, 20, kDrop, {}, kComplexities[c]);
			int32_t rollStart = kBuild;
			while (rollStart < kGap && steps[rollStart].count(kDrumsPart, Lane::OpenHat) == 0) {
				rollStart++;
			}
			expect(rollStart).to_equal(kRollStarts[c]);
			expect(countOf(steps, kDrumsPart, Lane::None, kBuild, rollStart)).to_equal(0);
			expect(silencedSteps(steps, kDrumsPart, kBuild, rollStart)).to_equal(rollStart - kBuild);
			expect(silencedSteps(steps, kDrumsPart, rollStart, kGap)).to_equal(0);
			expect(silencedSteps(steps, kDrumsPart, kGap, kDrop)).to_equal(kDrop - kGap);

			Generator generator;
			Recipe recipe = newRecipe(kRollOnlyGenre, 0);
			recipe.macros.set(Macro::Density, macroPoints(20));
			recipe.macros.set(Macro::Motion, macroPoints(25));
			recipe.macros.set(Macro::Complexity, macroPoints(kComplexities[c]));
			generator.start(recipe, 1, {});
			ArcPlan plan;
			generator.planArc(plan);
			expect(plan.at(3)->kind).to_equal(SectionKind::Build);
			expect(plan.at(3)->sounds & partBit(kDrumsPart)).to_equal(partBit(kDrumsPart));
			expect(plan.at(2)->kind).to_equal(SectionKind::Break);
			expect(plan.at(2)->sounds & partBit(kDrumsPart)).to_equal(partBit(kDrumsPart)); // the kept kick
		}
	});

});

CPPSPEC_SPEC(silencers)
