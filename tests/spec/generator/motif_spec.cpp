// The Motif flavour (spec §5.2, §6.6, §8.1): a line of slots storing chord positions, not pitches, rolled at each pick
// and take change and re-rolled slot by slot under the Motif curve, so a remembered phrase follows each chord change.
// No genre plays a Motif yet (Detroit techno will, spec §4.5): kTechnoMotif stands in.

#include "chord_moves.h"
#include "core_walk.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kNone = -1;
constexpr int32_t kStep = kTicksPerStep;
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// 2-bar phrases, no re-picks and no Arc offsets, so the Recipe's macros are what plays and only steering changes the
/// pick. runOf() starts in a Groove that outlasts every run. An empty Harmony slot: the home chord throughout, a plain
/// triad.
constexpr Genre motifGenre(std::span<const PartSpec> parts) {
	return {
	    .name = "motif",
	    .facts = {.scales = kScales},
	    .phraseBars = 2,
	    .parts = parts,
	    .repickChance = 0,
	    .harmony = {.extensionCeiling = 0},
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {64, 64},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuildBars,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	        },
	};
}

/// A minor's tonic at A3 (57), the span from E♭3 (51) to C♯6 (85).
constexpr Register kMotifRegister{.anchor = 57, .spanLow = -6, .spanHigh = 28};

/// Six chord positions, evenly weighted: the triad, then the same an octave up.
constexpr PositionWeight kSix[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}};
/// A minor's i (A C E) at each position, from A3, and iv (D F A) from D4.
constexpr int32_t kOnI[] = {57, 60, 64, 69, 72, 76};
constexpr int32_t kOnIV[] = {62, 65, 69, 74, 77, 81};

constexpr Pattern kEvery16th[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
/// Two bars of 16ths: a riff that spans a chord change.
constexpr Pattern kTwoBars[] = {{"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", "................................", 0}};
/// Anchors on the beats, and a note on every other step that always plays at Density 50.
constexpr Pattern kBusy[] = {{"X9.9X9.9X9.9X9.9", "................", 0}};
/// Anchors on each beat, and 16ths Density thins in between.
constexpr Pattern kThinned[] = {{"X375X573X735X357", "................", 0}};
/// Onsets on the 1, four 16ths on, two on, then the bar's last 16th: uneven gaps.
constexpr Pattern kGaps[] = {{"X...X.X........X", "................", 0}};

constexpr MotifSpec kPlain{.positions = kSix, .leap = 5, .octaveUp = 0, .accent = 0, .reroll = 333};

template <const Pattern* Pool, size_t N = 1>
struct OneMotif {
	static constexpr PartSpec parts[] = {{.role = Role::Lead,
	                                      .flavour = Flavour::Motif,
	                                      .name = "MOTIF",
	                                      .pool = {Pool, N},
	                                      .reg = kMotifRegister,
	                                      .motif = kPlain}};
	static constexpr Genre genre = motifGenre(parts);
};

/// A chord a bar, i and iv in turn.
constexpr ChordMove kTurns[] = {{kI, kIV, 100}, {kIV, kI, 100}};
constexpr ChordGraph kTurnGraph{.moves = kTurns, .barsPerChord = 1};
constexpr Genre kTurningGenre = [] {
	Genre genre = OneMotif<kTwoBars>::genre;
	genre.harmony.graph = &kTurnGraph;
	return genre;
}();

/// Complexity 0 keeps to the root and the 3rd, in place; Complexity 50 reaches every position and jumps octaves.
constexpr PartSpec kShapedParts[] = {{.role = Role::Lead,
                                      .flavour = Flavour::Motif,
                                      .name = "MOTIF",
                                      .pool = kBusy,
                                      .reg = kMotifRegister,
                                      .motif = {.positions = kSix, .leap = {1, 5}, .octaveUp = {0, 500}}}};
constexpr Genre kShapedGenre = motifGenre(kShapedParts);

Run runOf(const Genre& genre, uint32_t seed, int32_t motion, int32_t complexity = 25, int32_t density = 50) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.keyRoot = 9; // A
	run.recipe.scale = Scale::Minor;
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Motion, macroPoints(motion));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.seed = seed;
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

using Bar = std::array<int32_t, kStepsPerBar>;

