// Drop-outs in the event stream (spec §8.8): when they play, how sounding notes ring and a Sustain comes back, the
// Throw on the target held over them (§11.4), what ends one early, their precedence over fills (§8.7), the dim and the
// Arc plan (§13.5), takes and freeze, and genreValid()'s check. Played on the spec's own genre, which pins each case's
// entries; Dub's own drop-outs are dub_spec's.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <utility>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;

constexpr Pattern kFourOnTheFloor[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kBackbeat[] = {{"....X.......X...", "................", 0}};
constexpr Pattern kOffBeats[] = {{"..X...X...X...X.", "................", 0}};
constexpr Pattern kTomFill[] = {{"............X.X.", "................", 0}};
constexpr Pattern kSnareFill[] = {{"............XXXX", "................", 0}};
/// The Bass hits on the last beat of each bar, so it has a hit to throw.
constexpr Pattern kBassHits[] = {{"X.......X...X...", "................", 0}};
constexpr LineWeight kRoot[] = {{0, 1}};

constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor}, {.lane = Lane::Snare, .pool = kBackbeat, .fillPool = kSnareFill},
    {.lane = Lane::ClosedHat, .pool = kOffBeats},  {.lane = Lane::Crash}, // the accent lane
    {.lane = Lane::Tom, .fillPool = kTomFill},
};
constexpr LaneSpec kFx[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};
constexpr LaneSpec kFxWithSiren[] = {
    {.lane = Lane::Riser},
    {.lane = Lane::Fall},
    {.lane = Lane::Impact},
    {.lane = Lane::Siren, .delay = {.on = true}, .heldBars = 2},
};

/// No drift, so Space shows the Throw alone; the Drums' Throw acts on the Snare's row.
constexpr DriftSpec kNoDrift{.weight = 0};
constexpr GestureSpec kThrowOnSnare = [] {
	GestureSpec gestures;
	gestures.aim[static_cast<int32_t>(Gesture::Throw)] = Lane::Snare;
	return gestures;
}();
/// A Bass note lasts 6 steps, so the one on the last beat rings 2 steps into a drop-out after it.
constexpr int32_t kBassSteps = 6;
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums, .gestures = kThrowOnSnare, .drift = kNoDrift},
    {.role = Role::Bass,
     .flavour = Flavour::Loop,
     .name = "BASS",
     .pool = kBassHits,
     .loop =
         {.degrees = kRoot, .draws = {.octaveUp = 0, .accent = 0}, .slide = 0, .gateTicks = kBassSteps * kTicksPerStep},
     .drift = kNoDrift},
    {.role = Role::Pad, .flavour = Flavour::Sustain, .name = "PAD", .drift = kNoDrift},
    {.role = Role::FX, .name = "FX", .lanes = kFx, .drift = kNoDrift},
};
/// The same with a delay setup on the Bass (spec §10.6), so its Throw lifts delay feedback too.
constexpr PartSpec kDelayParts[] = {kParts[0],
                                    [] {
	                                    PartSpec bass = kParts[1];
	                                    bass.delay = {.on = true};
	                                    return bass;
                                    }(),
                                    kParts[2], kParts[3]};
/// The same with the Drums' Throw kit-wide.
constexpr PartSpec kKitWideParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums, .drift = kNoDrift}, kParts[1], kParts[2], kParts[3]};
/// Lines that change every pass, so a spec can see them advance: a Loop whose mutation re-rolls most steps and a
/// Motif re-rolled as often, each with several degrees or positions.
constexpr Pattern kEighths[] = {{"X.X.X.X.X.X.X.X.", "................", 0}};
constexpr Pattern kQuarters[] = {{"X...X...X...X...", "................", 0}};
constexpr LineWeight kDegrees[] = {{0, 1}, {2, 1}, {4, 1}, {5, 1}, {-3, 1}};
constexpr LineWeight kPositions[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}};
constexpr PartSpec kLineParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrums, .drift = kNoDrift},
    {.role = Role::Bass,
     .flavour = Flavour::Loop,
     .name = "BASS",
     .pool = kEighths,
     .loop = {.degrees = kDegrees, .slide = 0, .mutation = 900, .gateTicks = kTicksPerStep},
     .drift = kNoDrift},
    {.role = Role::Lead,
     .flavour = Flavour::Motif,
     .name = "MOTIF",
     .pool = kQuarters,
     .motif = {.positions = kPositions, .reroll = 900, .gateTicks = kTicksPerStep},
     .drift = kNoDrift},
    {.role = Role::FX, .name = "FX", .lanes = kFx, .drift = kNoDrift},
};
constexpr uint8_t kLinePart = 2;
constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr uint8_t kPadPart = 2;
constexpr Scale kScales[] = {Scale::Minor};

constexpr PartsAndLanes kBass{.parts = partBit(kBassPart)};
constexpr PartsAndLanes kPad{.parts = partBit(kPadPart)};
constexpr PartsAndLanes kSnare{.lanes = laneBit(Lane::Snare)};

