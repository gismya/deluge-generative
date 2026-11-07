// Density gates and late entry (spec §5.6).

#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr Pattern kFour[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kBusy[] = {{"9999999999999999", "................", 0}};
constexpr Pattern kOnTheAnds[] = {{"..X...X...X...X.", "................", 0}};
/// One note at the start of every 2 bars.
constexpr Pattern kEveryTwoBars[] = {{"X...............................", "................................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// One-bar phrases and no Arc offsets, so the Recipe's macros are what plays. runOf() starts in Groove at Motion 0,
/// so the Groove never ends and bar 0 is the only Section line.
constexpr Genre streamGenre(std::span<const PartSpec> parts) {
	return {
	    .name = "gates",
	    .facts = {.scales = kScales},
	    .phraseBars = 1,
	    .parts = parts,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {4, 4},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuildBars,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	        },
	};
}

/// A kick on the beats and a Perc lane on every step, gated at Density 30.
constexpr LaneSpec kGatedPerc[] = {
    {.lane = Lane::Kick, .pool = kFour},
    {.lane = Lane::Perc1, .pool = kEveryStep, .densityGate = macroPoints(30)},
};
constexpr PartSpec kGatedPercParts[] = {{.role = Role::Drums, .lanes = kGatedPerc}};
constexpr Genre kGatedPercGenre = streamGenre(kGatedPercParts);

/// A Bass of long notes: one every 2 bars, lasting `Steps`, its part gated at `Gate` points.
template <int32_t Steps, int32_t Gate = 0>
struct LongBass {
	static constexpr PartSpec parts[] = {{
	    .role = Role::Bass,
	    .flavour = Flavour::Loop,
	    .pool = kEveryTwoBars,
	    .densityGate = macroPoints(Gate),
	    .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 0, .gateTicks = Steps * kTicksPerStep},
	}};
	static constexpr Genre genre = streamGenre(parts);
};

/// Steers `macro` to each value at its step.
struct Steer {
	Macro macro;
	std::vector<std::pair<int32_t, int32_t>> points; ///< (step, macro points)
	void operator()(int32_t nextStep, SteeringQueue& queue) const {
		for (auto [step, value] : points) {
			if (step == nextStep) {
				queue.push({CommandKind::SetMacro, static_cast<uint8_t>(macro), macroPoints(value)});
			}
		}
	}
};

Run runOf(const Genre& genre, int32_t density = 25) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Motion, 0);
	run.skipIntro = true; // the Recipe's Density plays from the first bar
	return run;
}

/// Bars where `lane` hit at least once.
std::set<int32_t> barsWith(const Result& result, Lane lane) {
	std::set<int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == lane) {
			out.insert(e.tick / kBarTicks);
		}
	}
	return out;
}

std::vector<Event> pitched(const Result& result, EventKind kind) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, kind)) {
		if (e.lane == Lane::None) {
			out.push_back(e);
		}
	}
	return out;
}

std::set<int32_t> ticksOf(const std::vector<Event>& events) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		out.insert(e.tick);
	}
	return out;
}

} // namespace

