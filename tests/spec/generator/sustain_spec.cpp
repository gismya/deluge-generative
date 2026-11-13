// The Sustain flavour (spec §5.2, §8.1, §5.6, §12.5): held voicings that re-strike at chord changes, the pedal, the
// retrigger period, Density thinning by note importance, late entry, and the voice leading between chords.

#include "generative/core/chord_walk.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// 4-bar phrases and no Arc offsets, so the Recipe's macros are what plays. runOf() starts in Groove at Motion 0, so
/// the Groove never ends and bar 0 is the only Section line. An empty Harmony slot: the home chord throughout, a plain
/// triad unless `ceiling` allows Extensions.
constexpr Genre padGenre(std::span<const PartSpec> parts, uint8_t ceiling = 0) {
	return {
	    .name = "sustain",
	    .facts = {.scales = kScales},
	    .phraseBars = 4,
	    .parts = parts,
	    .harmony = {.extensionCeiling = ceiling},
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

constexpr PartSpec kPadParts[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD"}};
constexpr Genre kPadGenre = padGenre(kPadParts);

constexpr PartSpec kGatedPadParts[] = {
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD", .densityGate = macroPoints(30)}};
constexpr Genre kGatedPadGenre = padGenre(kGatedPadParts);

constexpr PartSpec kRetriggerParts[] = {
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD", .sustain = {.retriggerBars = 2}}};
constexpr Genre kRetriggerGenre = padGenre(kRetriggerParts);

constexpr PartSpec kDoubledParts[] = {
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD", .sustain = {.doubleRoot = true}}};
constexpr Genre kDoubledGenre = padGenre(kDoubledParts);

/// A chord a bar, i and iv in turn: from i only iv weighs, and from iv home wins, except on a phrase's last chord,
/// where home is excluded and the walk goes from i to iv anyway.
constexpr ChordMove kTurns[] = {{kI, kIV, 100}, {kIV, kI, 100}};
constexpr ChordGraph kTurnGraph{.moves = kTurns, .barsPerChord = 1};
constexpr Genre withGraph(Genre genre) {
	genre.harmony.graph = &kTurnGraph;
	return genre;
}
constexpr Genre kTurningPad = withGraph(kPadGenre);
/// i and iv in turn, a phrase (4 bars) each.
constexpr ChordGraph kSlowTurnGraph{.moves = kTurns, .barsPerChord = 4};
constexpr Genre kSlowGatedPad = [] {
	Genre genre = padGenre(kGatedPadParts);
	genre.harmony.graph = &kSlowTurnGraph;
	return genre;
}();
constexpr PartSpec kPedalParts[] = {
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "DRONE", .sustain = {.pedal = true}}};
constexpr Genre kTurningPedal = withGraph(padGenre(kPedalParts));

Run runOf(const Genre& genre, int32_t density = 50) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.keyRoot = 9; // A: the Pad's tonic is A3 (57), the A nearest its anchor C4
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

std::multiset<int32_t> ticksOfAll(const std::vector<Event>& events) {
	std::multiset<int32_t> out;
	for (const Event& e : events) {
		out.insert(e.tick);
	}
	return out;
}

/// Note-ons at `tick`, by pitch.
std::set<int32_t> pitchesAt(const std::vector<Event>& events, int32_t tick) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		if (e.tick == tick) {
			out.insert(e.pitch);
		}
	}
	return out;
}

/// Steers `macro` to each value at its step.
std::function<void(int32_t, SteeringQueue&)> steer(Macro macro, std::vector<std::pair<int32_t, int32_t>> points) {
	std::vector<std::pair<int32_t, Command>> commands;
	for (auto [step, value] : points) {
		commands.push_back({step, {CommandKind::SetMacro, static_cast<uint8_t>(macro), macroPoints(value)}});
	}
	return steerAt(commands);
}

std::set<int32_t> pitchesOf(const std::vector<Event>& events) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		out.insert(e.pitch);
	}
	return out;
}

} // namespace

