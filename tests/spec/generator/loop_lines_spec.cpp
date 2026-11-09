// Loop lines (spec §5.2, §6.6, §6.7, §8.6): a line rolled at each pick and take change, mutation at pass ends, Answers,
// slide, and freeze, read off the event stream of small test genres with one Loop Bass.

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr int32_t kNone = -1;

/// Anchors on the beats, and a note on every other step that always plays at Density 50.
constexpr Pattern kBusy[] = {{"X9.9X9.9X9.9X9.9", "................", 0}};
/// Seven degrees, evenly weighted, so a re-rolled step usually changes.
constexpr DegreeWeight kSeven[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// Two-bar phrases and no Arc offsets, so the Recipe's macros are what plays. runOf() starts in Groove at Motion 0
/// or with no re-picks, so only steering changes the pick.
constexpr Genre streamGenre(std::span<const PartSpec> parts) {
	return {
	    .name = "loop",
	    .facts = {.scales = kScales},
	    .phraseBars = 2,
	    .parts = parts,
	    .repickChance = 0,
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

constexpr LoopSpec kMutating{.degrees = kSeven, .octaveUp = 0, .accent = 0, .slide = 0, .mutation = 333};
constexpr PartSpec kMutatingParts[] = {
    {.role = Role::Bass, .flavour = Flavour::Loop, .pool = kBusy, .loop = kMutating},
};
constexpr Genre kMutatingGenre = streamGenre(kMutatingParts);

/// Complexity 0 keeps to the root and the 2nd, plain; Complexity 50 leaps to every degree, jumps octaves and slides.
constexpr LoopSpec kShaped{
    .degrees = kSeven, .leap = {1, 6}, .octaveUp = {0, 500}, .accent = 0, .slide = {0, 500}, .gateTicks = 16};
constexpr PartSpec kShapedParts[] = {
    {.role = Role::Bass, .flavour = Flavour::Loop, .pool = kBusy, .loop = kShaped},
};
constexpr Genre kShapedGenre = streamGenre(kShapedParts);

/// Every step plays; half of them slide, onto any of seven degrees.
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr PartSpec kSlidingParts[] = {{
    .role = Role::Bass,
    .flavour = Flavour::Loop,
    .pool = kEveryStep,
    .loop = {.degrees = kSeven, .octaveUp = 0, .accent = 0, .slide = 500, .gateTicks = 16},
}};
constexpr Genre kSlidingGenre = streamGenre(kSlidingParts);

/// Every step slides onto the root.
constexpr DegreeWeight kRoot[] = {{0, 1}};
constexpr PartSpec kTiedParts[] = {{
    .role = Role::Bass,
    .flavour = Flavour::Loop,
    .pool = kEveryStep,
    .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 1000, .gateTicks = 16},
}};
constexpr Genre kTiedGenre = streamGenre(kTiedParts);

/// A Bass that re-picks at every phrase line from four entries, each its own anchor and none mutating, then a
/// Kick that does the same: freeze holds either.
constexpr Pattern kFourWays[] = {
    {"X...............", "................", 0},
    {"....X...........", "................", 0},
    {"........X.......", "................", 0},
    {"............X...", "................", 0},
};
constexpr LaneSpec kRepickingLanes[] = {{.lane = Lane::Kick, .pool = kFourWays}};
constexpr PartSpec kRepickingParts[] = {
    {.role = Role::Drums, .lanes = kRepickingLanes},
    {.role = Role::Bass, .flavour = Flavour::Loop, .pool = kFourWays, .loop = {.degrees = kSeven, .mutation = 0}},
};
constexpr Genre kRepickingGenre = [] {
	Genre genre = streamGenre(kRepickingParts);
	genre.repickChance = 1000;
	return genre;
}();

Run runOf(const Genre& genre, uint32_t seed, int32_t motion, int32_t complexity = 25) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.keyRoot = 0;
	run.recipe.scale = Scale::Minor;
	run.recipe.macros.set(Macro::Density, macroPoints(50));
	run.recipe.macros.set(Macro::Motion, macroPoints(motion));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.seed = seed;
	run.skipIntro = true;
	return run;
}

using Bar = std::array<int32_t, kStepsPerBar>;

/// For each bar, the pitch the Bass started on each step, or kNone.
std::vector<Bar> barsOf(const Result& result, int32_t count) {
	Bar silent;
	silent.fill(kNone);
	std::vector<Bar> out(count, silent);
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == Lane::None && e.tick / kBarTicks < count && !e.ornament) {
			out[e.tick / kBarTicks][(e.tick % kBarTicks) / kTicksPerStep] = e.pitch;
		}
	}
	return out;
}

