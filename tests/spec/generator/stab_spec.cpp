// The Stab flavour (spec §5.2, §8.1, §7.4): the voiced chord, at most 4 notes, on the onsets of a tiered rhythm
// pattern, thinned by Density like a drum lane.

#include "chord_moves.h"
#include "core_walk.h"
#include "generative/core/chord_walk.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// A skank: the chord on beats 2 and 4, both anchors.
constexpr Pattern kSkank[] = {{"....X.......X...", "................", 0}};

/// 4-bar phrases and no Arc offsets, so the Recipe's macros are what plays. runOf() starts in Groove at Motion 0, so
/// the Groove never ends and bar 0 is the only Section line. An empty Harmony slot: the home chord throughout, a plain
/// triad unless `ceiling` allows Extensions.
constexpr Genre stabGenre(std::span<const PartSpec> parts, uint8_t floor = 0, uint8_t ceiling = 0) {
	return {
	    .name = "stab",
	    .facts = {.scales = kScales},
	    .phraseBars = 4,
	    .parts = parts,
	    .harmony = {.extensionFloor = floor, .extensionCeiling = ceiling},
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

constexpr PartSpec kSkankParts[] = {
    {.role = Role::Chords, .flavour = Flavour::Stab, .name = "STAB", .pool = kSkank, .stab = {.gateTicks = 36}}};
constexpr Genre kSkankGenre = stabGenre(kSkankParts);
/// Two Extensions on every chord.
constexpr Genre kExtendedGenre = stabGenre(kSkankParts, 2, 2);

/// A chord a bar, i and iv in turn: from i only iv weighs, and from iv home wins, except on a phrase's last chord,
/// where home is excluded and the walk goes from i to iv anyway.
constexpr ChordMove kTurns[] = {{kI, kIV, 100}, {kIV, kI, 100}};
constexpr ChordGraph kTurnGraph{.moves = kTurns, .barsPerChord = 1};
constexpr Genre kTurningGenre = [] {
	Genre genre = stabGenre(kSkankParts);
	genre.harmony.graph = &kTurnGraph;
	return genre;
}();

/// Anchors on beats 1 and 3, and steps Density thins in between.
constexpr Pattern kThinned[] = {{"X.5.7.3.X.5.7.3.", "................", 0}};
constexpr PartSpec kThinnedParts[] = {
    {.role = Role::Chords, .flavour = Flavour::Stab, .name = "STAB", .pool = kThinned, .stab = {.gateTicks = 12}}};
constexpr Genre kThinnedGenre = stabGenre(kThinnedParts);

/// Onsets two 16ths apart, and on the last 16th of the bar: a long gate ends at each.
constexpr Pattern kClose[] = {{"X.X............X", "................", 0}};
constexpr PartSpec kLongParts[] = {
    {.role = Role::Chords, .flavour = Flavour::Stab, .name = "STAB", .pool = kClose, .stab = {.gateTicks = 72}}};
constexpr Genre kLongGenre = stabGenre(kLongParts);

constexpr PartSpec kGatedParts[] = {{.role = Role::Chords,
                                     .flavour = Flavour::Stab,
                                     .name = "STAB",
                                     .pool = kSkank,
                                     .densityGate = macroPoints(30),
                                     .stab = {.gateTicks = 36}}};
constexpr Genre kGatedGenre = stabGenre(kGatedParts);

/// The skank, which the held-fill entry rests alone (spec §8.7).
constexpr FillEntry kRestTheStab[] = {{.kind = FillKind::Rest, .mask = {.parts = partBit(0)}, .beats = 1}};
constexpr Genre kRestingGenre = [] {
	Genre genre = stabGenre(kSkankParts);
	genre.fills = kRestTheStab;
	genre.heldFill = 0;
	genre.fillChance = 0;
	return genre;
}();

Run runOf(const Genre& genre, int32_t density = 50) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.keyRoot = 9; // A: the Stab's tonic is A3 (57), the A nearest its anchor C4
	run.recipe.scale = Scale::Minor;
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Motion, 0);
	run.skipIntro = true;
	return run;
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

/// Note-ons or note-offs at `tick`, by pitch.
std::set<int32_t> pitchesAt(const std::vector<Event>& events, int32_t tick) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		if (e.tick == tick) {
			out.insert(e.pitch);
		}
	}
	return out;
}