// clang-format off
describe sustain("The Sustain flavour", $ {
	it("holds its voicing across bar lines while the chord stays: one note-on per note, ended only at the stop", _ {
		Result result = play(runOf(kPadGenre), 6);
		auto ons = pitched(result, EventKind::NoteOn);
		// A minor's i, voiced around the tonic A3: E3, A3, C4.
		expect(pitchesOf(ons) == std::set<int32_t>{52, 57, 60}).to_be_true();
		expect(ticksOfAll(ons) == std::multiset<int32_t>{0, 0, 0}).to_be_true();
		for (const Event& off : pitched(result, EventKind::NoteOff)) {
			expect(off.tick).to_be_greater_than(5 * kBarTicks);
		}
		expect(result.evictedNotes).to_equal(0U);
	});

	it("re-strikes its whole voicing at each chord change, the old notes ending there, voice-led within the Register", _ {
		Result result = play(runOf(kTurningPad), 5);
		auto ons = pitched(result, EventKind::NoteOn);
		// i, iv, i, iv, i. Each note goes to the instance of its pitch class nearest the voicing before: i around the
		// tonic is E3 A3 C4; iv keeps A3, takes D3 (a tie with D4 goes down) and F3; i keeps A3 and moves to C3 and E3.
		expect(pitchesAt(ons, 0) == std::set<int32_t>{52, 57, 60}).to_be_true();
		expect(pitchesAt(ons, kBarTicks) == std::set<int32_t>{50, 53, 57}).to_be_true();
		expect(pitchesAt(ons, 2 * kBarTicks) == std::set<int32_t>{48, 52, 57}).to_be_true();
		expect(pitchesAt(ons, 3 * kBarTicks) == std::set<int32_t>{50, 53, 57}).to_be_true();
		expect(pitchesAt(ons, 4 * kBarTicks) == std::set<int32_t>{48, 52, 57}).to_be_true();
		expect(ons.size()).to_equal(size_t{15});
		auto offs = pitched(result, EventKind::NoteOff);
		for (int32_t bar = 1; bar <= 4; bar++) {
			expect(pitchesAt(offs, bar * kBarTicks) == pitchesAt(ons, (bar - 1) * kBarTicks)).to_be_true();
		}
		expect(result.evictedNotes).to_equal(0U);
	});

	it("holds a pedal's root and fifth through chord changes", _ {
		Result result = play(runOf(kTurningPedal), 5);
		auto ons = pitched(result, EventKind::NoteOn);
		expect(pitchesAt(ons, 0) == std::set<int32_t>{57, 64}).to_be_true(); // A3 and the E above
		expect(ons.size()).to_equal(size_t{2});
	});

	it("re-strikes on every retrigger line, counted from the phrase line", _ {
		Result result = play(runOf(kRetriggerGenre), 6);
		auto ons = pitched(result, EventKind::NoteOn);
		expect(ticksOfAll(ons) == std::multiset<int32_t>{0, 0, 0, 2 * kBarTicks, 2 * kBarTicks, 2 * kBarTicks,
		                                                 4 * kBarTicks, 4 * kBarTicks, 4 * kBarTicks})
		    .to_be_true();
	});

	it("adds the root an octave up when the part doubles it", _ {
		auto ons = pitched(play(runOf(kDoubledGenre), 2), EventKind::NoteOn);
		expect(pitchesOf(ons) == std::set<int32_t>{52, 57, 60, 69}).to_be_true();
	});

	context("under Density (spec §8.1)", _ {
		it("plays only the root, an anchor, at Density 0", _ {
			auto ons = pitched(play(runOf(kPadGenre, 0), 2), EventKind::NoteOn);
			expect(pitchesOf(ons) == std::set<int32_t>{57}).to_be_true();
		});

		it("brings a note in on the bar line Density lets it, and ends it on the bar line Density thins it", _ {
			Run run = runOf(kPadGenre, 0);
			run.steer = steer(Macro::Density, {{20, 50}, {52, 0}});
			Result result = play(run, 6);
			auto ons = pitched(result, EventKind::NoteOn);
			expect(pitchesAt(ons, 0) == std::set<int32_t>{57}).to_be_true();
			expect(pitchesAt(ons, 2 * kBarTicks) == std::set<int32_t>{52, 60}).to_be_true(); // the root holds
			expect(ons.size()).to_equal(size_t{3});
			auto offs = pitched(result, EventKind::NoteOff);
			expect(pitchesAt(offs, 4 * kBarTicks) == std::set<int32_t>{52, 60}).to_be_true();
		});

		it("keeps a note in until Density falls 2 below where it came in", _ {
			// At each Density, a step 1 point down ends nothing; 3 points down, past 2 below where a note came in, ends it.
			int32_t closed = 0;
			for (int32_t density = 2; density <= 50; density++) {
				for (int32_t fall : {1, 3}) {
					Run run = runOf(kPadGenre, density);
					run.steer = steer(Macro::Density, {{4, density - fall}});
					const auto offs = pitched(play(run, 3), EventKind::NoteOff);
					for (const Event& off : offs) {
						const bool early = off.tick <= 2 * kBarTicks;
						closed += early ? 1 : 0;
						expect(early && fall == 1).to_be_false();
					}
				}
			}
			expect(closed).to_be_greater_than(0);
		});
	});

	context("late entry and lines (spec §5.6, §12.5)", _ {
		it("comes in on the first bar line its gate allows, not at its next chord", _ {
			// i holds bars 0 to 3, iv starts on bar 4. The gate opens on bar 2, mid-chord: i comes in there.
			Run run = runOf(kSlowGatedPad, 20);
			run.steer = steer(Macro::Density, {{20, 30}});
			auto ons = pitched(play(run, 6), EventKind::NoteOn);
			expect(pitchesAt(ons, 2 * kBarTicks) == std::set<int32_t>{52, 57, 60}).to_be_true();
			expect(pitchesAt(ons, 4 * kBarTicks) == std::set<int32_t>{50, 53, 57}).to_be_true();
			expect(ons.size()).to_equal(size_t{6});
		});

		it("ends on the bar line its gate closes", _ {
			Run run = runOf(kGatedPadGenre, 30);
			run.steer = steer(Macro::Density, {{20, 20}});
			Result result = play(run, 4);
			expect(pitched(result, EventKind::NoteOn).size()).to_equal(size_t{3});
			expect(ticksOfAll(pitched(result, EventKind::NoteOff)) == std::multiset<int32_t>{2 * kBarTicks, 2 * kBarTicks, 2 * kBarTicks})
			    .to_be_true();
		});

		it("re-strikes the held voicing on a Section line", _ {
			Run run = runOf(kPadGenre);
			run.steer = at(20, force(SectionKind::Break)); // lands on the next phrase line, bar 4
			auto ons = pitched(play(run, 6), EventKind::NoteOn);
			expect(pitchesAt(ons, 4 * kBarTicks) == std::set<int32_t>{52, 57, 60}).to_be_true();
			expect(ons.size()).to_equal(size_t{6});
		});

		it("goes quiet on the bar line a mute lands, and comes back on the one an unmute lands", _ {
			Run run = runOf(kPadGenre);
			run.steer = steerAt({{20, mute(0)}, {52, mute(0)}});
			Result result = play(run, 6);
			// Perform drops the muted re-strike on bar 2 at the output, so the voicing ends there and starts again
			// on bar 4.
			auto ons = pitched(result, EventKind::NoteOn);
			expect(ticksOfAll(ons) == std::multiset<int32_t>{0, 0, 0, 4 * kBarTicks, 4 * kBarTicks, 4 * kBarTicks})
			    .to_be_true();
			auto offs = pitched(result, EventKind::NoteOff);
			expect(pitchesAt(offs, 2 * kBarTicks) == std::set<int32_t>{52, 57, 60}).to_be_true();
		});
	});
});

CPPSPEC_SPEC(sustain)
