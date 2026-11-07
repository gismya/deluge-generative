// Section rules in the event stream (spec §8.3, §9.5): Break and Build keeps, the Build cut, the Gap, the Build
// roll, FX-lane timing and the Phrase accent.

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;
constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kHalfNotes[] = {{"X.......X.......", "................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
    {.lane = Lane::Snare}, // the roll lane: only section rules play it
    {.lane = Lane::ClosedHat, .pool = kEveryStep},
    {.lane = Lane::Crash}, // the accent lane
};
constexpr LaneSpec kFx[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kHalfNotes,
        .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 0, .gateTicks = 12 * kTicksPerStep},
    },
    {.role = Role::FX, .name = "FX", .lanes = kFx},
};
constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr uint8_t kFxPart = 2;
constexpr Scale kScales[] = {Scale::Minor};

/// Intro, Groove and Break a phrase each, then a `Bars` Build ending on the Drop: Groove at step 128, Break at 256,
/// and an 8- or 16-bar Build from 384. Flat macros, so only the rules change what plays.
template <uint8_t Bars>
struct Rules {
	static constexpr WeightedBars kBuild[] = {{Bars, 1}};
	static constexpr ArcSpec arc(GapLength gap, uint8_t cutBars) {
		return {
		    .introPhrases = 1,
		    .groovePhrases = {1, 1},
		    .breakPhrases = {1, 1},
		    .buildBars = kBuild,
		    .dropLiftEnergy = 0,
		    .dropLiftDensity = 0,
		    .offsets = {},
		    .breakKeeps = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::ClosedHat)},
		    .buildCut = {.lanes = laneBit(Lane::Kick)},
		    .buildCutBars = cutBars,
		    .gap = gap,
		    .roll = {.lane = Lane::Snare},
		    .accentLane = Lane::Crash,
		};
	}
	static constexpr Genre genre(GapLength gap = GapLength::Beat, uint8_t cutBars = 1) {
		return {.name = "rules", .facts = {.scales = kScales}, .parts = kParts, .arc = arc(gap, cutBars)};
	}
};

constexpr int32_t kBreak = 2 * kPhraseSteps;
constexpr int32_t kBuild8 = 3 * kPhraseSteps; // a Build of 8 or 16 bars starts on the phrase line after the Break
constexpr int32_t kDrop8 = 4 * kPhraseSteps;  // the Drop after an 8-bar Build

Result playRules(const Genre& genre, int32_t bars, int32_t complexity = macroPoints(25),
                 int32_t density = macroPoints(25), int32_t motion = macroPoints(25)) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Complexity, complexity);
	run.recipe.macros.set(Macro::Density, density);
	run.recipe.macros.set(Macro::Motion, motion);
	return play(run, bars);
}

/// Note-ons of `part` (and `lane`, unless Lane::None stands for any) from step `from` up to `to`.
std::vector<Event> ons(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == part && (lane == Lane::None || e.lane == lane) && e.tick >= stepTick(from)
		    && e.tick < stepTick(to)) {
			out.push_back(e);
		}
	}
	return out;
}

int32_t sectionTick(const Result& result, SectionKind kind, uint16_t occurrence) {
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.section == kind && e.occurrence == occurrence) {
			return e.tick;
		}
	}
	return -1;
}

} // namespace