// clang-format off
describe gates("Density gates", $ {
	it("open at a bar line once Density reaches the gate, and close only when it falls 2 below", _ {
		Run run = runOf(kGatedPercGenre, 25);
		// Up to 30 mid bar 1, so the lane comes in on bar 2. Down to 29 in bar 3: it stays. 28 in bar 4: it leaves on
		// bar 5. Up to 29 in bar 6: still out.
		run.steer = Steer{Macro::Density, {{20, 30}, {52, 29}, {70, 28}, {100, 29}}};
		Result result = play(run, 8);
		expect(barsWith(result, Lane::Perc1) == std::set<int32_t>{2, 3, 4}).to_be_true();
		auto perc = std::vector<Event>{};
		for (const Event& e : only(result.events, EventKind::NoteOn)) {
			if (e.lane == Lane::Perc1) {
				perc.push_back(e);
			}
		}
		expect(perc.size()).to_equal(size_t{3 * kStepsPerBar}); // whole bars only
		expect(barsWith(result, Lane::Kick).size()).to_equal(size_t{8});
	});

	it("gate a whole part too", _ {
		Result closed = play(runOf(LongBass<4, 30>::genre, 29), 4);
		expect(pitched(closed, EventKind::NoteOn).empty()).to_be_true();
		Result open = play(runOf(LongBass<4, 30>::genre, 30), 4);
		expect(pitched(open, EventKind::NoteOn).size()).to_equal(size_t{2});
	});

	it("leave what rules read alone: a gated-out kick still shapes the hats", _ {
		static constexpr InterplayRule kHatRules[] = {{.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .offBeat = true}};
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::Kick, .pool = kFour, .densityGate = kMacroFull},
		    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kHatRules},
		};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kLanes}};
		static constexpr Genre kGenre = streamGenre(kParts);
		Result result = play(runOf(kGenre, 45), 1); // the hat's 9s all play, the kick's gate stays shut
		expect(barsWith(result, Lane::Kick).empty()).to_be_true();
		std::set<int32_t> hats;
		for (const Event& e : only(result.events, EventKind::NoteOn)) {
			hats.insert(e.tick / kTicksPerStep);
		}
		expect(hats == std::set<int32_t>{2, 6, 10, 14}).to_be_true();
	});

	it("keep a gated-out lane from replacing another", _ {
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::ClosedHat, .pool = kEveryStep},
		    {.lane = Lane::OpenHat, .pool = kOnTheAnds, .densityGate = macroPoints(40)},
		};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kLanes}};
		static constexpr Genre kGenre = streamGenre(kParts);
		Result result = play(runOf(kGenre, 25), 1);
		expect(only(result.events, EventKind::NoteOn).size()).to_equal(size_t{16});
	});

	it("end a closing part's sounding notes on the bar line", _ {
		Run run = runOf(LongBass<24, 30>::genre, 30);
		run.steer = Steer{Macro::Density, {{4, 20}}};
		Result result = play(run, 4);
		auto ons = pitched(result, EventKind::NoteOn);
		auto offs = pitched(result, EventKind::NoteOff);
		expect(ons.size()).to_equal(size_t{1});
		expect(offs.size()).to_equal(size_t{1});
		expect(offs[0].tick).to_equal(kBarTicks);
	});

	it("bring in a held note on the bar line it opens, if it lasts to the end of that bar", _ {
		// A 2-bar note from bar 0 still has a bar left on bar 1's line: it starts there for the rest of its length,
		// up to the next note at bar 2.
		Run run = runOf(LongBass<32, 30>::genre, 20);
		run.steer = Steer{Macro::Density, {{4, 30}}};
		Result result = play(run, 3);
		std::set<int32_t> ons;
		std::set<int32_t> offs;
		for (const Event& e : pitched(result, EventKind::NoteOn)) {
			if (e.tick <= 2 * kBarTicks) {
				ons.insert(e.tick);
			}
		}
		for (const Event& e : pitched(result, EventKind::NoteOff)) {
			if (e.tick <= 2 * kBarTicks) {
				offs.insert(e.tick);
			}
		}
		expect(ons == std::set<int32_t>{kBarTicks, 2 * kBarTicks}).to_be_true();
		expect(offs == std::set<int32_t>{2 * kBarTicks}).to_be_true();

		// A 20-step note has only 4 steps left: it's skipped.
		Run shorter = runOf(LongBass<20, 30>::genre, 20);
		shorter.steer = Steer{Macro::Density, {{4, 30}}};
		for (const Event& e : pitched(play(shorter, 3), EventKind::NoteOn)) {
			expect(e.tick).to_equal(2 * kBarTicks);
		}
	});

	it("let a part's own note on the bar line take the place of a late entry on its key", _ {
		// The gate opens on bar 2 (not a Section line), where the Bass plays its own note 5 ticks early on the same
		// pitch as the held-back note from bar 1, which would otherwise enter late.
		static constexpr Pattern kOnTheOne[] = {{"X...............", "................", 0}};
		static constexpr PartSpec kParts[] = {{
		    .role = Role::Bass,
		    .flavour = Flavour::Loop,
		    .pool = kOnTheOne,
		    .densityGate = macroPoints(30),
		    .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 0, .gateTicks = 40 * kTicksPerStep},
		    .expression = {.lean = {-5, 0, 0, 0}},
		}};
		static constexpr Genre kGenre = streamGenre(kParts);
		Run run = runOf(kGenre, 20);
		run.steer = Steer{Macro::Density, {{20, 30}}};
		run.ticksPerCall = 1;
		Result result = play(run, 3);
		std::vector<int32_t> ons;
		for (const Event& e : pitched(result, EventKind::NoteOn)) {
			if (e.tick < 2 * kBarTicks + kBarTicks / 2) {
				ons.push_back(e.tick);
			}
		}
		expect(ons == std::vector<int32_t>{2 * kBarTicks - 5}).to_be_true();
		std::map<uint8_t, int32_t> sounding;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn) {
				expect(sounding.count(e.pitch)).to_equal(size_t{0});
				sounding[e.pitch] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff && e.lane == Lane::None) {
				expect(sounding.count(e.pitch) == 1 && e.tick > sounding[e.pitch]).to_be_true();
				sounding.erase(e.pitch);
			}
		}
	});

	it("hold back what a closing gate cut, and bring it in again when the gate reopens", _ {
		// A 4-bar note from step 0 (importance 1: it plays at Density 50, and not again at 30). The gate closes on
		// bar 1 and reopens on bar 2, where the note still has 2 bars left.
		static constexpr Pattern kQuietOne[] = {{"1...............................", "................................", 0}};
		static constexpr PartSpec kParts[] = {{
		    .role = Role::Bass,
		    .flavour = Flavour::Loop,
		    .pool = kQuietOne,
		    .densityGate = macroPoints(30),
		    .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 0, .gateTicks = 64 * kTicksPerStep},
		}};
		static constexpr Genre kGenre = streamGenre(kParts);
		Run run = runOf(kGenre, 50);
		run.steer = Steer{Macro::Density, {{4, 20}, {20, 30}}};
		run.ticksPerCall = 1;
		Result result = play(run, 3);
		expect(ticksOf(pitched(result, EventKind::NoteOn)) == std::set<int32_t>{0, 2 * kBarTicks}).to_be_true();
		auto offs = ticksOf(pitched(result, EventKind::NoteOff));
		expect(offs.count(kBarTicks)).to_equal(size_t{1});
	});
});

CPPSPEC_SPEC(gates)