/// For each bar, the pitch the Motif started on each step, or kNone.
std::vector<Bar> barsOf(const Result& result, int32_t count) {
	Bar silent;
	silent.fill(kNone);
	std::vector<Bar> out(static_cast<size_t>(count), silent);
	for (const Event& e : pitched(result, EventKind::NoteOn)) {
		const int32_t step = stepOf(e);
		if (step / kStepsPerBar < count) {
			out[step / kStepsPerBar][step % kStepsPerBar] = e.pitch;
		}
	}
	return out;
}

/// The position each pitch of A minor's i stands for.
int32_t positionOnI(int32_t pitch) {
	const auto* at = std::find(std::begin(kOnI), std::end(kOnI), pitch);
	return at == std::end(kOnI) ? kNone : static_cast<int32_t>(at - std::begin(kOnI));
}

/// Whether a genre of one Lead Motif part over `pool`, with `motif` and `expression`, passes motifNotesValid().
bool motifValid(std::span<const Pattern> pool, const MotifSpec& motif, const Expression& expression = {}) {
	const PartSpec parts[] = {{.role = Role::Lead,
	                           .flavour = Flavour::Motif,
	                           .pool = pool,
	                           .reg = kMotifRegister,
	                           .motif = motif,
	                           .expression = expression}};
	return motifNotesValid(Genre{.name = "motif", .parts = parts});
}

/// Techno's Drums, Bass and FX, and a tiered Motif lead over Melodic techno's Chord graph with up to two Extensions:
/// one- and two-bar riffs, Lean on the a, ratchets and flams, as Detroit techno's lead will be (spec §4.5).
constexpr Pattern kRiffs[] = {
    {"X..5..X...7.X...", "................", 0},
    {"X..5..X..3..X.7.X..5..X...7..9..", "................................", 1},
    {"X.5.7.X..5.3X.7.X..5..X.7.5..9.3", "................................", 2},
};
constexpr PositionWeight kRiffPositions[] = {{-1, 1}, {0, 3}, {1, 2}, {2, 2}, {3, 2}, {4, 1}};
constexpr Expression kRiffExpression{
    .lean = {0, 4, 0, 10},
    .spread = 6,
    .ornaments = {.rateFloor = 100, .rateCeiling = 400, .ratchet = {1, 0, 0}, .flam = 1, .flamGap = 4}};
const PartSpec kTechnoMotifParts[] = {
    genres::techno.parts[0],
    genres::techno.parts[1],
    {.role = Role::Lead,
     .flavour = Flavour::Motif,
     .name = "MOTIF",
     .pool = kRiffs,
     .band = {0, 2},
     .motif = {.positions = kRiffPositions, .leap = {2, 4}, .octaveUp = {0, 200}, .gateTicks = 72},
     .expression = kRiffExpression},
    genres::techno.parts[2],
};
const Genre kTechnoMotif = [] {
	Genre genre = genres::technoMelodic;
	genre.name = "techno-motif";
	genre.parts = kTechnoMotifParts;
	genre.harmony.extensionCeiling = 2;
	return genre;
}();
constexpr uint8_t kMotifPart = 2;

} // namespace

