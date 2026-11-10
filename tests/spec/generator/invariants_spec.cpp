// The Generator's invariants (spec §21.2), checked on Techno through the Perform driver across seeds, scales and
// macro extremes.

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
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
		// Every macro at 0 may leave the bass silent (it has no anchors), so the count is over all cases.
		int32_t pitched = 0;
		for (const Case& c : kCases) {
			Result result = playCase(c);
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
		}
		expect(pitched).to_be_greater_than(0);
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

	it("sends every Control event's offset already clamped to the control's range", _ {
		// Every macro at 50 pushes Energy's layer, Rise and drift past the range together, so the clamp is reached.
		bool reachedEnd = false;
		for (const Case& c : kCases) {
			for (const Event& e : only(playCase(c).events, EventKind::Control)) {
				expect(static_cast<int32_t>(e.offset)).to_be_between(-kControlFull, kControlFull);
				reachedEnd = reachedEnd || e.offset == kControlFull || e.offset == -kControlFull;
			}
		}
		expect(reachedEnd).to_be_true();
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
		static constexpr Genre kFullKit{.name = "fullkit", .parts = kParts, .replacePairs = {}};
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
		size_t most = 0;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn) {
				sounding[keyOf(e)] = e.tick;
				most = std::max(most, sounding.size());
			}
			else if (e.kind == EventKind::NoteOff) {
				expect(sounding.erase(keyOf(e))).to_equal(size_t{1});
			}
		}
		expect(sounding.empty()).to_be_true();
		// A pitched part sounds at most two chords' worth at once, what the event buffer counts for it.
		expect(most).to_equal(static_cast<size_t>(2 * kMaxChordNotes));
	});

	it("never sends a note-off twice when an overflowing step drops the note it made room for", _ {
		// Bad data: six Kits, every lane hit on every step and ratcheted 4 times, so a step overflows the buffer and
		// ending a hit's earlier hit can push out the new hit itself.
		static constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
		static constexpr Expression kRatchet4{.ornaments = {.rateFloor = 1000, .rateCeiling = 1000, .ratchet = {0, 0, 1}}};
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::Kick, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Snare, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Clap, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Rim, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::ClosedHat, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::OpenHat, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Ride, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Crash, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Tom, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Perc1, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		    {.lane = Lane::Perc2, .pool = kEveryStep, .gateTicks = 200, .expression = kRatchet4},
		};
		static constexpr PartSpec kKits[] = {
		    {.role = Role::Drums, .lanes = kLanes}, {.role = Role::Drums, .lanes = kLanes},
		    {.role = Role::Drums, .lanes = kLanes}, {.role = Role::Drums, .lanes = kLanes},
		    {.role = Role::Drums, .lanes = kLanes}, {.role = Role::Drums, .lanes = kLanes},
		};
		static constexpr Genre kOverflowing{.name = "overflowing", .parts = kKits, .replacePairs = {}};
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &kOverflowing;
		run.recipe.macros.set(Macro::Motion, kMacroFull); // every lane's drift moving too, on every step
		run.ticksPerCall = 1;
		Result result = play(run, 2);
		expect(result.droppedEvents).to_be_greater_than(0U);
		std::map<NoteKey, int32_t> sounding;
		int32_t strays = 0;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn) {
				sounding[keyOf(e)] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff) {
				strays += sounding.erase(keyOf(e)) == 1 ? 0 : 1;
			}
		}
		expect(strays).to_equal(0);
		expect(sounding.empty()).to_be_true();
	});

	it("reports voice starts per step and per tick against the budget of 4 per render", _ {
		// Reported, not failed: the firmware defers extra starts to the next render (spec §12.6, §23.14). At 130 BPM a
		// render is shorter than a tick, so the budget is per tick.
		int32_t worstStep = 0;
		int32_t overStep = 0;
		int32_t worstTick = 0;
		int32_t overTick = 0;
		int32_t ornaments = 0;
		int32_t steps = 0;
		for (const Case& c : kCases) {
			std::map<int32_t, int32_t> perStep;
			std::map<int32_t, int32_t> perTick;
			for (const Event& e : only(playCase(c).events, EventKind::NoteOn)) {
				perStep[(e.tick + kTicksPerStep / 2) / kTicksPerStep]++;
				perTick[e.tick]++;
				ornaments += e.ornament ? 1 : 0;
			}
			for (auto [step, starts] : perStep) {
				worstStep = starts > worstStep ? starts : worstStep;
				overStep += starts > 4 ? 1 : 0;
			}
			for (auto [tick, starts] : perTick) {
				worstTick = starts > worstTick ? starts : worstTick;
				overTick += starts > 4 ? 1 : 0;
			}
			steps += kBars * kStepsPerBar;
		}
		std::printf("voice starts: worst %d in one step, %d of %d steps over 4; worst %d in one tick, %d ticks over 4; "
		            "%d Ornament hits\n",
		            worstStep, overStep, steps, worstTick, overTick, ornaments);
		expect(worstStep).to_be_greater_than(0);
	});
});

CPPSPEC_SPEC(invariants)