std::vector<int32_t> onsetTicks(const Result& result) {
	std::vector<int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == Lane::None) {
			out.push_back(e.tick);
		}
	}
	return out;
}

// ---- Answers ----

/// A note on every step at Density 50 (an anchor on the 1), plain and never mutating, in a Register wide enough that
/// no Answer folds, answering with `List` every `Bars` bars at Motion 25.
constexpr Pattern kEvery[] = {{"X999999999999999", "................", 0}};
/// Five onsets, lopsided, so reversing the onsets and reversing the steps play different notes.
constexpr Pattern kFiveOnsets[] = {{"X.9...9..9...9..", "................", 0}};
/// The same five, with the one on step 2 thinned out below Density 50.
constexpr Pattern kThinnedOnset[] = {{"X.2...9..9...9..", "................", 0}};
constexpr Register kWide{.anchor = 48, .spanLow = -24, .spanHigh = 24};
template <const auto& List, uint8_t Bars = 4, const auto& Pool = kEvery>
struct Answering {
	static constexpr PartSpec parts[] = {{
	    .role = Role::Bass,
	    .flavour = Flavour::Loop,
	    .pool = Pool,
	    .reg = kWide,
	    .loop = {.degrees = kSeven,
	             .octaveUp = 0,
	             .accent = 300,
	             .slide = 0,
	             .mutation = 0,
	             .answers = List,
	             .answerBars = Bars},
	}};
	static constexpr Genre genre = streamGenre(parts);
};

constexpr Answer kOctave[] = {{AnswerKind::Octave}};
constexpr Answer kFifth[] = {{AnswerKind::Fifth}};
constexpr Answer kThird[] = {{AnswerKind::Third}};
constexpr Answer kReverse[] = {{AnswerKind::Reverse}};
constexpr Answer kRotate[] = {{AnswerKind::Rotate}};
constexpr Answer kMirror[] = {{AnswerKind::Mirror}};
constexpr Answer kSparse[] = {{AnswerKind::Sparse}};
constexpr Answer kAccentsOnly[] = {{AnswerKind::AccentsOnly}};
constexpr Answer kOctaveOrMirror[] = {{AnswerKind::Octave}, {AnswerKind::Mirror}};

/// The Bass Register's default, with octave jumps, answering an octave every 4 bars.
constexpr PartSpec kNarrowParts[] = {{
    .role = Role::Bass,
    .flavour = Flavour::Loop,
    .pool = kEvery,
    .loop = {.degrees = kSeven,
             .octaveUp = 300,
             .octaveDown = 300,
             .slide = 0,
             .mutation = 0,
             .answers = kOctave,
             .answerBars = 4},
}};
constexpr Genre kNarrowGenre = streamGenre(kNarrowParts);

/// A bar of the wide-Register Bass: each step's pitch in scale steps from its tonic (C3, the key is C minor) and its
/// velocity, or kNone.
struct Notes {
	std::array<int32_t, kStepsPerBar> steps;
	std::array<int32_t, kStepsPerBar> velocity;
	bool operator==(const Notes&) const = default;
};

int32_t scaleSteps(int32_t pitch) {
	constexpr int32_t kTonic = 48;
	const int32_t octave = (pitch - kTonic + 120) / 12 - 10;
	const int32_t semitone = pitch - kTonic - 12 * octave;
	for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
		if (kScaleSemitones[static_cast<int32_t>(Scale::Minor)][degree] == semitone) {
			return 7 * octave + degree;
		}
	}
	return 1000; // out of the scale
}

std::vector<Notes> notesOf(const Result& result, int32_t count) {
	Notes silent;
	silent.steps.fill(kNone);
	silent.velocity.fill(kNone);
	std::vector<Notes> out(count, silent);
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == Lane::None && e.tick / kBarTicks < count && !e.ornament) {
			const int32_t s = (e.tick % kBarTicks) / kTicksPerStep;
			out[e.tick / kBarTicks].steps[s] = scaleSteps(e.pitch);
			out[e.tick / kBarTicks].velocity[s] = e.velocity;
		}
	}
	return out;
}