// clang-format off
describe motif("The Motif flavour", $ {
	context("chord positions (spec §5.2, §7.4)", _ {
		it("plays each slot's chord position on the current chord, from the root's place in the Register", _ {
			std::set<int32_t> heard;
			for (uint32_t seed = 1; seed <= 5; seed++) {
				Result result = play(runOf(OneMotif<kEvery16th>::genre, seed, 0), 2);
				expect(result.evictedNotes).to_equal(0U);
				expect(result.droppedEvents).to_equal(0U);
				for (const Event& e : pitched(result, EventKind::NoteOn)) {
					expect(positionOnI(e.pitch) != kNone).to_be_true();
					heard.insert(e.pitch);
				}
			}
			expect(heard.size()).to_equal(size_t{6});
		});

		it("follows a chord change without re-rolling: each slot plays its position on the new chord", _ {
			// i in bar 0, iv in bar 1, over one two-bar pass. Motion 0, so nothing re-rolls.
			for (uint32_t seed = 1; seed <= 5; seed++) {
				auto still = barsOf(play(runOf(OneMotif<kTwoBars>::genre, seed, 0), 4), 4);
				auto turning = barsOf(play(runOf(kTurningGenre, seed, 0), 4), 4);
				for (int32_t bar = 0; bar < 4; bar++) {
					for (int32_t s = 0; s < kStepsPerBar; s++) {
						const int32_t position = positionOnI(still[bar][s]);
						expect(position != kNone).to_be_true();
						if (position == kNone) {
							continue;
						}
						expect(turning[bar][s]).to_equal(bar % 2 == 0 ? kOnI[position] : kOnIV[position]);
					}
				}
				expect(still[2] == still[0] && still[3] == still[1]).to_be_true(); // the riff repeats
			}
		});

		it("walks the chord's Extensions: a position past the triad is the 7th when the chord has one", _ {
			static constexpr PositionWeight kFourth[] = {{3, 1}};
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Motif,
			                                       .pool = kEvery16th,
			                                       .reg = kMotifRegister,
			                                       .motif = {.positions = kFourth, .leap = 3, .octaveUp = 0}}};
			static constexpr Genre kTriad = motifGenre(kParts);
			static constexpr Genre kSeventh = [] {
				Genre genre = kTriad;
				genre.harmony.extensionFloor = 1;
				genre.harmony.extensionCeiling = 1;
				return genre;
			}();
			for (const Event& e : pitched(play(runOf(kTriad, 1, 0), 1), EventKind::NoteOn)) {
				expect(e.pitch).to_equal(uint8_t{69}); // A an octave up
			}
			for (const Event& e : pitched(play(runOf(kSeventh, 1, 0), 1), EventKind::NoteOn)) {
				expect(e.pitch).to_equal(uint8_t{67}); // G, Am7's 7th
			}
		});

		it("rolls with the live Complexity's leap range and octave-jump chance", _ {
			std::set<int32_t> plain;
			std::set<int32_t> busy;
			for (uint32_t seed = 1; seed <= 20; seed++) {
				for (const Event& e : pitched(play(runOf(kShapedGenre, seed, 0, 0), 2), EventKind::NoteOn)) {
					plain.insert(e.pitch);
				}
				for (const Event& e : pitched(play(runOf(kShapedGenre, seed, 0, 50), 2), EventKind::NoteOn)) {
					busy.insert(e.pitch);
				}
			}
			// Complexity 0: the root and the 3rd, no octave jumps. Complexity 50: every position, and jumps above them.
			expect(plain == std::set<int32_t>{57, 60}).to_be_true();
			expect(busy.size()).to_be_greater_than(size_t{6});
			expect(*busy.rbegin()).to_be_greater_than(76);
		});

		it("never shifts a note when Density thins it", _ {
			auto pitchByStep = [](const Result& result) {
				std::map<int32_t, int32_t> out;
				for (const Event& e : pitched(result, EventKind::NoteOn)) {
					out[stepOf(e)] = e.pitch;
				}
				return out;
			};
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Motif,
			                                       .pool = kThinned,
			                                       .reg = kMotifRegister,
			                                       .motif = kPlain}};
			static constexpr Genre kGenre = motifGenre(kParts);
			const auto full = pitchByStep(play(runOf(kGenre, 1, 0, 25, 50), 2));
			expect(full.size()).to_equal(size_t{2 * kStepsPerBar});
			for (int32_t density : {0, 15, 25, 35}) {
				const auto thinned = pitchByStep(play(runOf(kGenre, 1, 0, 25, density), 2));
				expect(thinned.size()).to_be_less_than(full.size());
				for (const auto& [step, pitch] : thinned) {
					expect(full.at(step)).to_equal(pitch);
				}
			}
		});
	});

	context("re-rolls (spec §5.2, §3.5)", _ {
		it("re-roll slots at pass ends along the Motif curve, never moving an onset or an anchor's pitch", _ {
			constexpr int32_t kBars = 24;
			int32_t changed = 0;
			int32_t compared = 0;
			for (uint32_t seed = 1; seed <= 20; seed++) {
				Result still = play(runOf(OneMotif<kBusy>::genre, seed, 0), kBars);
				Result moving = play(runOf(OneMotif<kBusy>::genre, seed, 50), kBars);
				auto calm = barsOf(still, kBars);
				auto lively = barsOf(moving, kBars);
				for (int32_t bar = 1; bar < kBars; bar++) {
					expect(calm[bar] == calm[0]).to_be_true(); // Motion 0 never re-rolls: deja vu
					for (int32_t s = 0; s < kStepsPerBar; s++) {
						expect(lively[bar][s] == kNone).to_equal(calm[bar][s] == kNone); // the rhythm stays
						if (s % 4 == 0) {
							expect(lively[bar][s]).to_equal(lively[0][s]); // anchors never move
						}
						else if (lively[bar][s] != kNone) {
							compared++;
							changed += lively[bar][s] != lively[bar - 1][s] ? 1 : 0;
						}
					}
				}
			}
			// The Motif curve is a straight line (R 2, start 0): 33% at Motion 25 is 67% at 50, and a re-rolled slot
			// lands on another of six positions five times in six: about 56%. The Loop's curve would give 83%.
			expect(changed * 100 / compared).to_be_greater_than(45);
			expect(changed * 100 / compared).to_be_less_than(68);
		});

		it("hold a frozen line with no re-rolls", _ {
			constexpr int32_t kBars = 10;
			constexpr int32_t kFreeze = 2 * kStepsPerBar + 8;
			for (uint32_t seed = 1; seed <= 10; seed++) {
				Run frozen = runOf(OneMotif<kBusy>::genre, seed, 50);
				frozen.steer = steerAt({{kFreeze, {CommandKind::SetFreeze, 0, kPress}}});
				auto b = barsOf(play(frozen, kBars), kBars);
				for (int32_t bar = 3; bar < kBars; bar++) {
					expect(b[bar] == b[2]).to_be_true();
				}
			}
		});
	});

	context("takes (spec §6.6)", _ {
		it("replays a take's line as it unfolded before, on stepping back to it within one pick", _ {
			// One pick throughout. Take 1 lands on bar 2 and the step back on bar 6, each on a phrase line.
			constexpr int32_t kBars = 16;
			constexpr int32_t kBack = 6;
			for (uint32_t seed = 1; seed <= 10; seed++) {
				Run plain = runOf(OneMotif<kBusy>::genre, seed, 50);
				Run stepped = plain;
				stepped.steer = steerAt({{20, {CommandKind::SetTake, 0, 1}}, {70, {CommandKind::SetTake, 0, 0}}});
				auto a = barsOf(play(plain, kBars), kBars);
				auto b = barsOf(play(stepped, kBars), kBars);
				expect(b[0] == a[0] && b[1] == a[1]).to_be_true();
				expect(b[2] == a[2]).to_be_false(); // take 1's own line
				for (int32_t bar = kBack; bar < kBars; bar++) {
					expect(b[bar] == a[bar - kBack]).to_be_true(); // rolled fresh, then re-rolling from its first pass
				}
			}
		});
	});

	context("note lengths (spec §5.2, §2.7)", _ {
		static constexpr PartSpec kGapParts[] = {{.role = Role::Lead,
		                                          .flavour = Flavour::Motif,
		                                          .pool = kGaps,
		                                          .reg = kMotifRegister,
		                                          .motif = {.positions = kSix, .gateTicks = 72}}};
		static constexpr Genre kGapGenre = motifGenre(kGapParts);

		it("lasts its gate, ending sooner at the pattern's next onset or the bar line", _ {
			Result result = play(runOf(kGapGenre, 1, 0), 2);
			auto ons = pitched(result, EventKind::NoteOn);
			auto offs = pitched(result, EventKind::NoteOff);
			expect(ons.size()).to_equal(size_t{8});
			expect(offs.size()).to_equal(size_t{8});
			// A note is long only where its onsets are sparse.
			const std::vector<int32_t> lengths{72, 2 * kStep, 72, kStep};
			for (size_t i = 0; i < lengths.size(); i++) {
				expect(offs[i].tick - ons[i].tick).to_equal(lengths[i]);
			}
		});

		it("ends by the bar line when it leans late", _ {
			// The second bar is empty, so nothing on the next bar line ends the last note for it.
			static constexpr Pattern kGapsThenRest[] = {
			    {"X...X.X........X................", "................................", 0}};
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Motif,
			                                       .pool = kGapsThenRest,
			                                       .reg = kMotifRegister,
			                                       .motif = {.positions = kSix, .gateTicks = 72},
			                                       .expression = {.lean = {0, 0, 0, 12}}}};
			static constexpr Genre kGenre = motifGenre(kParts);
			Result result = play(runOf(kGenre, 1, 0), 2);
			auto ons = pitched(result, EventKind::NoteOn);
			auto offs = pitched(result, EventKind::NoteOff);
			expect(ons.size()).to_equal(size_t{4});
			expect(offs.size()).to_equal(size_t{4});
			expect(ons[3].tick).to_equal(15 * kStep + 12);
			expect(offs[3].tick).to_equal(kBarTicks);
		});

		it("ends every note by the time the onset after its next one starts, over its uneven gaps, whatever its Lean", _ {
			static constexpr Expression kCrossing{
			    .lean = {12, -12, 12, -12},
			    .ornaments = {.rateFloor = 500, .rateCeiling = 500, .ratchet = {1, 0, 0}, .flam = 1, .flamGap = 6}};
			static constexpr Pattern kUneven[] = {{"X.XX...X.XX.X..XX..XXX..X.X...XX", "................................", 0}};
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Motif,
			                                       .pool = kUneven,
			                                       .reg = kMotifRegister,
			                                       .motif = {.positions = kSix, .gateTicks = 96},
			                                       .expression = kCrossing}};
			static constexpr Genre kGenre = motifGenre(kParts);
			expect(motifNotesValid(kGenre)).to_be_true();
			for (uint32_t seed : {1U, 2U, 3U}) {
				Result result = play(runOf(kGenre, seed, 50, 50), 8);
				expect(result.evictedNotes).to_equal(0U);
				expect(result.droppedEvents).to_equal(0U);
			}
		});
	});

	context("its genre data (spec §5.2)", _ {
		it("needs a pool, chord positions to draw from and a gate of at least a tick", _ {
			expect(motifValid(kGaps, kPlain)).to_be_true();
			expect(motifValid({}, kPlain)).to_be_false();
			expect(motifValid(kGaps, {.positions = {}})).to_be_false();
			expect(motifValid(kGaps, {.positions = kSix, .gateTicks = 0})).to_be_false();
		});
	});

	context("in a Techno-like roster (spec §4.5)", _ {
		it("plays the walk's chord tones under every chord, with none dropped or evicted, over whole Arcs", _ {
			int32_t notes = 0;
			std::set<int32_t> chords;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				for (int32_t complexity : {0, 25, 50}) {
					Recipe recipe = technoRecipe();
					recipe.genre = &kTechnoMotif;
					recipe.macros.set(Macro::Complexity, macroPoints(complexity));
					recipe.macros.set(Macro::Motion, macroPoints(40));
					Walked walked = walk(recipe, seed, kStepsPerBar * 8 * (2 + 8 + 2 + 2 + 2));
					expect(walked.dropped).to_equal(0U);
					expect(walked.evicted).to_equal(0U);
					const int32_t level = extensionLevel(recipe.macros[Macro::Complexity], 0, 2);
					int32_t sounding = 0;
					for (const Event& e : walked.notes) {
						if (e.part == kMotifPart) {
							sounding += e.kind == EventKind::NoteOn ? 1 : -1;
							expect(sounding).to_be_less_than(3);
						}
					}
					for (const Event& e : walked.noteOns) {
						const auto n = static_cast<size_t>(stepOf(e));
						if (e.part != kMotifPart || n >= walked.steps.size()) {
							continue;
						}
						const Chord chord = voiceChord(recipe.scale, walked.steps[n].chord, level);
						std::set<int32_t> classes;
						for (int32_t i = 0; i < chord.size; i++) {
							classes.insert((recipe.keyRoot + semitonesOf(chord.steps[i], recipe.scale)) % 12);
						}
						expect(classes.count(e.pitch % 12)).to_equal(size_t{1});
						notes++;
						chords.insert(walked.steps[n].chord);
					}
				}
			}
			expect(notes).to_be_greater_than(1000);
			expect(chords.size()).to_be_greater_than(size_t{2});
		});
	});
});
// clang-format on

CPPSPEC_SPEC(motif)
