// The Generator's invariants (spec §21.2), checked on Techno through the Perform driver across seeds, scales and
// macro extremes.

#include "harness.h"

#include "cppspec.hpp"

#include <cstdio>
#include <map>
#include <tuple>

using namespace harness;

namespace {

struct Case {
	uint32_t seed;
	Scale scale;
	uint8_t keyRoot;
	int32_t points; ///< Every macro at this value.
};

const Case kCases[] = {
    {1, Scale::Minor, 9, 25},    {2, Scale::Phrygian, 0, 50}, {3, Scale::Minor, 11, 0},
    {4, Scale::Phrygian, 6, 50}, {5, Scale::Minor, 3, 40},    {0xDEADBEEF, Scale::Phrygian, 1, 10},
};

/// Long enough for the Intro, a Groove, a Break, a Build and a Drop at any seed.
constexpr int32_t kBars = 8 * (2 + 8 + 2 + 2 + 2);

Result playCase(const Case& c) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.scale = c.scale;
	run.recipe.keyRoot = c.keyRoot;
	for (int32_t m = 0; m < kNumMacros; m++) {
		run.recipe.macros.set(static_cast<Macro>(m), macroPoints(c.points));
	}
	run.seed = c.seed;
	return play(run, kBars);
}

using NoteKey = std::tuple<uint8_t, Lane, uint8_t>;
NoteKey keyOf(const Event& e) {
	return {e.part, e.lane, e.pitch};
}

} // namespace

// clang-format off
describe invariants("The Generator's invariants", $ {
	it("plays every pitched note in the scale and in its part's Register", _ {
		for (const Case& c : kCases) {
			Result result = playCase(c);
			int32_t pitched = 0;
			for (const Event& e : only(result.events, EventKind::NoteOn)) {
				if (e.lane != Lane::None) {
					continue;
				}
				pitched++;
				const PartSpec& part = genres::techno.parts[e.part];
				Register reg = part.registerOrDefault();
				// The tonic: the key root nearest the anchor, the lower one on a tie (spec §7.3).
				int32_t tonic = reg.anchor - 6;
				while ((tonic - c.keyRoot) % 12 != 0) {
					tonic++;
				}
				expect(inScale(e.pitch % 12, c.keyRoot, c.scale)).to_be_true();
				expect(static_cast<int32_t>(e.pitch)).to_be_between(tonic + reg.spanLow, tonic + reg.spanHigh);
			}
			expect(pitched).to_be_greater_than(0);
		}
	});

	it("gives every note-on a note-off, after it, and no stray note-offs", _ {
		for (const Case& c : kCases) {
			Result result = playCase(c);
			std::map<NoteKey, int32_t> sounding; // key → note-on tick
			int32_t ons = 0;
			for (const Event& e : result.events) {
				if (e.kind == EventKind::NoteOn) {
					expect(sounding.count(keyOf(e))).to_equal(size_t{0}); // the old one ended first
					sounding[keyOf(e)] = e.tick;
					ons++;
				}
				else if (e.kind == EventKind::NoteOff) {
					auto found = sounding.find(keyOf(e));
					expect(found != sounding.end()).to_be_true();
					if (found != sounding.end()) {
						expect(e.tick).to_be_greater_than(found->second);
						sounding.erase(found);
					}
				}
			}
			expect(ons).to_be_greater_than(100);
			expect(sounding.empty()).to_be_true();
		}
	});

	it("never overflows a step's event buffer or a part's sounding-note table", _ {
		for (const Case& c : kCases) {
			Result result = playCase(c);
			expect(result.droppedEvents).to_equal(0U);
			expect(result.evictedNotes).to_equal(0U);
		}
	});

	it("fits every lane of a Kit sounding at once, retriggered every step, in the table", _ {
		// Valid data: 11 lanes hit on every step with notes longer than a step. A repeat hit ends its lane's own
		// note, so the table never fills (spec §2.7).
		static constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::Kick, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Snare, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Clap, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Rim, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::ClosedHat, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::OpenHat, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Ride, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Crash, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Tom, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Perc1, .pool = kEveryStep, .gateTicks = 200},
		    {.lane = Lane::Perc2, .pool = kEveryStep, .gateTicks = 200},
		};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kLanes}};
		static constexpr Genre kFullKit{.name = "fullkit", .parts = kParts};
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &kFullKit;
		Result result = play(run, 4);
		expect(only(result.events, EventKind::NoteOn).size()).to_equal(size_t{4 * kStepsPerBar * kNumDrumLanes});
		expect(result.evictedNotes).to_equal(0U);
		expect(result.droppedEvents).to_equal(0U);
	});

	it("reports a full sounding-note table, and still ends every note it starts", _ {
		// Bad genre data: a Loop whose every step is an anchor and whose notes last a bar, on some twenty pitches,
		// so more notes sound at once than a part's table holds.
		static constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
		static constexpr DegreeWeight kDegrees[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}};
		static constexpr PartSpec kParts[] = {{
		    .role = Role::Lead,
		    .flavour = Flavour::Loop,
		    .pool = kEveryStep,
		    .reg = {60, -36, 36},
		    .loop = {.degrees = kDegrees, .octaveUp = 330, .octaveDown = 500, .slide = 0, .gateTicks = 16 * kTicksPerStep},
		}};
		static constexpr Genre kCrowded{.name = "crowded", .parts = kParts};
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &kCrowded;
		Result result = play(run, 8);
		expect(result.evictedNotes).to_be_greater_than(0U);
		std::map<NoteKey, int32_t> sounding;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn) {
				sounding[keyOf(e)] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff) {
				expect(sounding.erase(keyOf(e))).to_equal(size_t{1});
			}
		}
		expect(sounding.empty()).to_be_true();
	});

	it("reports voice starts per step against the budget of 4 per render", _ {
		// Reported, not failed: the firmware defers extra starts to the next render (spec §12.6, §23.14).
		int32_t worst = 0;
		int32_t over = 0;
		int32_t steps = 0;
		for (const Case& c : kCases) {
			std::map<int32_t, int32_t> perStep;
			for (const Event& e : only(playCase(c).events, EventKind::NoteOn)) {
				perStep[(e.tick + kTicksPerStep / 2) / kTicksPerStep]++;
			}
			for (auto [step, starts] : perStep) {
				worst = starts > worst ? starts : worst;
				over += starts > 4 ? 1 : 0;
			}
			steps += kBars * kStepsPerBar;
		}
		std::printf("voice starts: worst %d in one step, %d of %d steps over 4\n", worst, over, steps);
		expect(worst).to_be_greater_than(0);
	});
});

CPPSPEC_SPEC(invariants)
