// Fills in the event stream (spec §8.7): automatic fills and held FILL, Rests and Swaps, their precedence under the
// Gap and the Build roll, the Phrase accent after a fill (§9.5) and the Answer before an automatic fill (§5.2).

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <utility>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kOffBeats[] = {{"..X...X...X...X.", "................", 0}};
constexpr Pattern kHalfNotes[] = {{"X.......X.......", "................", 0}};
// Fill patterns for the last beat: the snare's off-16ths thin out with Density.
constexpr Pattern kSnareFill[] = {{"............X5X5", "................", 0}};
constexpr Pattern kTomFill[] = {{"............X.X.", "................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};
constexpr Answer kFifth[] = {{AnswerKind::Fifth}};
/// The hats keep off the kick: their plan reads the kick's, never a fill.
constexpr InterplayRule kHatsOffKick[] = {{.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .offBeat = true}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor, .gateTicks = 200}, // a long note, to show a Rest leaves it alone
    {.lane = Lane::Snare, .fillPool = kSnareFill},                   // the roll lane, and a Swap's
    {.lane = Lane::ClosedHat, .pool = kOffBeats, .rules = kHatsOffKick},
    {.lane = Lane::Crash}, // the accent lane
    {.lane = Lane::Tom, .fillPool = kTomFill},
};
constexpr LaneSpec kFx[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};
/// A Bass of half notes 12 steps long, so a note is sounding when a fill starts. Answers only before a fill: N is
/// longer than any piece here.
constexpr LoopSpec kBassLoop{.degrees = kRoot,
                             .octaveUp = 0,
                             .accent = 0,
                             .slide = 0,
                             .answers = kFifth,
                             .answerBars = 200,
                             .gateTicks = 12 * kTicksPerStep};
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums},
    {.role = Role::Bass, .flavour = Flavour::Loop, .name = "BASS", .pool = kHalfNotes, .loop = kBassLoop},
    {.role = Role::FX, .name = "FX", .lanes = kFx},
};
constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr Scale kScales[] = {Scale::Minor};

constexpr FillEntry kKickDrop{
    .kind = FillKind::Rest, .mask = {.parts = 1U << kBassPart, .lanes = laneBit(Lane::Kick)}, .beats = 2};
constexpr FillEntry kSnareRun{
    .kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 1};
constexpr FillEntry kRestOnly[] = {kKickDrop};
constexpr FillEntry kSwapOnly[] = {kSnareRun};
constexpr FillEntry kBoth[] = {kKickDrop, kSnareRun};

constexpr WeightedBars kBuild8[] = {{8, 1}};

/// Intro a phrase, then a Groove of `groove` phrases at Motion 25 (b ÷ 3 to 3 × b with Motion), a Break and an 8-bar
/// Build with a beat's Gap. Flat macros, so only fills and section rules change what plays.
constexpr Genre fillGenre(std::span<const FillEntry> fills, int16_t chance, uint8_t groove = 2,
                          int16_t sectionEnd = kSameAsFillChance) {
	return {
	    .name = "fills",
	    .facts = {.scales = kScales},
	    .parts = kParts,
	    .fills = fills,
	    .fillChance = chance,
	    .sectionEndChance = sectionEnd,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {groove, groove},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuild8,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	            .breakKeeps = kEverything,
	            .buildCut = {},
	            .gap = GapLength::Beat,
	            .roll = {.lane = Lane::Snare, .top = RollStage::Sixteenth},
	            .accentLane = Lane::Crash,
	        },
	};
}

Run fillRun(const Genre& genre, int32_t motion = macroPoints(25), uint32_t seed = 1) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, kMacroFull);
	run.recipe.macros.set(Macro::Motion, motion);
	run.seed = seed;
	return run;
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

/// The steps in [from, to) where `part`'s `lane` starts a note.
std::vector<int32_t> hitSteps(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<int32_t> out;
	for (const Event& e : ons(result, part, lane, from, to)) {
		out.push_back(e.tick / kTicksPerStep);
	}
	return out;
}

/// Whether the phrase starting on step `phrase` ends in a kick drop: no kick in its last 2 beats, where it has 2.
bool kickDropped(const Result& result, int32_t phrase) {
	const int32_t end = phrase + kPhraseSteps;
	return ons(result, kDrumsPart, Lane::Kick, end - 2 * kStepsPerBeat, end).empty();
}