/// Whether `answer` is `call` moved by `shift` scale steps, one way for the whole bar. @return +1 or -1 for the way,
/// or 0 if it isn't.
int32_t shiftedBy(const Notes& call, const Notes& answer, int32_t shift) {
	for (int32_t way : {1, -1}) {
		bool all = true;
		for (int32_t s = 0; s < kStepsPerBar; s++) {
			all = all && answer.steps[s] == call.steps[s] + way * shift;
		}
		if (all) {
			return way;
		}
	}
	return 0;
}

/// Whether `answer` plays step i with the pitch of step from(i) in `call`.
template <typename From>
bool reordered(const Notes& call, const Notes& answer, From from) {
	for (int32_t s = 0; s < kStepsPerBar; s++) {
		if (answer.steps[s] != call.steps[from(s)]) {
			return false;
		}
	}
	return true;
}

/// Whether `answer` reflects `call` about the middle of its range: min + max - pitch, in scale steps.
bool mirrored(const Notes& call, const Notes& answer) {
	const auto [low, high] = std::minmax_element(call.steps.begin(), call.steps.end());
	for (int32_t s = 0; s < kStepsPerBar; s++) {
		if (answer.steps[s] != *low + *high - call.steps[s]) {
			return false;
		}
	}
	return true;
}

/// Bars whose notes differ from bar 0's, which is always a call.
std::set<int32_t> answerBars(const Result& result, int32_t count) {
	auto notes = notesOf(result, count);
	std::set<int32_t> out;
	for (int32_t bar = 1; bar < count; bar++) {
		if (!(notes[bar] == notes[0])) {
			out.insert(bar);
		}
	}
	return out;
}

/// Bass notes that last past their step: slides (gates are shorter than a step).
int32_t longNotes(const Result& result) {
	std::map<int32_t, int32_t> on; // pitch → tick
	int32_t count = 0;
	for (const Event& e : result.events) {
		if (e.kind == EventKind::NoteOn && e.lane == Lane::None) {
			on[e.pitch] = e.tick;
		}
		else if (e.kind == EventKind::NoteOff && e.lane == Lane::None && on.contains(e.pitch)) {
			count += e.tick - on[e.pitch] > kTicksPerStep ? 1 : 0;
			on.erase(e.pitch);
		}
	}
	return count;
}

} // namespace

