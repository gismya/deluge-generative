// Drop-outs in the event stream (spec §8.8): when they play, how sounding notes ring and a Sustain comes back, the
// Throw on the target held over them (§11.4), what ends one early, their precedence over fills (§8.7), the dim and the
// Arc plan (§13.5), takes and freeze, and genreValid()'s check. Played on the spec's own genre: no built-in genre has
// drop-out entries yet (Dub, #131, is the first).

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
constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr uint8_t kPadPart = 2;
constexpr Scale kScales[] = {Scale::Minor};

constexpr PartsAndLanes kBass{.parts = 1U << kBassPart};
constexpr PartsAndLanes kPad{.parts = 1U << kPadPart};
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
};

/// An Intro of a phrase, a Groove of `shape.groove` phrases at Motion 25, a Break and a Build. Flat macros, no macro
/// layer and no drift, so only drop-outs, fills and gestures change what plays and the Space sent.
constexpr Genre dropGenre(const Shape& shape) {
	return {
	    .name = "drop-outs",
	    .facts = {.scales = kScales},
	    .soundWeight = {},
	    .parts = kParts,
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
	            .breakKeeps = kEverything,
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
		static constexpr DropOutEntry kLongOrShort[] = {{.mask = kBass, .bars = 4, .weight = 3}, {.mask = kBass, .bars = 1}};
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

		it("throws a Drums lane on the row its Throw aims at", _ {
			static constexpr Genre kGenre = dropGenre({.dropOuts = kSnareAndHatsOneBar});
			const Result result = play(dropRun(kGenre), 4);
			const auto space = [&](Lane lane, int32_t step) {
				return offsetAt(result, kDrumsPart, lane, SoundControl::Space, stepTick(step));
			};
			expect(space(Lane::Snare, 12)).to_equal(kThrowSpace);
			expect(space(Lane::Snare, 28)).to_equal(kThrowSpace);
			expect(space(Lane::Snare, 32)).to_equal(0);
			expect(space(Lane::None, 20)).to_equal(0); // the kit-wide set stays home: the kick plays on
			expect(ons(result, kDrumsPart, Lane::Snare, 16, 32).empty()).to_be_true();
			expect(ons(result, kDrumsPart, Lane::Kick, 16, 32).size()).to_equal(size_t{4});
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
			const Walk walk = walkPiece(startOf(kGenre), 6 * kStepsPerBar, kBassPart, [](int32_t s, StepwisePiece<>& piece) {
				if (s == 20) {
					piece.push(force(SectionKind::Build));
				}
			});
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
			// One bar dropped every other bar: drawn on bar 6's line for bar 7, where a 1-bar Build forced after the draw
			// starts.
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
		static constexpr Genre kBassGenre = dropGenre({.dropOuts = kBassOneBar});
		StepwisePiece<> bass{startOf(kBassGenre)};
		bass.decideBefore(21);
		expect(bass.generator.partSilenced(kBassPart)).to_be_true();
		bass.decideBefore(33);
		expect(bass.generator.partSilenced(kBassPart)).to_be_false();
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
		const Walk frozen = walkPiece(startOf(kGenre, 5), 4 * kPhraseSteps, kBassPart, [](int32_t s, StepwisePiece<>& p) {
			if (s == 3) {
				p.push({CommandKind::SetFreeze, kBassPart, kPress});
			}
		});
		expect(plain.spans.size()).to_be_greater_than(size_t{0});
		expect(withTakes.spans == plain.spans).to_be_true();
		expect(frozen.spans == plain.spans).to_be_true();
	});

	context("genreValid()", _ {
		it("refuses an entry that silences every pitched part and every Drums lane with a pool", _ {
			static constexpr DropOutEntry kEverythingOut[] = {
			    {.mask = {.parts = kBass.parts | kPad.parts,
			              .lanes = laneBit(Lane::Kick) | laneBit(Lane::Snare) | laneBit(Lane::ClosedHat)}}};
			static constexpr DropOutEntry kAllButHats[] = {
			    {.mask = {.parts = kBass.parts | kPad.parts, .lanes = laneBit(Lane::Kick) | laneBit(Lane::Snare)}}};
			// The pool-less lanes don't count: dropping them too still silences everything.
			static constexpr DropOutEntry kWithPoolless[] = {
			    {.mask = {.parts = kBass.parts | kPad.parts,
			              .lanes = laneBit(Lane::Kick) | laneBit(Lane::Snare) | laneBit(Lane::ClosedHat)
			                       | laneBit(Lane::Tom)}}};
			expect(dropOutsValid(dropGenre({.dropOuts = kEverythingOut}))).to_be_false();
			expect(dropOutsValid(dropGenre({.dropOuts = kWithPoolless}))).to_be_false();
			expect(dropOutsValid(dropGenre({.dropOuts = kAllButHats}))).to_be_true();
			// genreValid() holds it: Acid techno, with an entry over its Bass and Drone and every Drums lane with a pool.
			const Genre& acid = genres::technoAcid;
			DropOutEntry all{.mask = {.parts = 0b110}};
			for (const LaneSpec& lane : acid.parts[0].lanes) {
				all.mask.lanes |= lane.pool.empty() ? 0 : laneBit(lane.lane);
			}
			const DropOutEntry allEntries[] = {all};
			Genre everything = acid;
			everything.dropOuts = allEntries;
			expect(genreValid(acid)).to_be_true();
			expect(genreValid(everything)).to_be_false();
			const DropOutEntry bassEntries[] = {{.mask = {.parts = 0b010}, .bars = 2, .throwTarget = {.parts = 0b010}}};
			Genre bassOut = acid;
			bassOut.dropOuts = bassEntries;
			expect(genreValid(bassOut)).to_be_true();
		});

		it("refuses a length but 1, 2 or 4 bars, an FX lane, a whole Kit, a target outside the mask, and no gap", _ {
			static constexpr DropOutEntry kThreeBars[] = {{.mask = kBass, .bars = 3}};
			static constexpr DropOutEntry kFxLane[] = {{.mask = {.lanes = laneBit(Lane::Riser)}}};
			static constexpr DropOutEntry kWholeKit[] = {{.mask = {.parts = 1U << kDrumsPart}}};
			static constexpr DropOutEntry kTargetOutside[] = {{.mask = kBass, .throwTarget = kPad}};
			static constexpr DropOutEntry kTwoTargets[] = {
			    {.mask = {.parts = kBass.parts | kPad.parts}, .throwTarget = {.parts = kBass.parts | kPad.parts}}};
			for (std::span<const DropOutEntry> entries :
			     {std::span<const DropOutEntry>(kThreeBars), std::span<const DropOutEntry>(kFxLane),
			      std::span<const DropOutEntry>(kWholeKit), std::span<const DropOutEntry>(kTargetOutside),
			      std::span<const DropOutEntry>(kTwoTargets)}) {
				expect(dropOutsValid(dropGenre({.dropOuts = entries}))).to_be_false();
			}
			expect(dropOutsValid(dropGenre({.dropOuts = kBassOneBar, .gapBars = 0}))).to_be_false();
			expect(dropOutsValid(dropGenre({.dropOuts = kBassOneBar, .chance = 1001}))).to_be_false();
		});

		it("holds every built-in genre", _ {
			for (const Genre* genre : allGenres()) {
				expect(dropOutsValid(*genre)).to_be_true();
			}
		});
	});
});

CPPSPEC_SPEC(drop_outs);