constexpr int32_t kStep = kTicksPerStep;

/// A Techno-like roster with a Stab (no built-in genre plays one yet): Techno's Drums, Bass and FX, and a Chords
/// Stab on tiered patterns, over Melodic techno's Chord graph with up to two Extensions.
constexpr Pattern kStabPool[] = {
    {"..X...X...X...X.", "................", 0},                                    // offbeat 8ths
    {"..X..7X...X.5.X.", "..........a.....", 1},                                    // pushes
    {"..X..7X.3.X.5.X3", "......a.......a.", 2},                                    // busier pushes
    {"..X...X...X..7X.....X...X..5X..3", "................................", 2, 2}, // 2-bar
};
constexpr Expression kStabExpression{.lean = {0, 0, 2, 0}, .spread = 6};
const PartSpec kTechnoStabParts[] = {
    genres::technoAcid.parts[0],
    genres::technoAcid.parts[1],
    {.role = Role::Chords,
     .flavour = Flavour::Stab,
     .name = "STAB",
     .pool = kStabPool,
     .band = {0, 2},
     .stab = {.gateTicks = 36},
     .expression = kStabExpression},
    genres::technoAcid.parts.back(), // FX
};
const Genre kTechnoStab = [] {
	Genre genre = genres::technoMelodic;
	genre.name = "techno-stab";
	genre.parts = kTechnoStabParts;
	genre.harmony.extensionCeiling = 2;
	return genre;
}();
constexpr uint8_t kStabPart = 2;

/// `bars` of `genre` from a Groove, straight through the core, which keeps a muted note-on in the stream with its flag
/// (the driver drops it): each command pushed before its step is decided.
Walked walkSteered(const Genre& genre, std::vector<std::pair<int32_t, Command>> commands, int32_t bars) {
	return walk(
	    runOf(genre).recipe, 1, bars * kStepsPerBar,
	    [commands](int32_t step, const Generator&, SteeringQueue& queue) {
		    for (const auto& [at, command] : commands) {
			    if (at == step) {
				    queue.push(command);
			    }
		    }
	    },
	    /*skipIntro=*/true);
}

/// The Stab's note-ons or note-offs at `tick`.
std::vector<Event> eventsAt(const Walked& walked, EventKind kind, int32_t tick) {
	std::vector<Event> out;
	for (const Event& e : walked.notes) {
		if (e.kind == kind && e.tick == tick) {
			out.push_back(e);
		}
	}
	return out;
}

/// Steers `macro` to `value` at `step`.
std::function<void(int32_t, SteeringQueue&)> steer(Macro macro, int32_t step, int32_t value) {
	return steerAt({{step, {CommandKind::SetMacro, static_cast<uint8_t>(macro), macroPoints(value)}}});
}

} // namespace