/// Steering pushed when the driver is about to decide each step.
std::function<void(int32_t, SteeringQueue&)> fillAt(std::vector<std::pair<int32_t, int32_t>> presses) {
	std::vector<std::pair<int32_t, Command>> commands;
	for (auto [step, value] : presses) {
		commands.push_back({step, {CommandKind::SetFill, 0, value}});
	}
	return steerAt(commands);
}

} // namespace

// clang-format off
describe fills("Fills", $ {
	it("play an automatic fill at the end of each Groove phrase and the Intro's last, ending on the phrase line", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 1000);
		Result result = play(fillRun(kGenre), 3 * 8 + 1);
		for (int32_t phrase : {0, kPhraseSteps, 2 * kPhraseSteps}) {
			const int32_t end = phrase + kPhraseSteps;
			// Kicks on every beat up to the fill's 2 beats, none in them, and the phrase line's kick plays.
			expect(hitSteps(result, kDrumsPart, Lane::Kick, end - 4 * kStepsPerBeat, end)
			       == std::vector<int32_t>{end - 16, end - 12}).to_be_true();
			expect(ons(result, kBassPart, Lane::None, end - 2 * kStepsPerBeat, end).empty()).to_be_true();
			expect(ons(result, kBassPart, Lane::None, end - 3 * kStepsPerBeat, end - 2 * kStepsPerBeat).size())
			    .to_equal(size_t{0}); // the Bass's half notes fall on the bar's steps 0 and 8
			expect(ons(result, kBassPart, Lane::None, end - kStepsPerBar, end - kStepsPerBar + 1).size())
			    .to_equal(size_t{1});
			// Lanes outside the mask play on.
			expect(ons(result, kDrumsPart, Lane::ClosedHat, end - 2 * kStepsPerBeat, end).size()).to_equal(size_t{2});
		}
		expect(ons(result, kDrumsPart, Lane::Kick, 3 * kPhraseSteps, 3 * kPhraseSteps + 1).size()).to_equal(size_t{1});
	});

	it("play none in Breaks and Builds, whatever the chance", _ {
		// Intro, two Groove phrases, then the Break at 384 and the Build at 512.
		static constexpr Genre kGenre = fillGenre(kRestOnly, 1000);
		Result result = play(fillRun(kGenre), 5 * 8);
		expect(kickDropped(result, 2 * kPhraseSteps)).to_be_true(); // the Groove's last phrase
		expect(kickDropped(result, 3 * kPhraseSteps)).to_be_false(); // the Break
		expect(ons(result, kDrumsPart, Lane::Kick, 5 * kPhraseSteps - 16, 5 * kPhraseSteps - 8).size())
		    .to_equal(size_t{2}); // the Build, before its Gap
	});

	it("come more often with more Motion, and never at Motion 0", _ {
		// Techno's 35% at Motion 25: about 3% just above 0 and 88% at 50 (spec §3.5). A long Groove, so most phrases
		// can fill; Motion shortens it, so count over the Groove's own phrases.
		static constexpr Genre kGenre = fillGenre(kRestOnly, 350, 120);
		auto share = [&](int32_t motion) {
			Result result = play(fillRun(kGenre, motion), 41 * 8);
			int32_t phrases = 0;
			int32_t filled = 0;
			for (int32_t phrase = kPhraseSteps; phrase < 40 * kPhraseSteps; phrase += kPhraseSteps) {
				phrases++;
				filled += kickDropped(result, phrase) ? 1 : 0;
			}
			return filled * 100 / phrases;
		};
		expect(share(0)).to_equal(0);
		const int32_t low = share(macroPoints(1));
		const int32_t mid = share(macroPoints(25));
		const int32_t high = share(kMacroFull);
		expect(low).to_be_less_than(15);
		expect(mid).to_be_greater_than(19);
		expect(mid).to_be_less_than(55);
		expect(high).to_be_greater_than(70);
	});

	it("use the section-end chance for a Groove's last phrase before a Break, and a force queued before it counts", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 0, 3, 1000);
		Result natural = play(fillRun(kGenre), 6 * 8);
		// Groove phrases at 128, 256 and 384: only the last fills.
		expect(kickDropped(natural, kPhraseSteps)).to_be_false();
		expect(kickDropped(natural, 2 * kPhraseSteps)).to_be_false();
		expect(kickDropped(natural, 3 * kPhraseSteps)).to_be_true();
		// A Break forced during the first Groove phrase makes it the last, landing on its end. It was queued after the
		// phrase's first step, where the draw was made, so it doesn't count.
		Run late = fillRun(kGenre);
		late.steer = at(kPhraseSteps + 72, {CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		Result forced = play(late, 4 * 8);
		expect(kickDropped(forced, kPhraseSteps)).to_be_false();
	});

	it("end the notes sounding on a Rest's pitched parts as it starts, and let drum one-shots play out", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 1000);
		Result result = play(fillRun(kGenre), 2 * 8);
		// The fill takes steps 120–127. The Bass note from step 112 would last to 124: it ends at 120. The kick from
		// step 116 keeps its 200 ticks.
		const int32_t rest = 2 * kPhraseSteps - 2 * kStepsPerBeat;
		bool bassCut = false;
		bool kickRings = false;
		for (const Event& e : only(result.events, EventKind::NoteOff)) {
			bassCut = bassCut || (e.part == kBassPart && e.tick == stepTick(rest));
			kickRings = kickRings || (e.part == kDrumsPart && e.lane == Lane::Kick && e.tick == stepTick(rest - 4) + 200);
		}
		expect(bassCut).to_be_true();
		expect(kickRings).to_be_true();
	});

	it("start a held fill on the next undecided step, keep it while held, and bring the parts back on release", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 0);
		Run run = fillRun(kGenre);
		run.steer = fillAt({{kPhraseSteps + 40, kFillHeldEntry}, {kPhraseSteps + 60, kFillReleased}});
		Result result = play(run, 2 * 8);
		expect(hitSteps(result, kDrumsPart, Lane::Kick, kPhraseSteps + 32, kPhraseSteps + 68)
		       == std::vector<int32_t>{kPhraseSteps + 32, kPhraseSteps + 36, kPhraseSteps + 60, kPhraseSteps + 64})
		    .to_be_true();
		expect(ons(result, kBassPart, Lane::None, kPhraseSteps + 40, kPhraseSteps + 60).empty()).to_be_true();
		expect(ons(result, kBassPart, Lane::None, kPhraseSteps + 64, kPhraseSteps + 65).size()).to_equal(size_t{1});
	});

	it("pause a held fill on a phrase's first step, starting its Rest again on the next", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 0);
		Run run = fillRun(kGenre);
		const int32_t line = 2 * kPhraseSteps;
		run.steer = fillAt({{line - 8, kFillHeldEntry}, {line + 12, kFillReleased}});
		Result result = play(run, 3 * 8);
		expect(hitSteps(result, kDrumsPart, Lane::Kick, line - 8, line + 16)
		       == std::vector<int32_t>{line, line + 12}).to_be_true();
		// The Bass note on the phrase line plays, and the Rest cuts it a step later.
		expect(hitSteps(result, kBassPart, Lane::None, line - 8, line + 12) == std::vector<int32_t>{line}).to_be_true();
		int32_t cuts = 0;
		for (const Event& e : only(result.events, EventKind::NoteOff)) {
			cuts += e.part == kBassPart && e.tick == stepTick(line + 1) ? 1 : 0;
		}
		expect(cuts).to_equal(1);
	});

	it("let a held fill override the automatic one", _ {
		static constexpr Genre kGenre = [] {
			Genre genre = fillGenre(kBoth, 1000);
			genre.heldFill = 0; // the kick drop
			return genre;
		}();
		Result automatic = play(fillRun(kGenre), 2 * 8);
		Run run = fillRun(kGenre);
		run.steer = fillAt({{kPhraseSteps + 100, kFillHeldEntry}});
		Result held = play(run, 2 * 8);
		// Held from step 228 on: the kick drop, and no snare run.
		const int32_t end = 2 * kPhraseSteps;
		const bool snareRun = !ons(automatic, kDrumsPart, Lane::Snare, end - 4, end).empty();
		expect(snareRun || kickDropped(automatic, kPhraseSteps)).to_be_true(); // an automatic fill plays there
		expect(ons(held, kDrumsPart, Lane::Snare, kPhraseSteps, end).empty()).to_be_true();
		expect(ons(held, kDrumsPart, Lane::Kick, kPhraseSteps + 100, end).empty()).to_be_true();
	});

	it("play the held-fill entry on FILL, and with SHIFT a weighted pick keyed on the step it lands on", _ {
		static constexpr Genre kGenre = [] {
			Genre genre = fillGenre(kBoth, 0);
			genre.heldFill = 1; // the snare run
			return genre;
		}();
		auto heldAt = [&](int32_t step, int32_t value) {
			Run run = fillRun(kGenre);
			run.steer = fillAt({{step, value}});
			return play(run, 2).generator->heldFill();
		};
		expect(heldAt(5, kFillHeldEntry)).to_equal(1);
		expect(heldAt(9, kFillHeldEntry)).to_equal(1);
		// A fresh pick replays where it lands, and differs between steps.
		std::vector<int32_t> picks;
		for (int32_t step = 1; step < 30; step++) {
			const int32_t pick = heldAt(step, kFillFreshPick);
			expect(heldAt(step, kFillFreshPick)).to_equal(pick);
			picks.push_back(pick);
		}
		expect(std::count(picks.begin(), picks.end(), 0) > 0 && std::count(picks.begin(), picks.end(), 1) > 0)
		    .to_be_true();
		// A genre with no fill entries: FILL does nothing.
		static constexpr Genre kNoFills = fillGenre({}, 0);
		Run none = fillRun(kNoFills);
		none.steer = fillAt({{5, kFillHeldEntry}});
		Result result = play(none, 2);
		expect(result.generator->heldFill()).to_equal(-1);
	});

	it("play a Swap's fill pattern on its lanes instead of their own, thinned by Density", _ {
		static constexpr Genre kGenre = fillGenre(kSwapOnly, 1000);
		Run run = fillRun(kGenre);
		Result full = play(run, 2 * 8);
		const int32_t end = 2 * kPhraseSteps;
		// One beat: the snare's fill pattern plays all four 16ths at full Density, and the tom its two.
		expect(hitSteps(full, kDrumsPart, Lane::Snare, end - kStepsPerBar, end)
		       == std::vector<int32_t>{end - 4, end - 3, end - 2, end - 1}).to_be_true();
		expect(hitSteps(full, kDrumsPart, Lane::Snare, kPhraseSteps, end - 4).empty()).to_be_true();
		expect(hitSteps(full, kDrumsPart, Lane::Tom, end - kStepsPerBar, end)
		       == std::vector<int32_t>{end - 4, end - 2}).to_be_true();
		expect(ons(full, kDrumsPart, Lane::Kick, end - 4, end).size()).to_equal(size_t{1});
		// At Density 0 only the fill pattern's anchors play, as for any pattern.
		run.recipe.macros.set(Macro::Density, 0);
		Result thin = play(run, 2 * 8);
		expect(hitSteps(thin, kDrumsPart, Lane::Snare, end - kStepsPerBar, end)
		       == std::vector<int32_t>{end - 4, end - 2}).to_be_true();
	});

	it("leave interplay rules reading the normal plan, never the fill", _ {
		// The hats keep off the kick's beats; a kick drop doesn't move them onto the beats.
		static constexpr Genre kGenre = fillGenre(kRestOnly, 1000);
		Result result = play(fillRun(kGenre), 2 * 8);
		const int32_t end = 2 * kPhraseSteps;
		expect(hitSteps(result, kDrumsPart, Lane::ClosedHat, end - 8, end) == std::vector<int32_t>{end - 6, end - 2})
		    .to_be_true();
	});

	it("give way to the Gap and keep the Build roll rolling under a held fill", _ {
		// Held through the Build: the snare run never replaces the roll, the tom plays its fill, and nothing plays in
		// the Gap. The Build is 512–639, its Gap the last beat.
		static constexpr Genre kGenre = fillGenre(kSwapOnly, 0);
		Result plain = play(fillRun(kGenre, macroPoints(25)), 5 * 8);
		Run run = fillRun(kGenre);
		run.steer = fillAt({{4 * kPhraseSteps + 1, kFillHeldEntry}});
		Result held = play(run, 5 * 8);
		const int32_t build = 4 * kPhraseSteps;
		const int32_t drop = 5 * kPhraseSteps;
		auto plainRoll = ons(plain, kDrumsPart, Lane::Snare, build, drop);
		expect(plainRoll.empty()).to_be_false();
		const int32_t rollStart = plainRoll.front().tick / kTicksPerStep;
		expect(rollStart).to_be_greater_than(build); // the roll starts late at the default Complexity
		expect(ons(held, kDrumsPart, Lane::Snare, rollStart, drop) == plainRoll).to_be_true();
		// Before the roll starts, the snare plays the run.
		expect(ons(held, kDrumsPart, Lane::Snare, build, rollStart).empty()).to_be_false();
		expect(ons(held, kDrumsPart, Lane::Tom, build, drop - 4).empty()).to_be_false();
		expect(ons(held, kDrumsPart, Lane::None, drop - 4, drop).empty()).to_be_true();
	});

	it("put a Phrase accent on the phrase line after a fill that covered its last step", _ {
		static constexpr Genre kGenre = fillGenre(kRestOnly, 1000);
		Result result = play(fillRun(kGenre), 3 * 8 + 1);
		for (int32_t line : {kPhraseSteps, 2 * kPhraseSteps, 3 * kPhraseSteps}) {
			expect(ons(result, kDrumsPart, Lane::Crash, line, line + 1).size()).to_equal(size_t{1});
		}
		static constexpr Genre kNone = fillGenre(kRestOnly, 0);
		Result plain = play(fillRun(kNone), 3 * 8 + 1);
		expect(ons(plain, kDrumsPart, Lane::Crash, kPhraseSteps + 1, 3 * kPhraseSteps + 1).empty()).to_be_true();
		// A held fill let go before the phrase's last step gives none.
		Run run = fillRun(kNone);
		run.steer = fillAt({{kPhraseSteps + 100, kFillHeldEntry}, {kPhraseSteps + 120, kFillReleased},
		                    {2 * kPhraseSteps + 100, kFillHeldEntry}, {3 * kPhraseSteps + 2, kFillReleased}});
		Result held = play(run, 3 * 8 + 1);
		expect(ons(held, kDrumsPart, Lane::Crash, 2 * kPhraseSteps, 2 * kPhraseSteps + 1).empty()).to_be_true();
		expect(ons(held, kDrumsPart, Lane::Crash, 3 * kPhraseSteps, 3 * kPhraseSteps + 1).size()).to_equal(size_t{1});
	});

	it("make Loop parts answer in the bar before an automatic fill, and not before a held one", _ {
		// The snare run takes the last beat of bar 8, so bar 7 answers: a fifth from the call.
		static constexpr Genre kGenre = fillGenre(kSwapOnly, 1000);
		Result result = play(fillRun(kGenre), 2 * 8);
		auto pitchesIn = [](const Result& r, int32_t from) {
			std::vector<uint8_t> out;
			for (const Event& e : ons(r, kBassPart, Lane::None, from, from + kStepsPerBar)) {
				out.push_back(e.pitch);
			}
			return out;
		};
		const int32_t bar7 = 2 * kPhraseSteps - 2 * kStepsPerBar;
		const auto call = pitchesIn(result, bar7 - kStepsPerBar);
		const auto answer = pitchesIn(result, bar7);
		expect(call.size()).to_equal(size_t{2});
		expect(answer.size()).to_equal(size_t{2});
		expect(answer[0] != call[0] && answer[1] != call[1]).to_be_true();
		expect(pitchesIn(result, bar7 + kStepsPerBar) == call).to_be_true();
		// No automatic fills: a held one gets no Answer before it.
		static constexpr Genre kNone = fillGenre(kSwapOnly, 0);
		Run run = fillRun(kNone);
		run.steer = fillAt({{2 * kPhraseSteps - 20, kFillHeldEntry}});
		Result held = play(run, 2 * 8);
		expect(pitchesIn(held, bar7) == pitchesIn(held, bar7 - kStepsPerBar)).to_be_true();
	});
});
// clang-format on

CPPSPEC_SPEC(fills)