// clang-format off
describe section_rules("Section rules", $ {
	it("keep only the Break keeps playing in a Break, and the same set in a Build by default", _ {
		static constexpr Genre kGenre = Rules<8>::genre(GapLength::None, 0);
		Result result = playRules(kGenre, 6 * 8);
		expect(sectionTick(result, SectionKind::Break, 0)).to_equal(stepTick(kBreak));
		expect(sectionTick(result, SectionKind::Build, 0)).to_equal(stepTick(kBuild8));
		// Groove: everything plays.
		expect(ons(result, kBassPart, Lane::None, kPhraseSteps, kBreak).empty()).to_be_false();
		// Break and Build: the kick and the hats, but no Bass.
		for (int32_t from : {kBreak, kBuild8}) {
			expect(ons(result, kBassPart, Lane::None, from, from + kPhraseSteps).empty()).to_be_true();
			expect(ons(result, kDrumsPart, Lane::Kick, from, from + kPhraseSteps).size()).to_equal(size_t{32});
			expect(ons(result, kDrumsPart, Lane::ClosedHat, from, from + kPhraseSteps).size()).to_equal(size_t{128});
		}
		// Back on the Drop.
		expect(ons(result, kBassPart, Lane::None, kDrop8, kDrop8 + kStepsPerBar).empty()).to_be_false();
	});

	it("stop the Build cut's parts and lanes for its last bars, and silence everything in the Gap", _ {
		static constexpr Genre kGenre = Rules<8>::genre(GapLength::Beat, 1);
		Result result = playRules(kGenre, 6 * 8);
		const int32_t cut = kDrop8 - kStepsPerBar;
		const int32_t gap = kDrop8 - kStepsPerBeat;
		expect(ons(result, kDrumsPart, Lane::Kick, kBuild8, cut).size()).to_equal(size_t{28});
		expect(ons(result, kDrumsPart, Lane::Kick, cut, kDrop8).empty()).to_be_true();
		expect(ons(result, kDrumsPart, Lane::ClosedHat, cut, gap).size()).to_equal(size_t{12});
		for (uint8_t part : {kDrumsPart, kBassPart, kFxPart}) {
			expect(ons(result, part, Lane::None, gap, kDrop8).empty()).to_be_true();
		}
		expect(ons(result, kDrumsPart, Lane::Kick, kDrop8, kDrop8 + 1).size()).to_equal(size_t{1});
	});

	it("end the pitched notes sounding when the Gap starts", _ {
		// Every part kept in the Build, so the Bass's 12-step note from 8 steps before the Gap is sounding into it.
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<8>::genre(GapLength::Bar, 0);
			genre.arc.breakKeeps = kEverything;
			return genre;
		}();
		Result result = playRules(kGenre, 6 * 8);
		const int32_t gapTick = stepTick(kDrop8 - kStepsPerBar);
		int32_t atGap = 0;
		int32_t inGap = 0;
		for (const Event& e : only(result.events, EventKind::NoteOff)) {
			if (e.part == kBassPart) {
				atGap += e.tick == gapTick ? 1 : 0;
				inGap += e.tick > gapTick && e.tick < stepTick(kDrop8) ? 1 : 0;
			}
		}
		expect(atGap).to_equal(1);
		expect(inGap).to_equal(0);
	});

	it("roll from beats to 8ths, 16ths and 32nds at the stage fractions, whatever the Density", _ {
		// A 16-bar Build with no Gap: 8ths from bar 9, 16ths from bar 13 and 32nds from bar 15 (spec §8.3).
		static constexpr Genre kGenre = Rules<16>::genre(GapLength::None, 0);
		for (int32_t density : {0, macroPoints(25)}) {
			Result result = playRules(kGenre, 8 * 7, kMacroFull, density);
			const int32_t build = sectionTick(result, SectionKind::Build, 0) / kTicksPerStep;
			expect(build).to_equal(kBuild8);
			std::vector<int32_t> perBar(16);
			for (const Event& e : ons(result, kDrumsPart, Lane::Snare, build, build + 16 * kStepsPerBar)) {
				perBar[(e.tick / kTicksPerStep - build) / kStepsPerBar]++;
			}
			const std::vector<int32_t> expected = {4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 16, 16, 32, 32};
			expect(perBar == expected).to_be_true();
			expect(ons(result, kDrumsPart, Lane::Snare, 0, build).empty()).to_be_true();
		}
	});

	it("start the roll later at lower Complexity, joining at the stage reached, so the last bars sound the same", _ {
		static constexpr Genre kGenre = Rules<16>::genre(GapLength::None, 0);
		Result busy = playRules(kGenre, 8 * 7, kMacroFull);
		Result plain = playRules(kGenre, 8 * 7, 0);
		const int32_t lastBars = kBuild8 + 12 * kStepsPerBar;
		const int32_t drop = kBuild8 + 16 * kStepsPerBar;
		auto late = ons(plain, kDrumsPart, Lane::Snare, kBuild8, drop);
		expect(late.empty()).to_be_false();
		expect(late.size()).to_be_less_than(ons(busy, kDrumsPart, Lane::Snare, kBuild8, drop).size());
		// Hits and velocities match over the last 4 bars; Energy's layer is the same there.
		auto a = ons(plain, kDrumsPart, Lane::Snare, lastBars, drop);
		auto b = ons(busy, kDrumsPart, Lane::Snare, lastBars, drop);
		expect(a == b).to_be_true();
	});

	it("climb in velocity towards the Gap", _ {
		static constexpr Genre kGenre = Rules<16>::genre(GapLength::Beat, 0);
		Result result = playRules(kGenre, 8 * 7, kMacroFull);
		auto roll = ons(result, kDrumsPart, Lane::Snare, kBuild8, kBuild8 + 16 * kStepsPerBar);
		expect(roll.size()).to_be_greater_than(size_t{50});
		for (size_t i = 1; i < roll.size(); i++) {
			expect(roll[i].velocity).to_be_greater_than(roll[i - 1].velocity - 1);
		}
		expect(roll.back().velocity).to_be_greater_than(roll.front().velocity + 20);
	});

	it("play the Riser across the Build, the Fall on a Break's first step and the Impact on the Drop", _ {
		static constexpr Genre kGenre = Rules<8>::genre();
		Result result = playRules(kGenre, 6 * 8);
		const int32_t end = kDrop8 + kPhraseSteps; // before the next Break
		auto risers = ons(result, kFxPart, Lane::Riser, 0, end);
		expect(risers.size()).to_equal(size_t{1});
		expect(risers[0].tick).to_equal(stepTick(kBuild8));
		int32_t riserOff = -1;
		for (const Event& e : only(result.events, EventKind::NoteOff)) {
			if (e.lane == Lane::Riser && riserOff < 0) {
				riserOff = e.tick;
			}
		}
		expect(riserOff).to_equal(stepTick(kDrop8)); // it lasts through the Gap, to the Drop
		auto falls = ons(result, kFxPart, Lane::Fall, 0, end);
		expect(falls.size()).to_equal(size_t{1});
		expect(falls[0].tick).to_equal(stepTick(kBreak));
		auto impacts = ons(result, kFxPart, Lane::Impact, 0, end);
		expect(impacts.size()).to_equal(size_t{1});
		expect(impacts[0].tick).to_equal(stepTick(kDrop8));
	});

	it("gate FX lanes by Density like any lane", _ {
		static constexpr LaneSpec kGatedFx[] = {{.lane = Lane::Riser, .densityGate = macroPoints(40)},
		                                        {.lane = Lane::Fall},
		                                        {.lane = Lane::Impact, .densityGate = macroPoints(40)}};
		static constexpr PartSpec kGatedParts[] = {kParts[0], kParts[1], {.role = Role::FX, .lanes = kGatedFx}};
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<8>::genre();
			genre.parts = kGatedParts;
			return genre;
		}();
		Result result = playRules(kGenre, 6 * 8);
		expect(ons(result, kFxPart, Lane::Riser, 0, 6 * kPhraseSteps).empty()).to_be_true();
		expect(ons(result, kFxPart, Lane::Impact, 0, 6 * kPhraseSteps).empty()).to_be_true();
		expect(ons(result, kFxPart, Lane::Fall, 0, kDrop8).size()).to_equal(size_t{1});
	});

	it("accent the first step of every Drop, and other Groove phrases by a chance Motion scales", _ {
		static constexpr Genre kDropsOnly = Rules<8>::genre();
		Result result = playRules(kDropsOnly, 8 * 12);
		auto crashes = ons(result, kDrumsPart, Lane::Crash, 0, 12 * kPhraseSteps);
		std::vector<int32_t> drops;
		for (const Event& e : only(result.events, EventKind::Section)) {
			if (e.section == SectionKind::Groove && e.occurrence > 0) {
				drops.push_back(e.tick);
			}
		}
		expect(drops.size()).to_be_greater_than(size_t{1});
		expect(crashes.size()).to_equal(drops.size());
		for (size_t i = 0; i < crashes.size() && i < drops.size(); i++) {
			expect(crashes[i].tick).to_equal(drops[i]);
			expect(crashes[i].velocity).to_be_greater_than(uint8_t{kLevelVelocity[1]});
		}

		// A sure chance at Motion 25 accents every Groove phrase start; Motion 0 (no Drops either) never does.
		static constexpr Genre kAlways = [] {
			Genre genre = Rules<8>::genre();
			genre.arc.groovePhrases = {4, 4};
			genre.arc.accentChance = 1000;
			return genre;
		}();
		auto inGroove = ons(playRules(kAlways, 8 * 6), kDrumsPart, Lane::Crash, 0, 6 * kPhraseSteps);
		expect(inGroove.size()).to_equal(size_t{4}); // the first Groove's 4 phrase starts
		for (const Event& e : inGroove) {
			expect(e.tick % stepTick(kPhraseSteps)).to_equal(0);
		}
		Result still = playRules(kAlways, 8 * 6, macroPoints(25), macroPoints(25), 0);
		expect(ons(still, kDrumsPart, Lane::Crash, 0, 6 * kPhraseSteps).empty()).to_be_true();
	});

	it("put the Phrase accent through the lane's Density gate and Replace pairs like any hit", _ {
		static constexpr ReplacePair kCrashOverHat[] = {{Lane::Crash, Lane::ClosedHat}};
		static constexpr Genre kReplacing = [] {
			Genre genre = Rules<8>::genre();
			genre.replacePairs = kCrashOverHat;
			return genre;
		}();
		Result replaced = playRules(kReplacing, 6 * 8);
		expect(ons(replaced, kDrumsPart, Lane::Crash, kDrop8, kDrop8 + 1).size()).to_equal(size_t{1});
		expect(ons(replaced, kDrumsPart, Lane::ClosedHat, kDrop8, kDrop8 + 1).empty()).to_be_true();
		expect(ons(replaced, kDrumsPart, Lane::ClosedHat, kDrop8 + 1, kDrop8 + 2).size()).to_equal(size_t{1});

		static constexpr LaneSpec kGatedCrash[] = {kDrums[0], kDrums[1], kDrums[2],
		                                           {.lane = Lane::Crash, .densityGate = macroPoints(40)}};
		static constexpr PartSpec kGatedParts[] = {{.role = Role::Drums, .lanes = kGatedCrash}, kParts[1], kParts[2]};
		static constexpr Genre kGated = [] {
			Genre genre = Rules<8>::genre();
			genre.parts = kGatedParts;
			return genre;
		}();
		expect(ons(playRules(kGated, 6 * 8), kDrumsPart, Lane::Crash, 0, 6 * kPhraseSteps).empty()).to_be_true();
	});
});

CPPSPEC_SPEC(section_rules)