// clang-format off
describe stab("The Stab flavour", $ {
	it("plays the voiced chord on each onset of its pattern, each note lasting its gate", _ {
		Result result = play(runOf(kSkankGenre), 2);
		auto ons = pitched(result, EventKind::NoteOn);
		auto offs = pitched(result, EventKind::NoteOff);
		// A minor's i, voiced around the tonic A3 inside the Chords Register (52..69): E3, A3, C4.
		const std::set<int32_t> chord{52, 57, 60};
		const std::set<int32_t> onsets{4 * kStep, 12 * kStep, kBarTicks + 4 * kStep, kBarTicks + 12 * kStep};
		expect(ticksOf(ons) == onsets).to_be_true();
		for (int32_t tick : onsets) {
			expect(pitchesAt(ons, tick) == chord).to_be_true();
			expect(pitchesAt(offs, tick + 36) == chord).to_be_true();
		}
		expect(ons.size()).to_equal(size_t{12});
		expect(result.evictedNotes).to_equal(0U);
		expect(result.droppedEvents).to_equal(0U);
	});

	it("voices the extended chord, at most 4 notes", _ {
		auto ons = pitched(play(runOf(kExtendedGenre), 1), EventKind::NoteOn);
		// A minor's i at Extension level 2 is Am9 without its 5th: A, C, G and B, voiced around A3 in 52..69.
		const std::set<int32_t> chord{55, 57, 59, 60};
		expect(pitchesAt(ons, 4 * kStep) == chord).to_be_true();
		expect(pitchesAt(ons, 12 * kStep) == chord).to_be_true();
		expect(ons.size()).to_equal(size_t{8});
	});

	it("follows the chord, voice-led from the voicing before, and holds the voicing for the whole chord", _ {
		auto ons = pitched(play(runOf(kTurningGenre), 3), EventKind::NoteOn);
		// i, iv, i. Each note goes to the instance of its pitch class nearest the voicing before: i around the tonic
		// is E3 A3 C4; iv keeps A3, takes F3 by E3 and D4 by C4 (D3 is below the Register); i goes back.
		const std::set<int32_t> one{52, 57, 60};
		const std::set<int32_t> four{53, 57, 62};
		for (int32_t bar = 0; bar < 3; bar++) {
			const std::set<int32_t>& chord = bar == 1 ? four : one;
			expect(pitchesAt(ons, bar * kBarTicks + 4 * kStep) == chord).to_be_true();
			expect(pitchesAt(ons, bar * kBarTicks + 12 * kStep) == chord).to_be_true();
		}
		expect(ons.size()).to_equal(size_t{18});
	});

	context("under Density (spec §8.1)", _ {
		it("plays only its anchors at Density 0, each with the whole chord", _ {
			auto ons = pitched(play(runOf(kThinnedGenre, 0), 2), EventKind::NoteOn);
			expect(ticksOf(ons) == std::set<int32_t>{0, 8 * kStep, kBarTicks, kBarTicks + 8 * kStep}).to_be_true();
			expect(ons.size()).to_equal(size_t{12});
		});

		it("only ever adds onsets as Density rises, never swapping one for another, and plays them all at 100", _ {
			std::set<int32_t> before;
			for (int32_t density = 0; density <= 100; density += 5) {
				std::set<int32_t> now = ticksOf(pitched(play(runOf(kThinnedGenre, density), 2), EventKind::NoteOn));
				for (int32_t tick : before) {
					expect(now.count(tick)).to_equal(size_t{1});
				}
				before = now;
			}
			expect(before.size()).to_equal(size_t{16});
		});
	});

	it("ends a stab at the pattern's next onset or the next bar line when its gate would run past", _ {
		Result result = play(runOf(kLongGenre), 3);
		std::vector<Event> offs;
		for (const Event& off : pitched(result, EventKind::NoteOff)) {
			if (off.tick <= 2 * kBarTicks) {
				offs.push_back(off);
			}
		}
		// The stab on 1 ends at the onset two 16ths on, the one there lasts its gate, and the one on the last 16th
		// ends on the bar line, where the chord may change.
		expect(ticksOf(offs) == std::set<int32_t>{2 * kStep, 2 * kStep + 72, kBarTicks, kBarTicks + 2 * kStep,
		                                          kBarTicks + 2 * kStep + 72, 2 * kBarTicks})
		    .to_be_true();
		expect(result.evictedNotes).to_equal(0U);
	});

	it("plays the walk's chords on its patterns in a Techno-like roster, through whole Arcs", _ {
		int32_t stabs = 0;
		std::set<std::set<int32_t>> voicings;
		std::set<int32_t> sizes;
		for (uint32_t seed : {1U, 2U, 3U, 4U}) {
			for (int32_t complexity : {0, 25, 50, 100}) {
				Run run;
				run.recipe = acidRecipe();
				run.recipe.genre = &kTechnoStab;
				run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
				run.seed = seed;
				Result result = play(run, 8 * (2 + 8 + 2 + 2 + 2));
				expect(result.droppedEvents).to_equal(0U);
				expect(result.evictedNotes).to_equal(0U);
				std::map<int32_t, std::set<int32_t>> chordAt; // tick → the Stab's notes starting there
				int32_t sounding = 0;
				int32_t most = 0;
				for (const Event& e : result.events) {
					if (e.part != kStabPart || (e.kind != EventKind::NoteOn && e.kind != EventKind::NoteOff)) {
						continue;
					}
					if (e.kind == EventKind::NoteOn) {
						chordAt[e.tick].insert(e.pitch);
						most = std::max(most, ++sounding);
					}
					else {
						sounding--;
					}
				}
				// One voicing at a time: a stab ends by the next one.
				expect(most).to_be_less_than(kMaxChordNotes + 1);
				for (const auto& [tick, notes] : chordAt) {
					sizes.insert(static_cast<int32_t>(notes.size()));
					voicings.insert(notes);
					for (int32_t note : notes) {
						const Register reg = defaultRegister(Role::Chords);
						expect(note).to_be_greater_than(57 + reg.spanLow - 1);
						expect(note).to_be_less_than(57 + reg.spanHigh + 1);
					}
				}
				stabs += static_cast<int32_t>(chordAt.size());
			}
		}
		expect(stabs).to_be_greater_than(1000);
		// Triads and 4-note chords, never more, and the chord moves with the walk.
		expect(sizes == std::set<int32_t>{3, 4}).to_be_true();
		expect(voicings.size()).to_be_greater_than(size_t{8});
	});

	it("ends a sounding stab where a mid-bar Rest lands, plays none while it holds, and comes back on its next onset", _ {
		// FILL held from step 5, inside the stab on step 4 (ticks 96–132), and let go on step 14.
		const Walked walked = walkSteered(kRestingGenre, {{5, {CommandKind::SetFill, 0, kFillHeldEntry}},
		                                                  {14, {CommandKind::SetFill, 0, kFillReleased}}}, 2);
		std::set<int32_t> ons;
		for (const Event& e : walked.noteOns) {
			ons.insert(e.tick);
		}
		expect(ons == std::set<int32_t>{4 * kStep, kBarTicks + 4 * kStep, kBarTicks + 12 * kStep}).to_be_true();
		expect(eventsAt(walked, EventKind::NoteOff, 5 * kStep).size()).to_equal(size_t{3});
		expect(eventsAt(walked, EventKind::NoteOff, 4 * kStep + 36).empty()).to_be_true();
		expect(walked.evicted).to_equal(0U);
	});

	it("flags its stabs muted while a mute holds, cutting none that sounds, and plays them again once unmuted", _ {
		// SHIFT+mute on step 5, inside the stab on step 4, and SHIFT+unmute on step 14.
		const Walked walked = walkSteered(kSkankGenre, {{5, {CommandKind::SetMute, 0, kPressNow}},
		                                                {14, {CommandKind::SetMute, 0, kPressNow}}}, 2);
		std::map<int32_t, std::set<bool>> flags; // tick → the muted flags of the stab starting there
		for (const Event& e : walked.noteOns) {
			flags[e.tick].insert(e.muted);
		}
		expect(flags.size()).to_equal(size_t{4});
		expect(flags[4 * kStep] == std::set<bool>{false}).to_be_true();
		expect(flags[12 * kStep] == std::set<bool>{true}).to_be_true();
		expect(flags[kBarTicks + 4 * kStep] == std::set<bool>{false}).to_be_true();
		expect(flags[kBarTicks + 12 * kStep] == std::set<bool>{false}).to_be_true();
		// The stab sounding when the mute lands plays out its gate, and the muted one ends as it would have.
		expect(eventsAt(walked, EventKind::NoteOff, 4 * kStep + 36).size()).to_equal(size_t{3});
		expect(eventsAt(walked, EventKind::NoteOff, 12 * kStep + 36).size()).to_equal(size_t{3});
		expect(walked.evicted).to_equal(0U);
	});

	it("plays nothing while its Density gate is closed, and comes in on its next onset once it opens", _ {
		Run run = runOf(kGatedGenre, 20);
		run.steer = steer(Macro::Density, 20, 50);
		auto ons = pitched(play(run, 4), EventKind::NoteOn);
		// The gate opens on the bar line after the turn (bar 2). No stab lasts to a bar's end, so none enters late.
		expect(ticksOf(ons) == std::set<int32_t>{2 * kBarTicks + 4 * kStep, 2 * kBarTicks + 12 * kStep,
		                                         3 * kBarTicks + 4 * kStep, 3 * kBarTicks + 12 * kStep})
		    .to_be_true();
	});
});

CPPSPEC_SPEC(stab)