constexpr DropOutEntry kBassTwoBars[] = {{.mask = kBass, .bars = 2, .throwTarget = kBass}};
constexpr DropOutEntry kBassFourBars[] = {{.mask = kBass, .bars = 4, .throwTarget = kBass}};
constexpr DropOutEntry kBassOneBar[] = {{.mask = kBass, .bars = 1, .throwTarget = kBass}};
constexpr DropOutEntry kPadOneBar[] = {{.mask = kPad, .bars = 1, .throwTarget = kPad}};
constexpr DropOutEntry kSnareAndHatsOneBar[] = {
    {.mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::ClosedHat)}, .bars = 1, .throwTarget = kSnare}};
constexpr DropOutEntry kBassAndSnareOneBar[] = {
    {.mask = {.parts = kBass.parts, .lanes = kSnare.lanes}, .bars = 1, .throwTarget = kBass}};
constexpr DropOutEntry kMixed[] = {
    {.mask = kBass, .bars = 2, .weight = 2, .throwTarget = kBass},
    {.mask = kPad, .bars = 1, .throwTarget = kPad},
    {.mask = {.parts = kBass.parts | kPad.parts}, .bars = 4},
};

/// The phrase-end fills: a kick drop over the Bass, and a snare run on Snare and Tom.
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Rest, .mask = {.parts = kBass.parts, .lanes = laneBit(Lane::Kick)}, .beats = 2},
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 1},
};
constexpr FillEntry kSnareRun[] = {kFills[1]};

constexpr WeightedBars kBuild8[] = {{8, 1}};
constexpr WeightedBars kBuild2[] = {{2, 1}};
constexpr WeightedBars kBuild4[] = {{4, 1}};

struct Shape {
	std::span<const DropOutEntry> dropOuts;
	int16_t chance = 1000;
	uint8_t gapBars = 1;
	std::span<const FillEntry> fills = {};
	int16_t fillChance = 0;
	std::span<const WeightedBars> build = kBuild8;
	int16_t grooveToBuild = 0;
	Range groove{2, 2};
	std::span<const PartSpec> parts = kParts;
	PartsAndLanes breakKeeps = kEverything;
};

/// An Intro of a phrase, a Groove of `shape.groove` phrases at Motion 25, a Break and a Build. Flat macros, no macro
/// layer and no drift, so only drop-outs, fills and gestures change what plays and the Space sent.
constexpr Genre dropGenre(const Shape& shape) {
	return {
	    .name = "drop-outs",
	    .facts = {.scales = kScales},
	    .soundWeight = {},
	    .parts = shape.parts,
	    .fills = shape.fills,
	    .fillChance = shape.fillChance,
	    .dropOuts = shape.dropOuts,
	    .dropOutChance = shape.chance,
	    .dropOutGapBars = shape.gapBars,
	    .palette = &genres::technoPalette,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = shape.groove,
	            .breakPhrases = {1, 1},
	            .buildBars = shape.build,
	            .grooveToBuild = shape.grooveToBuild,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	            .breakKeeps = shape.breakKeeps,
	            .buildCut = {},
	            .gap = GapLength::None,
	            .accentLane = Lane::Crash,
	        },
	};
}

Recipe dropRecipe(const Genre& genre, int32_t motion = macroPoints(25)) {
	Recipe recipe = newRecipe(genre, 0);
	recipe.macros.set(Macro::Density, kMacroFull);
	recipe.macros.set(Macro::Motion, motion);
	return recipe;
}

Run dropRun(const Genre& genre, uint32_t seed = 1, int32_t motion = macroPoints(25)) {
	Run run;
	run.recipe = dropRecipe(genre, motion);
	run.seed = seed;
	run.skipIntro = true;
	return run;
}

/// A run of steps where a drop-out silenced a part: [from, to).
struct Span {
	int32_t from;
	int32_t to;
	bool operator==(const Span&) const = default;
};

/// What a piece played, step by step: whether `part` was silenced, and the Section and place each step was in.
struct Walk {
	std::vector<bool> silenced;
	std::vector<SectionKind> section;
	std::vector<int32_t> stepInPhrase;
	std::vector<Span> spans;
};

/// Steps a piece through `steps` steps, reading the dim of `part` after each decision. `steer(step, piece)` runs
/// before each step is decided.
template <typename Steer>
Walk walkPiece(const Start& start, int32_t steps, uint8_t part, Steer steer) {
	StepwisePiece<> piece{start};
	Walk walk;
	for (int32_t s = 0; s < steps; s++) {
		steer(s, piece);
		if (!piece.decideBefore(s + 1)) {
			break;
		}
		const Generator& g = piece.generator;
		walk.silenced.push_back(g.partSilenced(part));
		walk.section.push_back(g.arc().section());
		walk.stepInPhrase.push_back(g.arc().stepInPhrase());
	}
	for (int32_t s = 0; s < static_cast<int32_t>(walk.silenced.size()); s++) {
		if (walk.silenced[s] && (s == 0 || !walk.silenced[s - 1])) {
			walk.spans.push_back({s, s + 1});
		}
		else if (walk.silenced[s]) {
			walk.spans.back().to = s + 1;
		}
	}
	return walk;
}
Walk walkPiece(const Start& start, int32_t steps, uint8_t part) {
	return walkPiece(start, steps, part, [](int32_t, StepwisePiece<>&) {});
}

