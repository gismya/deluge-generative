// Section rules in the event stream (spec §8.3, §9.5): Break and Build keeps, the Build cut, the Gap, the Build
// roll, FX-lane timing and the Phrase accent.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kHalfNotes[] = {{"X.......X.......", "................", 0}};
constexpr LineWeight kRoot[] = {{0, 1}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
    // The roll lane: only section rules play it, and its gate never stops the roll.
    {.lane = Lane::Snare, .densityGate = macroPoints(30)},
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
        .loop = {.degrees = kRoot, .draws = {.octaveUp = 0, .accent = 0}, .slide = 0, .gateTicks = 12 * kTicksPerStep},
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

	it("roll from beats to 8ths, 16ths and 32nds at the stage fractions, whatever the Density and the lane's gate", _ {
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

	it("change roll stages on the grid with a Gap, measuring the stage fractions from the Drop", _ {
		// A beat's Gap only takes the last beat: the stages change where they would with no Gap (spec §8.3).
		static constexpr Genre kGenre = Rules<16>::genre(GapLength::Beat, 0);
		Result result = playRules(kGenre, 8 * 7, kMacroFull);
		std::vector<int32_t> perBar(16);
		for (const Event& e : ons(result, kDrumsPart, Lane::Snare, kBuild8, kBuild8 + 16 * kStepsPerBar)) {
			perBar[(e.tick / kTicksPerStep - kBuild8) / kStepsPerBar]++;
		}
		const std::vector<int32_t> expected = {4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 16, 16, 32, 24};
		expect(perBar == expected).to_be_true();
	});

	it("let the roll replace only where it hits, and never be cut by a Replace pair", _ {
		// The roll replacing the hats takes out only the hats on its grid; the hats replacing the roll leave it whole.
		static constexpr ReplacePair kRollOverHats[] = {{Lane::Snare, Lane::ClosedHat}};
		static constexpr ReplacePair kHatsOverRoll[] = {{Lane::ClosedHat, Lane::Snare}};
		static constexpr Genre kRollReplaces = [] {
			Genre genre = Rules<16>::genre(GapLength::None, 0);
			genre.replacePairs = kRollOverHats;
			return genre;
		}();
		static constexpr Genre kRollReplaced = [] {
			Genre genre = Rules<16>::genre(GapLength::None, 0);
			genre.replacePairs = kHatsOverRoll;
			return genre;
		}();
		auto perBar = [](const Result& result, Lane lane) {
			std::vector<int32_t> out(16);
			for (const Event& e : ons(result, kDrumsPart, lane, kBuild8, kBuild8 + 16 * kStepsPerBar)) {
				out[(e.tick / kTicksPerStep - kBuild8) / kStepsPerBar]++;
			}
			return out;
		};
		const std::vector<int32_t> hats = {12, 12, 12, 12, 12, 12, 12, 12, 8, 8, 8, 8, 0, 0, 0, 0};
		expect(perBar(playRules(kRollReplaces, 8 * 7, kMacroFull), Lane::ClosedHat) == hats).to_be_true();
		const std::vector<int32_t> roll = {4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 16, 16, 32, 32};
		expect(perBar(playRules(kRollReplaced, 8 * 7, kMacroFull), Lane::Snare) == roll).to_be_true();
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

	it("start the roll before the Gap at Complexity 0, however short the Build", _ {
		// A quarter of a 1-bar Build is inside a beat's Gap, and of a 1-, 2- or 4-bar Build inside a bar's: the roll
		// starts by the last grid step before the Gap instead.
		auto rolls = [](const Genre& genre) {
			Result result = playRules(genre, 8 * 4, 0);
			const int32_t build = sectionTick(result, SectionKind::Build, 0) / kTicksPerStep;
			const int32_t drop = sectionTick(result, SectionKind::Groove, 1) / kTicksPerStep;
			return ons(result, kDrumsPart, Lane::Snare, build, drop).size();
		};
		static constexpr Genre kBarBeat = Rules<1>::genre(GapLength::Beat, 0);
		static constexpr Genre kTwoBarsBar = Rules<2>::genre(GapLength::Bar, 0);
		static constexpr Genre kFourBarsBar = Rules<4>::genre(GapLength::Bar, 0);
		expect(rolls(kBarBeat)).to_be_greater_than(size_t{0});
		expect(rolls(kTwoBarsBar)).to_be_greater_than(size_t{0});
		expect(rolls(kFourBarsBar)).to_be_greater_than(size_t{0});
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
		int32_t fallOff = -1;
		for (const Event& e : only(result.events, EventKind::NoteOff)) {
			if (e.lane == Lane::Fall && fallOff < 0) {
				fallOff = e.tick;
			}
		}
		expect(fallOff).to_equal(stepTick(kBuild8)); // it spans the Break (#423)
		auto impacts = ons(result, kFxPart, Lane::Impact, 0, end);
		expect(impacts.size()).to_equal(size_t{1});
		expect(impacts[0].tick).to_equal(stepTick(kDrop8));
	});

	it("hold the Fall from a Break's first step to the Break's end in every genre with a Fall lane (#423)", _ {
		// A Fall sample of 2 s or more loads in CUT mode (spec §10.4), so a short note would cut it.
		int32_t genresChecked = 0;
		for (const Genre* genre : allGenres()) {
			int32_t fxPart = -1;
			for (int32_t p = 0; p < partCount(*genre); p++) {
				if (genre->parts[p].role == Role::FX && laneIndexOf(genre->parts[p], Lane::Fall) >= 0) {
					fxPart = p;
				}
			}
			if (fxPart < 0) {
				continue;
			}
			Run run;
			run.recipe = newRecipe(*genre, 0);
			run.recipe.macros.set(Macro::Density, macroPoints(100)); // every Fall gate open
			Result result = play(run, 256);
			const int32_t breakTick = sectionStart(result, SectionKind::Break);
			int32_t breakEnd = -1;
			for (const Event& e : only(result.events, EventKind::Section)) {
				if (breakTick >= 0 && e.tick > breakTick) {
					breakEnd = e.tick;
					break;
				}
			}
			if (breakTick < 0 || breakEnd < 0) {
				std::printf("%s: no whole Break in 256 bars\n", genre->name);
			}
			expect(breakEnd > breakTick && breakTick >= 0).to_be_true();
			// Paired by lane: a Siren can start on the same tick at the same pitch.
			std::vector<Held> falls;
			for (const Event& e : result.events) {
				if (e.part != fxPart || e.lane != Lane::Fall) {
					continue;
				}
				if (e.kind == EventKind::NoteOn && e.tick >= breakTick && e.tick < breakEnd) {
					falls.push_back({e.tick, INT32_MAX, e.pitch});
				}
				else if (e.kind == EventKind::NoteOff && !falls.empty() && falls.back().off == INT32_MAX) {
					falls.back().off = e.tick;
				}
			}
			if (falls.size() != 1 || falls[0].on != breakTick || falls[0].off != breakEnd) {
				std::printf("%s: Break %d-%d, %zu Falls, first %d-%d\n", genre->name, breakTick, breakEnd, falls.size(),
				            falls.empty() ? -1 : falls[0].on, falls.empty() ? -1 : falls[0].off);
			}
			expect(falls.size()).to_equal(size_t{1});
			if (!falls.empty()) {
				expect(falls[0].on).to_equal(breakTick);
				expect(falls[0].off).to_equal(breakEnd);
			}
			genresChecked++;
		}
		expect(genresChecked >= 5).to_be_true(); // Techno's subgenres, House, Psytrance, Dub and Ambient
	});

	it("hold the Impact from a Drop's first step to the phrase's end or the next Section line in every genre (#472)", _ {
		// An Impact sample of 2 s or more loads in CUT mode (spec §10.4), and a preset's tail ends at its note-off, so
		// the gate's short note would cut it.
		int32_t genresChecked = 0;
		for (const Genre* genre : allGenres()) {
			int32_t fxPart = -1;
			for (int32_t p = 0; p < partCount(*genre); p++) {
				if (genre->parts[p].role == Role::FX && laneIndexOf(genre->parts[p], Lane::Impact) >= 0) {
					fxPart = p;
				}
			}
			if (fxPart < 0) {
				continue;
			}
			Run run;
			run.recipe = newRecipe(*genre, 0);
			run.recipe.macros.set(Macro::Density, macroPoints(100)); // every Impact gate open
			Result result = play(run, 256);
			const int32_t buildTick = sectionStart(result, SectionKind::Build);
			const int32_t dropTick = buildTick >= 0 ? sectionStart(result, SectionKind::Groove, buildTick + 1) : -1;
			int32_t nextLine = -1;
			for (const Event& e : only(result.events, EventKind::Section)) {
				if (dropTick >= 0 && e.tick > dropTick && nextLine < 0) {
					nextLine = e.tick;
				}
			}
			expect(dropTick >= 0).to_be_true();
			const int32_t phraseEnd = dropTick + genre->phraseSteps() * kTicksPerStep;
			const int32_t end = nextLine >= 0 && nextLine < phraseEnd ? nextLine : phraseEnd;
			std::vector<Held> impacts;
			for (const Event& e : result.events) {
				if (e.part != fxPart || e.lane != Lane::Impact) {
					continue;
				}
				if (e.kind == EventKind::NoteOn && e.tick == dropTick) {
					impacts.push_back({e.tick, INT32_MAX, e.pitch});
				}
				else if (e.kind == EventKind::NoteOff && !impacts.empty() && impacts.back().off == INT32_MAX) {
					impacts.back().off = e.tick;
				}
			}
			if (impacts.size() != 1 || impacts[0].off != end) {
				std::printf("%s: Drop %d, end %d, %zu Impacts, first off %d\n", genre->name, dropTick, end,
				            impacts.size(), impacts.empty() ? -1 : impacts[0].off);
			}
			expect(impacts.size()).to_equal(size_t{1});
			if (!impacts.empty()) {
				expect(impacts[0].off).to_equal(end);
			}
			genresChecked++;
		}
		expect(genresChecked >= 5).to_be_true(); // Techno's subgenres, House, Psytrance and Dub
	});

	it("never bring an Impact in late, even when its gate opens after the Drop's first step (#472)", _ {
		static constexpr LaneSpec kGatedImpact[] = {
		    {.lane = Lane::Riser},
		    {.lane = Lane::Fall},
		    {.lane = Lane::Impact, .densityGate = macroPoints(40)},
		};
		static constexpr PartSpec kGatedParts[] = {kParts[0], kParts[1], {.role = Role::FX, .lanes = kGatedImpact}};
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<8>::genre();
			genre.parts = kGatedParts;
			return genre;
		}();
		// Shut on the Drop's first step, open from its next bar: no Impact in the Drop's phrase.
		{
			Run run;
			run.recipe = newRecipe(kGenre, 0);
			run.steer = steerAt({{kDrop8 + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}}});
			Result result = play(run, 6 * 8);
			expect(ons(result, kFxPart, Lane::Impact, kDrop8, kDrop8 + kPhraseSteps).empty()).to_be_true();
		}
		// Open on the Drop's first step, shut on bar 1, open again on bar 2: cut on bar 1, never struck again.
		{
			Run run;
			run.recipe = newRecipe(kGenre, 0);
			run.recipe.macros.set(Macro::Density, kMacroFull);
			run.steer = steerAt({
			    {kDrop8 + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), 0}},
			    {kDrop8 + kStepsPerBar + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}},
			});
			Result result = play(run, 6 * 8);
			auto impacts = ons(result, kFxPart, Lane::Impact, kDrop8, kDrop8 + kPhraseSteps);
			expect(impacts.size()).to_equal(size_t{1});
			int32_t off = -1;
			for (const Event& e : only(result.events, EventKind::NoteOff)) {
				if (e.part == kFxPart && e.lane == Lane::Impact && e.tick >= stepTick(kDrop8)) {
					off = e.tick;
					break;
				}
			}
			expect(off).to_equal(stepTick(kDrop8 + kStepsPerBar));
		}
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

	it("never bring a gated-out FX note in late after a Section line", _ {
		// The Riser's gate is shut through a 16-bar Build, so its note is held back. A Groove forced onto step 512 cuts
		// the Build, and Density opens the gate on that line: the Riser stays out, as it belongs to the Build.
		static constexpr LaneSpec kGatedRiser[] = {{.lane = Lane::Riser, .densityGate = macroPoints(40)},
		                                           {.lane = Lane::Fall},
		                                           {.lane = Lane::Impact}};
		static constexpr PartSpec kGatedParts[] = {kParts[0], kParts[1], {.role = Role::FX, .lanes = kGatedRiser}};
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<16>::genre(GapLength::None, 0);
			genre.parts = kGatedParts;
			return genre;
		}();
		Run run;
		run.recipe = newRecipe(kGenre, 0);
		run.steer = steerAt({{kBuild8 + 16, {CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0}},
		                     {kDrop8 - 6, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}}});
		Result result = play(run, 6 * 8);
		expect(sectionTick(result, SectionKind::Groove, 1)).to_equal(stepTick(kDrop8));
		expect(ons(result, kFxPart, Lane::Riser, 0, 6 * kPhraseSteps).empty()).to_be_true();
		expect(ons(result, kDrumsPart, Lane::ClosedHat, kDrop8, kDrop8 + 1).size()).to_equal(size_t{1});
	});

	it("never bring a Fall in late during a Break, even when its gate opens mid-Break (#471)", _ {
		// Fall gated at 40 alongside a ClosedHat gated at 40 (proving the gate really opens on bar 1 of the Break).
		static constexpr LaneSpec kGatedDrums[] = {
		    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
		    {.lane = Lane::Snare, .densityGate = macroPoints(30)},
		    {.lane = Lane::ClosedHat, .pool = kEveryStep, .densityGate = macroPoints(40)},
		    {.lane = Lane::Crash},
		};
		static constexpr LaneSpec kGatedFall[] = {
		    {.lane = Lane::Riser},
		    {.lane = Lane::Fall, .densityGate = macroPoints(40)},
		    {.lane = Lane::Impact},
		};
		static constexpr PartSpec kGatedParts[] = {
		    {.role = Role::Drums, .name = "DRUMS", .lanes = kGatedDrums},
		    kParts[1],
		    {.role = Role::FX, .name = "FX", .lanes = kGatedFall},
		};
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<8>::genre();
			genre.parts = kGatedParts;
			return genre;
		}();

		// 1) Gate shut on Break's first step, then Density rises past 40 mid-Break: Fall stays silent the whole Break,
		// while the gated ClosedHat comes in on bar 1 of the Break.
		{
			Run run;
			run.recipe = newRecipe(kGenre, 0);
			run.steer = steerAt({{kBreak + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}}});
			Result result = play(run, 6 * 8);
			expect(ons(result, kDrumsPart, Lane::ClosedHat, kBreak, kBreak + kStepsPerBar).empty()).to_be_true();
			expect(ons(result, kDrumsPart, Lane::ClosedHat, kBreak + kStepsPerBar, kBuild8).empty()).to_be_false();
			expect(ons(result, kFxPart, Lane::Fall, kBreak, kBuild8).empty()).to_be_true();
		}

		// 2) Gate open on Break's first step: Fall plays across the whole Break as now.
		{
			Result result = playRules(kGenre, 6 * 8, macroPoints(25), kMacroFull);
			auto falls = ons(result, kFxPart, Lane::Fall, kBreak, kBuild8);
			expect(falls.size()).to_equal(size_t{1});
			if (!falls.empty()) {
				expect(falls[0].tick).to_equal(stepTick(kBreak));
			}
			int32_t fallOff = -1;
			for (const Event& e : only(result.events, EventKind::NoteOff)) {
				if (e.part == kFxPart && e.lane == Lane::Fall && e.tick >= stepTick(kBreak)) {
					fallOff = e.tick;
					break;
				}
			}
			expect(fallOff).to_equal(stepTick(kBuild8));
		}

		// 3) Gate open on Break's first step, closes on bar 1, then reopens on bar 2: Fall is cut on bar 1 and never
		// re-enters late on bar 2.
		{
			Run run;
			run.recipe = newRecipe(kGenre, 0);
			run.recipe.macros.set(Macro::Density, kMacroFull);
			run.steer = steerAt({
			    {kBreak + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), 0}},
			    {kBreak + kStepsPerBar + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}},
			});
			Result result = play(run, 6 * 8);
			auto falls = ons(result, kFxPart, Lane::Fall, kBreak, kBuild8);
			expect(falls.size()).to_equal(size_t{1});
			if (!falls.empty()) {
				expect(falls[0].tick).to_equal(stepTick(kBreak));
			}
			int32_t fallOff = -1;
			for (const Event& e : only(result.events, EventKind::NoteOff)) {
				if (e.part == kFxPart && e.lane == Lane::Fall && e.tick >= stepTick(kBreak)) {
					fallOff = e.tick;
					break;
				}
			}
			expect(fallOff).to_equal(stepTick(kBreak + kStepsPerBar));
		}

		// 4) Real genre: Ambient's Fall is gated at 35 (default Density 25, Break offset -15). Raising Density to 50
		// mid-Break opens the gate (effective Density 35) without bringing the Fall in late.
		{
			const Genre* amb = findGenre("ambient");
			expect(amb != nullptr).to_be_true();
			int32_t fxPart = -1;
			for (int32_t p = 0; p < partCount(*amb); p++) {
				if (amb->parts[p].role == Role::FX && laneIndexOf(amb->parts[p], Lane::Fall) >= 0) {
					fxPart = p;
				}
			}
			expect(fxPart >= 0).to_be_true();
			Run baseRun;
			baseRun.recipe = newRecipe(*amb, 0);
			Result base = play(baseRun, 256);
			const int32_t breakTick = sectionStart(base, SectionKind::Break);
			int32_t breakEnd = -1;
			for (const Event& e : only(base.events, EventKind::Section)) {
				if (breakTick >= 0 && e.tick > breakTick) {
					breakEnd = e.tick;
					break;
				}
			}
			expect(breakTick >= 0 && breakEnd > breakTick + kBarTicks).to_be_true();
			const int32_t breakStep = breakTick / kTicksPerStep;
			const int32_t breakEndStep = breakEnd / kTicksPerStep;
			Run steered;
			steered.recipe = newRecipe(*amb, 0);
			steered.steer =
			    steerAt({{breakStep + 4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), kMacroFull}}});
			Result result = play(steered, 256);
			expect(ons(result, static_cast<uint8_t>(fxPart), Lane::Fall, breakStep, breakEndStep).empty()).to_be_true();
		}
	});

	context("the Siren (spec §9.5)", _ {
		// A Siren of 2 bars gated at 30, with a delay setup: Density 35 opens its gate, 25 keeps it shut. The FX part
		// switches every Throw moment off: the Siren's release Throw has no switch.
		static constexpr DelaySetup kEcho{.on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted};
		static constexpr LaneSpec kSirenFx[] = {
		    {.lane = Lane::Riser},
		    {.lane = Lane::Fall},
		    {.lane = Lane::Impact},
		    {.lane = Lane::Siren, .densityGate = macroPoints(30), .delay = kEcho, .heldBars = 2},
		};
		static constexpr uint16_t kNoThrows = momentBit(GestureMoment::ThrowBreak) | momentBit(GestureMoment::ThrowGap)
		                                      | momentBit(GestureMoment::ThrowTransition);
		static constexpr PartSpec kSirenParts[] = {
		    kParts[0], kParts[1], {.role = Role::FX, .lanes = kSirenFx, .gestures = {.off = kNoThrows}}};
		static constexpr Genre kGenre = [] {
			Genre genre = Rules<8>::genre();
			genre.parts = kSirenParts;
			return genre;
		}();
		constexpr int32_t kSirenSteps = 2 * kStepsPerBar;
		const auto noteOff = [](const Result& result, int32_t from) -> int32_t {
			for (const Event& e : only(result.events, EventKind::NoteOff)) {
				if (e.lane == Lane::Siren && e.tick > stepTick(from)) {
					return e.tick;
				}
			}
			return -1;
		};
		/// The Siren row's Throw spikes: Space snapping with the delay lifted in full (a Reset snaps it too, home).
		const auto spikes = [](const Result& result) {
			std::vector<Event> out;
			for (const Event& e : controls(result)) {
				if (e.part == kFxPart && e.lane == Lane::Siren && e.control == SoundControl::Space && e.snap
				    && e.throwDelay() == 100) {
					out.push_back(e);
				}
			}
			return out;
		};

		it("holds one note for its bars from a Break's first step and from a Drop, while its gate is open", _ {
			Result result = playRules(kGenre, 6 * 8, macroPoints(25), macroPoints(35));
			auto sirens = ons(result, kFxPart, Lane::Siren, 0, kDrop8 + kPhraseSteps);
			expect(sirens.size()).to_equal(size_t{2});
			expect(sirens[0].tick).to_equal(stepTick(kBreak));
			expect(sirens[1].tick).to_equal(stepTick(kDrop8));
			expect(noteOff(result, kBreak)).to_equal(stepTick(kBreak + kSirenSteps));
			expect(noteOff(result, kDrop8)).to_equal(stepTick(kDrop8 + kSirenSteps));
		});

		it("plays nothing while Density keeps its gate shut", _ {
			Result result = playRules(kGenre, 6 * 8);
			expect(ons(result, kFxPart, Lane::Siren, 0, 6 * kPhraseSteps).empty()).to_be_true();
			expect(spikes(result).empty()).to_be_true();
		});

		it("spikes its row's Space and delay where its note ends, with every Throw switch off", _ {
			Result result = playRules(kGenre, 6 * 8, macroPoints(25), macroPoints(35));
			const std::vector<Event> spiked = spikes(result);
			expect(spiked.size()).to_be_greater_than(size_t{1});
			expect(spiked[0].tick).to_equal(stepTick(kBreak + kSirenSteps));
			expect(spiked[0].throwDelay()).to_equal(100);
			// It holds a bar while the Siren is silent, then falls over a beat.
			const int32_t held = stepTick(kBreak + kSirenSteps + kStepsPerBar);
			expect(lastAt(result, kFxPart, Lane::Siren, SoundControl::Space, held)->throwDelay()).to_equal(100);
			const int32_t fallen = stepTick(kBreak + kSirenSteps + kStepsPerBar + kStepsPerBeat + 1);
			expect(lastAt(result, kFxPart, Lane::Siren, SoundControl::Space, fallen)->throwDelay()).to_equal(0);
			bool afterDrop = false;
			for (const Event& e : spiked) {
				afterDrop = afterDrop || e.tick == stepTick(kDrop8 + kSirenSteps);
			}
			expect(afterDrop).to_be_true();
		});

		it("spikes where a Section line cuts it short", _ {
			// A Siren of a phrase from the Drop, cut by the next 2-bar Build, which takes the end of the Drop's
			// phrase: Groove → Build always.
			static constexpr LaneSpec kLongSiren[] = {{.lane = Lane::Siren, .delay = kEcho, .heldBars = 8}};
			static constexpr PartSpec kLongParts[] = {kParts[0], kParts[1], {.role = Role::FX, .lanes = kLongSiren}};
			static constexpr WeightedBars kTwoBars[] = {{2, 1}};
			static constexpr Genre kCut = [] {
				Genre genre = Rules<8>::genre(GapLength::None, 0);
				genre.parts = kLongParts;
				genre.arc.buildBars = kTwoBars;
				genre.arc.grooveToBuild = 1000;
				return genre;
			}();
			Result result = playRules(kCut, 6 * 8);
			const int32_t drop = sectionStart(result, SectionKind::Groove, sectionStart(result, SectionKind::Build));
			const int32_t build = sectionStart(result, SectionKind::Build, drop);
			expect(drop).to_be_greater_than(0);
			expect(build).to_equal(drop + stepTick(kPhraseSteps - 2 * kStepsPerBar));
			expect(ons(result, kFxPart, Lane::Siren, 0, 6 * kPhraseSteps).size()).to_be_greater_than(size_t{0});
			const std::vector<Event> spiked = spikes(result);
			bool atBuild = false;
			for (const Event& e : spiked) {
				atBuild = atBuild || e.tick == build;
			}
			expect(atBuild).to_be_true();
		});
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