// clang-format off
describe loop_lines("Loop lines", $ {
	it("mutate at pass ends along the mutation curve, never moving an onset or an anchor's pitch", _ {
		constexpr int32_t kBars = 24;
		int32_t changed = 0;
		int32_t compared = 0;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			Result still = play(runOf(kMutatingGenre, seed, 0), kBars);
			Result moving = play(runOf(kMutatingGenre, seed, 50), kBars);
			expect(onsetTicks(moving) == onsetTicks(still)).to_be_true();

			auto calm = barsOf(still, kBars);
			auto lively = barsOf(moving, kBars);
			for (int32_t bar = 1; bar < kBars; bar++) {
				expect(calm[bar] == calm[0]).to_be_true(); // Motion 0 never mutates
				for (int32_t s = 0; s < kStepsPerBar; s++) {
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
		// At Motion 50 the curve triples the Motion 25 chance (spec §3.5): 33% becomes 99%, and a re-rolled step
		// lands on another of seven degrees six times in seven. A straight line would double it, to about 57%.
		expect(changed * 100 / compared).to_be_greater_than(75);
	});

	it("roll with the live Complexity's leap range, octave-jump chance and slide chance", _ {
		std::set<int32_t> plainPitches;
		std::set<int32_t> busyPitches;
		int32_t plainSlides = 0;
		int32_t busySlides = 0;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			for (int32_t complexity : {0, 50}) {
				Result result = play(runOf(kShapedGenre, seed, 0, complexity), 2);
				std::set<int32_t>& pitches = complexity == 0 ? plainPitches : busyPitches;
				int32_t& slides = complexity == 0 ? plainSlides : busySlides;
				for (const Event& e : only(result.events, EventKind::NoteOn)) {
					pitches.insert(e.pitch);
				}
				slides += longNotes(result);
			}
		}
		// Complexity 0: a leap of one scale step from the chord root (C, D), no octave jumps and no slides.
		expect(plainPitches == std::set<int32_t>{36, 38}).to_be_true();
		expect(plainSlides).to_equal(0);
		// Complexity 50: every degree, octave jumps (C an octave up fits the Bass Register) and slides.
		expect(busyPitches.size()).to_be_greater_than(size_t{7});
		expect(busyPitches.contains(48)).to_be_true();
		expect(busySlides).to_be_greater_than(0);
	});

	it("answer in the last bar of every N bars from the pick, N divided by the Answers curve", _ {
		constexpr int32_t kBars = 16;
		for (uint32_t seed = 1; seed <= 5; seed++) {
			const Genre& genre = Answering<kOctave>::genre;
			expect(answerBars(play(runOf(genre, seed, 0), kBars), kBars).empty()).to_be_true(); // never at Motion 0
			expect(answerBars(play(runOf(genre, seed, 25), kBars), kBars) == std::set<int32_t>{3, 7, 11, 15})
			    .to_be_true();
			expect(answerBars(play(runOf(genre, seed, 50), kBars), kBars) == std::set<int32_t>{1, 3, 5, 7, 9, 11, 13, 15})
			    .to_be_true();
		}
	});

	it("move every note of an octave, fifth or third Answer by its scale steps, up or down by a keyed even draw", _ {
		std::set<int32_t> ways;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto octave = notesOf(play(runOf(Answering<kOctave>::genre, seed, 25), 4), 4);
			auto fifth = notesOf(play(runOf(Answering<kFifth>::genre, seed, 25), 4), 4);
			auto third = notesOf(play(runOf(Answering<kThird>::genre, seed, 25), 4), 4);
			expect(shiftedBy(octave[2], octave[3], 7)).not_().to_equal(0);
			expect(shiftedBy(fifth[2], fifth[3], 4)).not_().to_equal(0);
			expect(shiftedBy(third[2], third[3], 2)).not_().to_equal(0);
			ways.insert(shiftedBy(octave[2], octave[3], 7));
		}
		expect(ways == std::set<int32_t>{-1, 1}).to_be_true();
	});

	it("reorder pitch only in a reverse, rotate or mirror Answer, keeping the pattern's rhythm", _ {
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto reverse = notesOf(play(runOf(Answering<kReverse>::genre, seed, 25), 4), 4);
			auto rotate = notesOf(play(runOf(Answering<kRotate>::genre, seed, 25), 4), 4);
			auto mirror = notesOf(play(runOf(Answering<kMirror>::genre, seed, 25), 4), 4);
			expect(reordered(reverse[2], reverse[3], [](int32_t s) { return kStepsPerBar - 1 - s; })).to_be_true();
			expect(reordered(rotate[2], rotate[3], [](int32_t s) { return (s - 4 + kStepsPerBar) % kStepsPerBar; }))
			    .to_be_true();
			expect(mirrored(mirror[2], mirror[3])).to_be_true();
		}
	});

	it("reverse the onsets' pitch content, counted before thinning: the k-th onset plays the (n-1-k)-th's", _ {
		// Onsets on steps 0, 2, 6, 9 and 13: the reverse plays 13, 9, 6, 2, 0's content there. Reversing the steps
		// would take step 15's, 13's, 9's... content, most of it from steps that never play.
		constexpr int32_t kOnsets[] = {0, 2, 6, 9, 13};
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto notes = notesOf(play(runOf(Answering<kReverse, 4, kFiveOnsets>::genre, seed, 25), 4), 4);
			for (int32_t k = 0; k < 5; k++) {
				expect(notes[3].steps[kOnsets[k]]).to_equal(notes[2].steps[kOnsets[4 - k]]);
			}
			// Thinned the same way, from the same steps: the rhythm stays the pattern's.
			for (int32_t s = 0; s < kStepsPerBar; s++) {
				expect(notes[3].steps[s] == kNone).to_equal(notes[2].steps[s] == kNone);
			}

			// At Density 40 the onset on step 2 is thinned out, and the four left (k = 0, 2, 3, 4) still pair with
			// onsets 4, 2, 1 and 0: step 9 plays step 2's content, which only a full-Density call lets us hear.
			// Pairing the onsets left after thinning would put step 6's content on step 9 and step 9's on step 6.
			const Genre& thinGenre = Answering<kReverse, 4, kThinnedOnset>::genre;
			auto full = notesOf(play(runOf(thinGenre, seed, 25), 4), 4);
			Run thin = runOf(thinGenre, seed, 25);
			thin.recipe.macros.set(Macro::Density, macroPoints(40));
			auto thinned = notesOf(play(thin, 4), 4);
			expect(thinned[3].steps[2]).to_equal(kNone);
			for (int32_t k : {0, 2, 3, 4}) {
				expect(thinned[3].steps[kOnsets[k]]).to_equal(full[2].steps[kOnsets[4 - k]]);
			}
		}
	});

	it("silence odd steps in a sparse Answer and unaccented ones in an accents-only Answer, never an anchor", _ {
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto sparse = notesOf(play(runOf(Answering<kSparse>::genre, seed, 25), 4), 4);
			auto accents = notesOf(play(runOf(Answering<kAccentsOnly>::genre, seed, 25), 4), 4);
			const auto [soft, accent] = std::minmax_element(accents[2].velocity.begin(), accents[2].velocity.end());
			for (int32_t s = 0; s < kStepsPerBar; s++) {
				const bool anchor = s == 0;
				expect(sparse[3].steps[s]).to_equal(s % 2 == 1 && !anchor ? kNone : sparse[2].steps[s]);
				const bool accented = *accent > *soft && accents[2].velocity[s] == *accent;
				expect(accents[3].steps[s]).to_equal(accented || anchor ? accents[2].steps[s] : kNone);
			}
		}
	});

	it("choose the tier from the live Complexity at the Answer's first step, holding it to the Answer's end", _ {
		const Genre& genre = Answering<kOctaveOrMirror>::genre;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			// Octave and mirror span tiers 1 to 3: octave up to Complexity 25 (tier 2 is empty), mirror at 50.
			for (int32_t complexity : {0, 25}) {
				auto notes = notesOf(play(runOf(genre, seed, 25, complexity), 4), 4);
				expect(shiftedBy(notes[2], notes[3], 7)).not_().to_equal(0);
			}
			auto busy = notesOf(play(runOf(genre, seed, 25, 50), 4), 4);
			expect(mirrored(busy[2], busy[3])).to_be_true();

			// Complexity turned to 50 partway through the first Answer: the next Answer mirrors.
			Run turned = runOf(genre, seed, 25, 0);
			turned.steer = at(3 * kStepsPerBar + 5, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Complexity),
			                                         macroPoints(50)});
			auto notes = notesOf(play(turned, 8), 8);
			expect(shiftedBy(notes[2], notes[3], 7)).not_().to_equal(0);
			expect(mirrored(notes[6], notes[7])).to_be_true();
		}
	});

	it("move only the notes whose pitch class fits twice in a narrow Register on an octave Answer", _ {
		// The Bass Register, -5..+12 from the tonic: the root and the 5th to the 7th fit twice (spec §5.2, §7.3).
		int32_t moved = 0;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto bars = barsOf(play(runOf(kNarrowGenre, seed, 25), 4), 4);
			for (int32_t s = 0; s < kStepsPerBar; s++) {
				const int32_t call = bars[2][s];
				const int32_t answer = bars[3][s];
				if (answer == call) {
					continue;
				}
				moved++;
				expect(answer - call == 12 || call - answer == 12).to_be_true();
				const int32_t fromTonic = (call - 36 + 12) % 12;
				expect(fromTonic == 0 || fromTonic >= 7).to_be_true();
			}
		}
		expect(moved).to_be_greater_than(0);
	});

	it("slide by running a note into the next onset, and start a note after a gap fresh", _ {
		int32_t slides = 0;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Result result = play(runOf(kSlidingGenre, seed, 0), 4);
			// Each note-on with its pitch and note-off, in onset order.
			struct Note {
				int32_t on;
				int32_t pitch;
				int32_t off;
			};
			std::vector<Note> notes;
			std::map<int32_t, size_t> open; // pitch → index in notes
			for (const Event& e : result.events) {
				if (e.kind == EventKind::NoteOn) {
					open[e.pitch] = notes.size();
					notes.push_back({e.tick, e.pitch, INT32_MAX});
				}
				else if (e.kind == EventKind::NoteOff && open.contains(e.pitch)) {
					notes[open[e.pitch]].off = e.tick;
					open.erase(e.pitch);
				}
			}
			for (size_t i = 0; i + 1 < notes.size(); i++) {
				const Note& note = notes[i];
				const Note& next = notes[i + 1];
				if (next.on != note.on + kTicksPerStep || next.pitch == note.pitch) {
					continue; // a tie, below
				}
				// A plain note ends before the next starts; a slid one runs into it, so a LEGATO voice glides.
				expect(note.off == note.on + 16 || note.off == next.on + 6).to_be_true();
				slides += note.off > next.on ? 1 : 0;
			}
			// Every step plays, so only a note on the last step has no next onset: it ends after its gate or slide.
			expect(notes.back().off).to_be_less_than(notes.back().on + kTicksPerStep + 7);
		}
		expect(slides).to_be_greater_than(0);
	});

	it("tie a slide onto the same note instead of starting it again", _ {
		// Every step slides onto the root: one note, held for the whole run.
		Result result = play(runOf(kTiedGenre, 1, 0), 2);
		auto ons = only(result.events, EventKind::NoteOn);
		expect(ons.size()).to_equal(size_t{1});
		auto offs = only(result.events, EventKind::NoteOff);
		expect(offs.size()).to_equal(size_t{1});
		expect(offs[0].tick).to_be_greater_than(2 * kBarTicks - kTicksPerStep);
	});

	it("hold a frozen line with no mutation, counting its passes on, and land a freeze press where it is decided", _ {
		// Frozen partway through bar 2, unfrozen partway through bar 8, both by pad presses.
		constexpr int32_t kBars = 12;
		constexpr int32_t kFreeze = 2 * kStepsPerBar + 8;
		constexpr int32_t kThaw = 8 * kStepsPerBar + 3;
		int32_t carriedOn = 0;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run plain = runOf(kMutatingGenre, seed, 50);
			Run frozen = plain;
			frozen.steer = steerAt({{kFreeze, {CommandKind::SetFreeze, 0, kPress}},
			                        {kThaw, {CommandKind::SetFreeze, 0, kPress}}});
			LogEntry entries[4];
			SteeringLog log(entries, 4);
			frozen.log = &log;
			auto a = barsOf(play(plain, kBars), kBars);
			auto b = barsOf(play(frozen, kBars), kBars);
			expect(b[2] == a[2]).to_be_true();
			for (int32_t bar = 3; bar <= 8; bar++) {
				expect(b[bar] == b[2]).to_be_true(); // held as it was, through the thaw's bar
			}
			// The next pass end mutates with the pass count the unfrozen run has there: each step either re-rolled
			// as it did in that run, or still holds.
			for (int32_t s = 0; s < kStepsPerBar; s++) {
				expect(b[9][s] == a[9][s] || b[9][s] == b[8][s]).to_be_true();
				carriedOn += b[9][s] == a[9][s] && b[9][s] != b[8][s] ? 1 : 0;
			}
			expect(log.size()).to_equal(2);
			expect(log.at(0).step).to_equal(kFreeze);
			expect(log.at(0).command.value).to_equal(1);
			expect(log.at(1).step).to_equal(kThaw);
			expect(log.at(1).command.value).to_equal(0);
		}
		expect(carriedOn).to_be_greater_than(0);
	});

	it("replay a frozen part's Answers from the phrase it froze in", _ {
		// Two-bar phrases and an octave Answer every 4 bars: bars 3, 7, 11. Frozen in bar 2, every phrase's second
		// bar answers as bar 3 did.
		const Genre& genre = Answering<kOctave>::genre;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run frozen = runOf(genre, seed, 25);
			frozen.steer = at(2 * kStepsPerBar + 1, {CommandKind::SetFreeze, 0, 1});
			auto notes = notesOf(play(frozen, 12), 12);
			for (int32_t bar = 5; bar < 12; bar += 2) {
				expect(notes[bar] == notes[3]).to_be_true();
				expect(notes[bar - 1] == notes[2]).to_be_true();
			}
			expect(notes[3] == notes[2]).to_be_false();
		}
	});

	it("replay the phrase just heard when a freeze lands on a phrase line", _ {
		// The freeze lands on bar 2, the second phrase's first step. The phrase heard last is bars 0-1, with no
		// Answer (bars 3, 7, 11 answer); the frozen part plays it again and again.
		const Genre& genre = Answering<kOctave>::genre;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run frozen = runOf(genre, seed, 25);
			frozen.steer = at(2 * kStepsPerBar, {CommandKind::SetFreeze, 0, 1});
			auto notes = notesOf(play(frozen, 8), 8);
			for (int32_t bar = 2; bar < 8; bar++) {
				expect(notes[bar] == notes[bar % 2]).to_be_true();
			}
		}
	});

	it("keep a frozen part's pick through re-picks, and take the latest on unfreeze", _ {
		// Every phrase line re-picks. The Drums and the Bass freeze in bar 1 and unfreeze partway through bar 7, or on
		// the phrase line at bar 8, where the line's own re-pick lands on top of the one that waited.
		constexpr int32_t kBars = 12;
		for (int32_t kThaw : {7 * kStepsPerBar + 5, 8 * kStepsPerBar})
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run plain = runOf(kRepickingGenre, seed, 25);
			Run frozen = plain;
			frozen.steer = steerAt({{kStepsPerBar + 2, {CommandKind::SetFreeze, 0, 1}},
			                        {kStepsPerBar + 2, {CommandKind::SetFreeze, 1, 1}},
			                        {kThaw, {CommandKind::SetFreeze, 0, 0}},
			                        {kThaw, {CommandKind::SetFreeze, 1, 0}}});
			Result a = play(plain, kBars);
			Result b = play(frozen, kBars);
			// A part's onsets in a bar, as steps: each entry has one, so they name the entry playing.
			auto onsets = [](const Result& result, int32_t part, int32_t bar) {
				std::vector<int32_t> out;
				for (const Event& e : only(result.events, EventKind::NoteOn)) {
					if (e.part == part && e.tick / kBarTicks == bar) {
						out.push_back(e.tick % kBarTicks / kTicksPerStep);
					}
				}
				return out;
			};
			bool changed = false;
			for (int32_t part : {0, 1}) {
				for (int32_t bar = 2; bar < 7; bar++) {
					expect(onsets(b, part, bar) == onsets(b, part, 1)).to_be_true(); // held
					changed = changed || onsets(a, part, bar) != onsets(a, part, 1);
				}
			}
			expect(changed).to_be_true(); // while the unfrozen run moved on

			// From the thaw on, the picks the unfrozen run has, and the Bass's line rolled fresh with its pick.
			auto from = [](const Result& result, int32_t step) {
				std::vector<Event> out;
				for (const Event& e : only(result.events, EventKind::NoteOn)) {
					if (e.tick >= stepTick(step)) {
						out.push_back(e);
					}
				}
				return out;
			};
			expect(from(b, kThaw) == from(a, kThaw)).to_be_true();
		}
	});

	it("replay a take's line as it unfolded before, on stepping back to it within one pick", _ {
		// No re-picks, so the whole run is one pick. Take 1 lands on bar 2 and the step back on bar 6, each on a
		// phrase line (spec §6.6).
		constexpr int32_t kBars = 16;
		constexpr int32_t kBack = 6;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run plain = runOf(kMutatingGenre, seed, 50);
			Run stepped = plain;
			stepped.steer = steerAt({{20, {CommandKind::SetTake, 0, 1}}, {70, {CommandKind::SetTake, 0, 0}}});
			auto a = barsOf(play(plain, kBars), kBars);
			auto b = barsOf(play(stepped, kBars), kBars);
			expect(b[0] == a[0] && b[1] == a[1]).to_be_true();
			expect(b[2] == a[2]).to_be_false(); // take 1's own line
			for (int32_t bar = kBack; bar < kBars; bar++) {
				expect(b[bar] == a[bar - kBack]).to_be_true(); // rolled fresh, then mutating from its first pass
			}
		}
	});
});
// clang-format on

CPPSPEC_SPEC(loop_lines)