Start startOf(const Genre& genre, uint32_t seed = 1, int32_t motion = macroPoints(25)) {
	return {.recipe = dropRecipe(genre, motion), .seed = seed, .skipIntro = true};
}

/// The first note of `part` starting at `tick`, from its note-on to its note-off.
const Held* noteAt(const std::vector<Held>& notes, int32_t tick) {
	for (const Held& note : notes) {
		if (note.on == tick) {
			return &note;
		}
	}
	return nullptr;
}

std::vector<Event> notesOf(const Result& result) {
	std::vector<Event> out;
	for (const Event& e : result.events) {
		if (e.kind == EventKind::NoteOn || e.kind == EventKind::NoteOff) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe drop_outs("Drop-outs", $ {
	it("play only in Groove phrases, from a bar line, for the entry's bars, inside the phrase, a gap apart", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kMixed, .gapBars = 2});
		int32_t played = 0;
		for (uint32_t seed = 1; seed <= 12; seed++) {
			const Walk walk = walkPiece(startOf(kGenre, seed), 6 * kPhraseSteps, kBassPart);
			int32_t lastEnd = -1000;
			for (const Span& span : walk.spans) {
				played++;
				expect(span.from % kStepsPerBar).to_equal(0);
				const int32_t bars = (span.to - span.from) / kStepsPerBar;
				expect((span.to - span.from) % kStepsPerBar).to_equal(0);
				expect(bars == 1 || bars == 2 || bars == 4).to_be_true();
				// Within one phrase, every step in a Groove.
				expect(walk.stepInPhrase[span.from] + (span.to - span.from)).to_be_less_than(kPhraseSteps + 1);
				for (int32_t s = span.from; s < span.to; s++) {
					expect(walk.section[s] == SectionKind::Groove).to_be_true();
				}
				expect(span.from - lastEnd).to_be_greater_than(2 * kStepsPerBar - 1);
				lastEnd = span.to;
			}
		}
		expect(played).to_be_greater_than(20);
	});

	it("start a bar after their draw: one each bar the gap allows, at chance 1000", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kBassOneBar});
		const Walk walk = walkPiece(startOf(kGenre), 2 * kPhraseSteps, kBassPart);
		// Drawn on bar 0's line for bar 1, then on each line it ends on for the bar after.
		const std::vector<Span> expected = {{16, 32}, {48, 64}, {80, 96}, {112, 128}, {144, 160}, {176, 192},
		                                    {208, 224}, {240, 256}};
		expect(walk.spans == expected).to_be_true();
	});

	it("never start at Motion 0, and come more often as Motion rises", _ {
		static constexpr DropOutEntry kRare[] = {{.mask = kBass, .bars = 1}};
		static constexpr Genre kGenre = dropGenre({.dropOuts = kRare, .chance = 100, .groove = {4, 4}});
		const auto count = [](int32_t motion) {
			size_t spans = 0;
			for (uint32_t seed = 1; seed <= 8; seed++) {
				spans += walkPiece(startOf(kGenre, seed, motion), 4 * kPhraseSteps, kBassPart).spans.size();
			}
			return spans;
		};
		expect(count(0)).to_equal(size_t{0});
		expect(count(macroPoints(50))).to_be_greater_than(count(macroPoints(15)));
	});

	it("end by a Build that takes the end of the phrase, known when they're drawn: too long an entry isn't drawn", _ {
		// The Groove ends after a phrase into a 4-bar Build, which takes the phrase's last 4 bars. From bar 0's line
		// only the 1-bar entry fits before it; drawn, the 4-bar one would be cut short there.
		static constexpr DropOutEntry kLongOrShort[] = {{.mask = kBass, .bars = 4, .weight = 3},
		                                                {.mask = kBass, .bars = 1}};
		static constexpr Genre kGenre =
		    dropGenre({.dropOuts = kLongOrShort, .build = kBuild4, .grooveToBuild = 1000, .groove = {1, 1}});
		int32_t played = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Walk walk = walkPiece(startOf(kGenre, seed), 4 * kPhraseSteps, kBassPart);
			for (const Span& span : walk.spans) {
				played++;
				expect(span.to - span.from).to_equal(kStepsPerBar);
				for (int32_t s = span.from; s < span.to; s++) {
					expect(walk.section[s] == SectionKind::Groove).to_be_true();
				}
			}
		}
		expect(played).to_be_greater_than(0);
	});

	it("let sounding notes ring: no note-off at the drop-out's start, and none of its notes start", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kBassTwoBars});
		const Result result = play(dropRun(kGenre), 8);
		const std::vector<Held> bass = heldNotes(notesOf(result), kBassPart);
		// Drawn on bar 0's line, the drop-out plays bars 1 and 2. The Bass's last-beat note rings its length into it.
		const Held* last = noteAt(bass, stepTick(12));
		expect(last != nullptr && last->off == stepTick(12 + kBassSteps)).to_be_true();
		expect(ons(result, kBassPart, Lane::None, 16, 48).empty()).to_be_true();
		// Back on bar 3's line, and the other parts play on.
		expect(ons(result, kBassPart, Lane::None, 48, 49).size()).to_equal(size_t{1});
		expect(ons(result, kDrumsPart, Lane::Kick, 16, 48).size()).to_equal(size_t{8});
	});

	it("end a dropped Sustain's notes on its first bar line, and strike them again on the line it ends", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kPadOneBar});
		const Result result = play(dropRun(kGenre), 4);
		const std::vector<Held> pad = heldNotes(notesOf(result), kPadPart);
		const auto onAt = [&](int32_t step) {
			return std::count_if(pad.begin(), pad.end(), [&](const Held& n) { return n.on == stepTick(step); });
		};
		expect(onAt(0)).to_be_greater_than(0);
		for (const Held& note : pad) {
			if (note.on == stepTick(0)) {
				expect(note.off).to_equal(stepTick(16)); // a Sustain note lasts to the next bar line
			}
		}
		expect(ons(result, kPadPart, Lane::None, 16, 32).empty()).to_be_true();
		expect(onAt(32)).to_equal(onAt(0)); // the whole voicing again, as after an unmute
	});

	context("Throw", _ {
		it("spikes the target's last-beat hit, holds while it is silent and falls over the last beat", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kBassTwoBars});
			const Result result = play(dropRun(kGenre), 8);
			const auto space = [&](int32_t step) {
				return offsetAt(result, kBassPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(space(11)).to_equal(0);
			expect(space(12)).to_equal(kThrowSpace); // its hit on bar 0's last beat
			expect(space(16)).to_equal(kThrowSpace);
			expect(space(44)).to_equal(kThrowSpace); // past a bar's hold: the drop-out holds it
			expect(space(45)).to_equal(kThrowSpace * 3 / 4);
			expect(space(46)).to_equal(kThrowSpace / 2);
			expect(space(47)).to_equal(kThrowSpace / 4);
			expect(space(48)).to_equal(0); // dry as it comes back
		});

		it("holds the delay Throw of a target with a delay setup the same way, and lets it fall over the last beat", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kBassTwoBars, .parts = kDelayParts});
			const Result result = play(dropRun(kGenre), 8);
			const auto delay = [&](int32_t step) {
				const Event* e = lastAt(result, kBassPart, Lane::None, SoundControl::Space, stepTick(step));
				return e != nullptr ? e->throwDelay() : 0;
			};
			expect(delay(11)).to_equal(0);
			expect(delay(12)).to_equal(100);
			expect(delay(44)).to_equal(100); // past a bar's hold: the drop-out holds it
			expect(delay(45)).to_equal(75);
			expect(delay(46)).to_equal(50);
			expect(delay(47)).to_equal(25);
			expect(delay(48)).to_equal(0); // dry as it comes back
		});

		it("throws a Drums lane on the row its Throw aims at", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar});
			const Result result = play(dropRun(kGenre), 4);
			const auto space = [&](Lane lane, int32_t step) {
				return offsetAt(result, kDrumsPart, lane, SoundControl::Space, stepTick(step));
			};
			expect(space(Lane::Snare, 12)).to_equal(kThrowSpace);
			// The hats and the kick hitting after it in the window are other rows: the Snare's holds.
			expect(ons(result, kDrumsPart, Lane::ClosedHat, 14, 15).size()).to_equal(size_t{1});
			expect(space(Lane::Snare, 14)).to_equal(kThrowSpace);
			expect(space(Lane::Snare, 28)).to_equal(kThrowSpace);
			expect(space(Lane::Snare, 32)).to_equal(0);
			expect(space(Lane::None, 20)).to_equal(0); // the kit-wide set stays home: the kick plays on
			expect(ons(result, kDrumsPart, Lane::Snare, 16, 32).empty()).to_be_true();
			expect(ons(result, kDrumsPart, Lane::Kick, 16, 32).size()).to_equal(size_t{4});
		});

		it("holds a kit-wide Throw over the drop-out while the kit's other lanes play, in the window and after", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar, .parts = kKitWideParts});
			const Result result = play(dropRun(kGenre), 4);
			const auto kit = [&](int32_t step) {
				return offsetAt(result, kDrumsPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			// The Snare's hit on step 12 spikes the kit; the hat on 14 and the kicks in the drop-out leave it held.
			expect(ons(result, kDrumsPart, Lane::ClosedHat, 14, 15).size()).to_equal(size_t{1});
			expect(kit(11)).to_equal(0);
			expect(kit(12)).to_equal(kThrowSpace);
			expect(kit(14)).to_equal(kThrowSpace);
			expect(kit(20)).to_equal(kThrowSpace);
			expect(kit(28)).to_equal(kThrowSpace);
			expect(kit(29)).to_equal(kThrowSpace * 3 / 4);
			expect(kit(32)).to_equal(0);
		});

		it("gives way to a Break's Throw on the kit-wide set when the drop-out ends on the Break's line", _ {
			// The Snare and hats drop out each other bar, the last in the bar before the Break. Its Throw is kit-wide
			// and the Break keeps no Drums lane, so the kick's hits in that last beat spike the kit for the Break, and
			// it holds into the Break as any Break Throw does, instead of falling dry with the drop-out.
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar,
			                                           .groove = {1, 1},
			                                           .parts = kKitWideParts,
			                                           .breakKeeps = {.parts = kBass.parts | kPad.parts}});
			const Result result = play(dropRun(kGenre), 10);
			expect(sectionStart(result, SectionKind::Break)).to_equal(stepTick(kPhraseSteps));
			const auto kit = [&](int32_t step) {
				return offsetAt(result, kDrumsPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(ons(result, kDrumsPart, Lane::Kick, 124, 125).size()).to_equal(size_t{1});
			expect(kit(124)).to_equal(kThrowSpace);
			// A bar's hold from the kick's last hit at 124, Wash easing in under it.
			expect(kit(kPhraseSteps + 2)).to_equal(kThrowSpace);
			expect(kit(kPhraseSteps + 10)).to_equal(kThrowSpace);
		});

		it("takes a kit-wide Throw back from a Break's spike when the Break goes away before its line", _ {
			// As above, but Motion turns to 0 on step 125, so the Groove no longer ends into the Break on 128. The
			// kick's spike on 124 was for the Break; with it gone, the drop-out's fall over its last beat resumes.
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar,
			                                           .groove = {1, 1},
			                                           .parts = kKitWideParts,
			                                           .breakKeeps = {.parts = kBass.parts | kPad.parts}});
			Run run = dropRun(kGenre);
			run.steer = at(125, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Motion), 0});
			const Result result = play(run, 10);
			expect(sectionStart(result, SectionKind::Break)).to_equal(-1);
			const auto kit = [&](int32_t step) {
				return offsetAt(result, kDrumsPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(kit(124)).to_equal(kThrowSpace);
			expect(kit(125)).to_equal(kThrowSpace * 3 / 4);
			expect(kit(127)).to_equal(kThrowSpace / 4);
			expect(kit(kPhraseSteps)).to_equal(0);
		});

		it("takes a kit-wide Throw back from a Break's spike when a waiting force for the Break is cancelled", _ {
			// As above, but the Groove runs two phrases, so the Break on 128 is a force pressed in bar 6. The kick's
			// spike on 124 was for it; pressed again on 125, the force is cancelled and the drop-out's fall resumes.
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar,
			                                           .groove = {2, 2},
			                                           .parts = kKitWideParts,
			                                           .breakKeeps = {.parts = kBass.parts | kPad.parts}});
			Run forced = dropRun(kGenre);
			forced.steer = at(100, force(SectionKind::Break));
			expect(sectionStart(play(forced, 10), SectionKind::Break)).to_equal(stepTick(kPhraseSteps));
			Run run = dropRun(kGenre);
			run.steer = steerAt({{100, force(SectionKind::Break)}, {125, force(SectionKind::Break)}});
			const Result result = play(run, 10);
			expect(sectionStart(result, SectionKind::Break)).to_equal(-1);
			const auto kit = [&](int32_t step) {
				return offsetAt(result, kDrumsPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(kit(124)).to_equal(kThrowSpace);
			expect(kit(125)).to_equal(kThrowSpace * 3 / 4);
			expect(kit(127)).to_equal(kThrowSpace / 4);
			expect(kit(kPhraseSteps)).to_equal(0);
		});

		it("plays no Throw when the target has no hit in the bar's last beat", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kPadOneBar}); // a Pad strikes on bar lines only
			const Result result = play(dropRun(kGenre), 6);
			for (const Event& e : controls(result)) {
				if (e.part == kPadPart && e.control == SoundControl::Space) {
					expect(e.offset).to_equal(0);
				}
			}
		});
	});

	context("what ends one early", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kBassFourBars, .build = kBuild4});

		it("a forced Build that starts mid-phrase ends one playing on its line, and the Throw falls from there", _ {
			// Drawn on bar 0's line for bars 1-4; the force, pressed in bar 1, starts a 4-bar Build on bar 4.
			const auto steer = [](int32_t s, StepwisePiece<>& piece) {
				if (s == 20) {
					piece.push(force(SectionKind::Build));
				}
			};
			const Walk walk = walkPiece(startOf(kGenre), 6 * kStepsPerBar, kBassPart, steer);
			expect(walk.section[64] == SectionKind::Build).to_be_true();
			expect(walk.spans == std::vector<Span>{{16, 64}}).to_be_true();

			Run run = dropRun(kGenre);
			run.steer = at(20, force(SectionKind::Build));
			const Result result = play(run, 6);
			const auto space = [&](int32_t step) {
				return offsetAt(result, kBassPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(ons(result, kBassPart, Lane::None, 64, 65).size()).to_equal(size_t{1}); // back on the line
			expect(space(63)).to_equal(kThrowSpace);
			expect(space(64)).to_equal(kThrowSpace * 3 / 4);
			expect(space(66)).to_equal(kThrowSpace / 4);
			expect(space(67)).to_equal(0);
		});

		it("one drawn to start on a Section line doesn't play, and its spiked target comes home on its next hit", _ {
			// One bar dropped every other bar: drawn on bar 6's line for bar 7, where a 1-bar Build forced after the
			// draw starts.
			static constexpr WeightedBars kBuild1[] = {{1, 1}};
			static constexpr Genre kShort = dropGenre({.dropOuts = kBassOneBar, .build = kBuild1});
			const auto steer = [](int32_t s, StepwisePiece<>& piece) {
				if (s == 98) {
					piece.push(force(SectionKind::Build));
				}
			};
			const Walk walk = walkPiece(startOf(kShort), kPhraseSteps, kBassPart, steer);
			expect(walk.section[112] == SectionKind::Build).to_be_true();
			expect(walk.spans == std::vector<Span>{{16, 32}, {48, 64}, {80, 96}}).to_be_true();

			Run run = dropRun(kShort);
			run.steer = at(98, force(SectionKind::Build));
			const Result result = play(run, 9);
			const auto space = [&](int32_t step) {
				return offsetAt(result, kBassPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(space(108)).to_equal(kThrowSpace);
			expect(ons(result, kBassPart, Lane::None, 112, 113).size()).to_equal(size_t{1});
			expect(space(112)).to_equal(0);
		});

		it("End's Outro cancels one drawn to start on its line, and the spiked target comes home on its next hit", _ {
			// Two bars dropped at bars 1-2 and 4-5; drawn on bar 7's line for the next phrase's first bar, where End,
			// pressed after the draw, starts the Outro.
			static constexpr Genre kGenre = dropGenre({.dropOuts = kBassTwoBars});
			const auto steer = [](int32_t s, StepwisePiece<>& piece) {
				if (s == 114) {
					piece.push({CommandKind::End, 0, 1});
				}
			};
			const Walk walk = walkPiece(startOf(kGenre), kPhraseSteps + 3 * kStepsPerBar, kBassPart, steer);
			expect(walk.section[kPhraseSteps] == SectionKind::Outro).to_be_true();
			expect(walk.spans == std::vector<Span>{{16, 48}, {64, 96}}).to_be_true();

			Run run = dropRun(kGenre);
			run.steer = at(114, {CommandKind::End, 0, 1});
			const Result result = play(run, 10);
			const auto space = [&](int32_t step) {
				return offsetAt(result, kBassPart, Lane::None, SoundControl::Space, stepTick(step));
			};
			expect(space(124)).to_equal(kThrowSpace);
			expect(ons(result, kBassPart, Lane::None, kPhraseSteps, kPhraseSteps + 1).size()).to_equal(size_t{1});
			expect(space(kPhraseSteps)).to_be_less_than(kThrowSpace / 10); // home, with only the Outro's Wash easing in
		});

		it("a stop clears it, and the next Start begins with none", _ {
			StepwisePiece<> piece{startOf(kGenre)};
			piece.decideBefore(20); // the drop-out plays bars 1-4
			expect(piece.generator.partSilenced(kBassPart)).to_be_true();
			piece.restart();
			piece.decideBefore(2);
			expect(piece.generator.partSilenced(kBassPart)).to_be_false();
			piece.decideBefore(17);
			expect(piece.generator.partSilenced(kBassPart)).to_be_true(); // drawn afresh on the new piece's bar 0
		});
	});

	it("start afresh after a stop with the gap set: a stale gap doesn't hold the next piece's first one back", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kBassOneBar, .gapBars = 4});
		StepwisePiece<> piece{startOf(kGenre)};
		piece.decideBefore(40); // the first played bar 1, so the next may start from bar 6
		piece.decideBefore(81);
		expect(piece.generator.partSilenced(kBassPart)).to_be_false();
		piece.restart();
		piece.decideBefore(17);
		expect(piece.generator.partSilenced(kBassPart)).to_be_true();
	});

	context("against fills", _ {
		it("wins on a lane they share: a Swap there doesn't play; the fill still plays its other lanes", _ {
			// One bar dropped every other bar, so the phrase's last bar is one; the snare run Swaps Snare and Tom.
			static constexpr Genre kGenre =
			    dropGenre({.dropOuts = kBassAndSnareOneBar, .fills = kSnareRun, .fillChance = 1000});
			const Result result = play(dropRun(kGenre), 9);
			const int32_t end = kPhraseSteps;
			expect(ons(result, kDrumsPart, Lane::Snare, end - kStepsPerBar, end).empty()).to_be_true();
			expect(ons(result, kDrumsPart, Lane::Tom, end - kStepsPerBeat, end).size()).to_equal(size_t{2});
			// The Phrase accent after the fill still plays.
			expect(ons(result, kDrumsPart, Lane::Crash, end, end + 1).size()).to_equal(size_t{1});
		});

		it("wins over a Rest on a part they share, which then sends it no note-offs; the Rest still drops the kick", _ {
			static constexpr DropOutEntry kBassLast[] = {{.mask = kBass, .bars = 1}};
			// A bar's kick drop over the Bass, starting with the drop-out on the phrase's last bar.
			static constexpr FillEntry kKickDrop[] = {
			    {.kind = FillKind::Rest, .mask = {.parts = kBass.parts, .lanes = laneBit(Lane::Kick)}, .beats = 4}};
			static constexpr Genre kGenre = dropGenre({.dropOuts = kBassLast, .fills = kKickDrop, .fillChance = 1000});
			const Result result = play(dropRun(kGenre), 9);
			const int32_t end = kPhraseSteps;
			expect(ons(result, kDrumsPart, Lane::Kick, end - kStepsPerBar, end).empty()).to_be_true();
			expect(ons(result, kBassPart, Lane::None, end - kStepsPerBar, end).empty()).to_be_true();
			// The Bass's note before the drop-out rings its length, not cut by the Rest.
			const std::vector<Held> bass = heldNotes(notesOf(result), kBassPart);
			const Held* last = noteAt(bass, stepTick(end - kStepsPerBar - 4));
			expect(last != nullptr && last->off == stepTick(end - kStepsPerBar - 4 + kBassSteps)).to_be_true();
		});
	});

	it("dim a dropped part's MUTE pad, and a Kit's only if every lane is dropped", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar});
		StepwisePiece<> piece{startOf(kGenre)};
		piece.decideBefore(21);
		expect(piece.generator.partSilenced(kDrumsPart)).to_be_false();
		static constexpr DropOutEntry kWholeKit[] = {
		    {.mask = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::Snare) | laneBit(Lane::ClosedHat)
		                       | laneBit(Lane::Crash) | laneBit(Lane::Tom)},
		     .bars = 1}};
		static constexpr Genre kKitGenre = dropGenre({.dropOuts = kWholeKit});
		StepwisePiece<> kit{startOf(kKitGenre)};
		kit.decideBefore(21);
		expect(kit.generator.partSilenced(kDrumsPart)).to_be_true();
		kit.decideBefore(33);
		expect(kit.generator.partSilenced(kDrumsPart)).to_be_false();
		static constexpr Genre kBassGenre = dropGenre({.dropOuts = kBassOneBar});
		StepwisePiece<> bass{startOf(kBassGenre)};
		bass.decideBefore(21);
		expect(bass.generator.partSilenced(kBassPart)).to_be_true();
		bass.decideBefore(33);
		expect(bass.generator.partSilenced(kBassPart)).to_be_false();
	});

	it("let Loop and Motif lines advance, pass and mutate while silent, as if they had played", _ {
		static constexpr DropOutEntry kBoth[] = {{.mask = {.parts = kBass.parts | partBit(kLinePart)}, .bars = 2}};
		static constexpr Genre kWith = dropGenre({.dropOuts = kBoth, .parts = kLineParts});
		static constexpr Genre kWithout = dropGenre({.dropOuts = {}, .parts = kLineParts});
		const Result with = play(dropRun(kWith), 16);
		const Result without = play(dropRun(kWithout), 16);
		const Walk walk = walkPiece(startOf(kWith), 16 * kStepsPerBar, kBassPart);
		expect(walk.spans.size()).to_be_greater_than(size_t{2});
		const auto dropped = [&](int32_t tick) {
			return std::ranges::any_of(walk.spans, [&](const Span& span) {
				return tick >= stepTick(span.from) && tick < stepTick(span.to);
			});
		};
		for (uint8_t part : {kBassPart, kLinePart}) {
			std::vector<std::pair<int32_t, uint8_t>> heard;
			std::vector<std::pair<int32_t, uint8_t>> expected;
			for (const Event& e : ons(with, part, Lane::None, 0, 16 * kStepsPerBar)) {
				heard.emplace_back(e.tick, e.pitch);
			}
			for (const Event& e : ons(without, part, Lane::None, 0, 16 * kStepsPerBar)) {
				if (!dropped(e.tick)) {
					expected.emplace_back(e.tick, e.pitch);
				}
			}
			expect(heard.size()).to_be_greater_than(size_t{20});
			expect(heard == expected).to_be_true();
		}
	});

	it("leave the Arc plan as it is without them", _ {
		static constexpr Genre kWith = dropGenre({.dropOuts = kMixed});
		static constexpr Genre kWithout = dropGenre({.dropOuts = {}});
		StepwisePiece<> with{startOf(kWith)};
		StepwisePiece<> without{startOf(kWithout)};
		for (int32_t step : {1, 20, 40, 100, 200}) {
			with.decideBefore(step);
			without.decideBefore(step);
			ArcPlan a;
			ArcPlan b;
			with.generator.planArc(a);
			without.generator.planArc(b);
			expect(a.count).to_equal(b.count);
			for (int32_t i = 0; i < a.count; i++) {
				expect(a.phrase[i].sounds).to_equal(b.phrase[i].sounds);
				expect(a.phrase[i].kind == b.phrase[i].kind).to_be_true();
			}
		}
	});

	it("don't move with a take or a freeze, and drop a frozen part too", _ {
		static constexpr Genre kGenre = dropGenre({.dropOuts = kMixed, .chance = 300, .groove = {4, 4}});
		const Walk plain = walkPiece(startOf(kGenre, 5), 4 * kPhraseSteps, kBassPart);
		Start taken = startOf(kGenre, 5);
		taken.takes.take[kBassPart] = 3;
		taken.takes.take[kDrumsPart] = 2;
		const Walk withTakes = walkPiece(taken, 4 * kPhraseSteps, kBassPart);
		const auto freeze = [](int32_t s, StepwisePiece<>& p) {
			if (s == 3) {
				p.push({CommandKind::SetFreeze, kBassPart, kPress});
			}
		};
		const Walk frozen = walkPiece(startOf(kGenre, 5), 4 * kPhraseSteps, kBassPart, freeze);
		expect(plain.spans.size()).to_be_greater_than(size_t{0});
		expect(withTakes.spans == plain.spans).to_be_true();
		expect(frozen.spans == plain.spans).to_be_true();
	});

	context("FX lanes in drop-outs (spec §8.8)", _ {
		static constexpr PartSpec kSirenParts[] = {
		    kParts[0], kParts[1], kParts[2], {.role = Role::FX, .name = "FX", .lanes = kFxWithSiren, .drift = kNoDrift}};
		static constexpr uint8_t kFxPart = 3;
		static constexpr uint16_t kAllFx =
		    laneBit(Lane::Riser) | laneBit(Lane::Fall) | laneBit(Lane::Impact) | laneBit(Lane::Siren);
		static constexpr DropOutEntry kPadAndFxTwoBars[] = {
		    {.mask = {.parts = kPad.parts, .lanes = kAllFx}, .bars = 2}};
		static constexpr DropOutEntry kPadTwoBars[] = {{.mask = kPad, .bars = 2}};
		static constexpr WeightedBars kBuild1[] = {{1, 1}};

		it("end a held Siren on the drop-out's first bar line, fire its release Throw, and dim FX", _ {
			static constexpr Genre kWithFx = dropGenre(
			    {.dropOuts = kPadAndFxTwoBars, .build = kBuild1, .grooveToBuild = 1000, .groove = {1, 1}, .parts = kSirenParts});
			static constexpr Genre kWithoutFx = dropGenre(
			    {.dropOuts = kPadTwoBars, .build = kBuild1, .grooveToBuild = 1000, .groove = {1, 1}, .parts = kSirenParts});
			expect(dropOutsValid(kWithFx)).to_be_true();
			// Build ends into a Drop at bar 8 (phrase 1): Siren starts its 2-bar note at bar 8, drop-out starts at bar 9.
			const auto check = [](const Genre& genre) {
				const Result result = play(dropRun(genre), 16);
				std::vector<Event> sirenEvents;
				for (const Event& e : result.events) {
					if (e.part == kFxPart && e.lane == Lane::Siren
					    && (e.kind == EventKind::NoteOn || e.kind == EventKind::NoteOff)) {
						sirenEvents.push_back(e);
					}
				}
				const std::vector<Held> fx = heldNotes(sirenEvents, kFxPart);
				int32_t dropBar = -1;
				for (const Event& e : only(result.events, EventKind::Section)) {
					if (e.section == SectionKind::Groove && e.occurrence == 1) {
						dropBar = e.tick / kBarTicks;
					}
				}
				return std::pair{dropBar, fx};
			};
			const auto [dropWith, fxWith] = check(kWithFx);
			const auto [dropWithout, fxWithout] = check(kWithoutFx);
			expect(dropWith).to_be_greater_than(0);
			expect(dropWith).to_equal(dropWithout);
			const Held* sirenCut = nullptr;
			for (const Held& n : fxWith) {
				if (n.on == dropWith * kBarTicks) {
					sirenCut = &n;
				}
			}
			const Held* sirenFull = nullptr;
			for (const Held& n : fxWithout) {
				if (n.on == dropWithout * kBarTicks) {
					sirenFull = &n;
				}
			}
			expect(sirenCut != nullptr).to_be_true();
			expect(sirenFull != nullptr).to_be_true();
			if (sirenCut == nullptr || sirenFull == nullptr) {
				return;
			}
			// Unmasked: Siren rings its full 2 bars. Masked: cut on bar 1 of the Drop when the drop-out begins.
			expect(sirenFull->off - sirenFull->on).to_equal(2 * kBarTicks);
			expect(sirenCut->off - sirenCut->on).to_equal(kBarTicks);
			// Cutting the Siren on bar 1 triggers its release Throw there.
			const Result result = play(dropRun(kWithFx), 16);
			const int32_t cutStep = (dropWith + 1) * kStepsPerBar;
			expect(offsetAt(result, kFxPart, Lane::Siren, SoundControl::Space, stepTick(cutStep - 1))).to_equal(0);
			expect(offsetAt(result, kFxPart, Lane::Siren, SoundControl::Space, stepTick(cutStep))).to_equal(kThrowSpace);
			// FX MUTE pad dims during the drop-out.
			const Walk fxWalk = walkPiece(startOf(kWithFx), 16 * kStepsPerBar, kFxPart);
			expect(static_cast<bool>(fxWalk.silenced[cutStep])).to_be_true();
			expect(static_cast<bool>(fxWalk.silenced[cutStep + 2 * kStepsPerBar])).to_be_false();
		});
	});
});

CPPSPEC_SPEC(drop_outs);
