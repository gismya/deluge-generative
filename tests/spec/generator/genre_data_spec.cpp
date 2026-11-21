// Checks every built-in genre's data (spec §4, §5.4, §7.3–§7.5, §9.3).

#include "generative/core/capacity.h"
#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/motion_curve.h"
#include "generative/core/recipe.h"
#include "generative/perform/genre_menu.h"
#include "generative/perform/glide.h"
#include "techno_motif.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <vector>

using namespace deluge::generative;

namespace {

/// Every pool a genre's parts and lanes pick from.
std::vector<std::span<const Pattern>> pools(const Genre& genre) {
	std::vector<std::span<const Pattern>> out;
	for (const PartSpec& part : genre.parts) {
		if (!part.pool.empty()) {
			out.push_back(part.pool);
		}
		for (const LaneSpec& lane : part.lanes) {
			out.push_back(lane.pool);
		}
	}
	return out;
}

/// Every pool a genre's parts and lanes pick from, and every lane's fill pool (spec §8.7).
std::vector<std::span<const Pattern>> poolsAndFillPools(const Genre& genre) {
	std::vector<std::span<const Pattern>> out = pools(genre);
	for (const PartSpec& part : genre.parts) {
		for (const LaneSpec& lane : part.lanes) {
			out.push_back(lane.fillPool);
		}
	}
	return out;
}

/// The importance of the pattern's strongest hit off the beat (an e, & or a), kAnchor for an off-beat anchor, or 0.
int32_t strongestOffBeat(const Pattern& pattern) {
	int32_t strongest = 0;
	for (int32_t i = 0; i < pattern.length; i++) {
		if (i % kStepsPerBeat != 0 && pattern.importance[i] > strongest) {
			strongest = pattern.importance[i];
		}
	}
	return strongest;
}

/// The authoring rule (spec §8.1): an anchor off the beat, or a hit off the beat that ranks in the top half of the
/// pattern's importances, so low Density keeps the pattern's syncopation. Anchors always play, so they are left out of
/// the ranking.
bool keepsSyncopation(const Pattern& pattern) {
	int32_t offBeat = strongestOffBeat(pattern);
	if (offBeat == kAnchor) {
		return true;
	}
	if (offBeat == 0) {
		return false;
	}
	int32_t hits = 0;
	int32_t higher = 0;
	for (int32_t i = 0; i < pattern.length; i++) {
		int32_t importance = pattern.importance[i];
		if (importance > 0 && importance < kAnchor) {
			hits++;
			higher += importance > offBeat ? 1 : 0;
		}
	}
	return 2 * higher < hits;
}

constexpr Pattern kSyncopated[] = {
    {"X...X...X...X.3.", "................", 2}, // its only other hit is off the beat
    {"9.7.5...9.7.5...", "................", 1}, // its off-beat 7s rank below the 9s, in the top half
    {"..X.............", "................", 1}, // an off-beat anchor
};
constexpr Pattern kStraight[] = {
    {"9.3.9.3.9.3.9.3.", "................", 1}, // off-beat hits rank in the bottom half
    {"9.5.9.5.9.5.9.5.", "................", 1}, // and ties don't lift them into the top half
    {"X...X...X...X...", "................", 1}, // nothing off the beat
};

/// Whether a genre of one Bass part playing a Loop with `loop` and `expression` passes loopNotesValid().
bool loopValid(const LoopSpec& loop, const Expression& expression, std::span<const Pattern> pool = {}) {
	const PartSpec parts[] = {
	    {.role = Role::Bass, .flavour = Flavour::Loop, .pool = pool, .loop = loop, .expression = expression}};
	return loopNotesValid(Genre{.name = "loop", .parts = parts});
}

/// Notes on the 1 and the & of 3: gaps of 10 and 6 steps, the pattern's end counted as an onset.
constexpr Pattern kLongNotes[] = {{"X.........5.....", "................", 0}};

/// Two bars: notes on the first bar's 1 and the second bar's beat 4, gaps of 28 steps and 4 to the pattern's end,
/// where it wraps.
constexpr Pattern kTwoBarLongNotes[] = {{"X...........................5...", "................................", 0}};

/// A period in bars that doesn't fit a `phraseBars` phrase (fitsPhrase()): longer than it, and no multiple of it. A
/// 1-bar phrase has none, since every period fits it: the specs skip those genres, and check it still doesn't fit.
constexpr uint8_t unfitBars(int32_t phraseBars) {
	return static_cast<uint8_t>(phraseBars + 1);
}

/// A Loop that always slides, `overlap` ticks into the next step.
constexpr LoopSpec sliding(uint8_t overlap) {
	return {.slide = 1000, .slideOverlapTicks = overlap};
}

} // namespace

// clang-format off
describe genre_data("Every built-in genre", $ {
	it("has a stable lowercase name it can be found by, a subgenre's hyphenated (spec §4.4)", _ {
		expect(allGenres().empty()).to_be_false();
		for (const Genre* genre : allGenres()) {
			expect(std::strlen(genre->name)).to_be_greater_than(size_t{0});
			for (const char* c = genre->name; *c; c++) {
				const bool hyphen = *c == '-' && genre->parent != nullptr;
				expect(std::islower(static_cast<unsigned char>(*c)) != 0 || hyphen).to_be_true();
			}
			expect(findGenre(genre->name) == genre).to_be_true();
		}
		expect(findGenre("polka") == nullptr).to_be_true();
	});

	it("has OLED names that fit a genre change's confirm title, and short names (spec §4.4, §9.7)", _ {
		for (const Genre* genre : allGenres()) {
			expect(std::strlen(genre->oledName)).to_be_greater_than(size_t{0});
			expect(std::strlen(genre->oledName))
			    .to_be_less_than(static_cast<size_t>(deluge::generative::perform::kMaxGenreOledName) + 1);
			expect(genre->parent == nullptr || std::strlen(genre->shortName) > 0).to_be_true();
		}
	});

	it("has 1–6 parts, lanes in the fixed lane order, and a pool for every patterned part and Drums lane", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->parts.size()).to_be_between(size_t{1}, size_t{kMaxParts});
			int32_t drumsParts = 0;
			for (const PartSpec& part : genre->parts) {
				drumsParts += part.role == Role::Drums ? 1 : 0;
				int32_t previous = -1;
				for (const LaneSpec& lane : part.lanes) {
					expect(part.role == Role::Drums || part.role == Role::FX).to_be_true();
					expect(isFxLane(lane.lane)).to_equal(part.role == Role::FX);
					expect(static_cast<int32_t>(lane.lane)).to_be_greater_than(previous);
					previous = static_cast<int32_t>(lane.lane);
					// Section rules play FX lanes (§5.4). A Drums lane with no pool plays only the roll, the Phrase accent
					// or a Swap, so a rule or a fill pool must name it.
					bool ruled = lane.lane == genre->arc.roll.lane || lane.lane == genre->arc.accentLane;
					if (isFxLane(lane.lane)) {
						expect(lane.pool.empty() && lane.fillPool.empty()).to_be_true();
					}
					else {
						expect(!lane.pool.empty() || !lane.fillPool.empty() || ruled).to_be_true();
					}
					expect(lane.band.low).to_be_less_than(lane.band.high + 1);
				}
				if (part.flavour == Flavour::Loop) {
					expect(part.pool.empty()).to_be_false();
					expect(part.loop.degrees.empty()).to_be_false();
					// A Loop that answers does so every N bars, N at least 1, with a weight on some transform.
					if (!part.loop.answers.empty()) {
						expect(part.loop.answerBars).to_be_greater_than(uint8_t{0});
						int32_t weights = 0;
						for (const Answer& answer : part.loop.answers) {
							weights += answer.weight;
							expect(answer.kind == AnswerKind::None).to_be_false();
						}
						expect(weights).to_be_greater_than(0);
					}
					expect(part.loop.draws.leap.atZero >= 0 && part.loop.draws.leap.atFull >= 0).to_be_true();
				}
			}
			expect(drumsParts).to_be_less_than(2);
		}
	});

	it("gives no two parts the same lane: one SoundPicks holds every part's lane samples (spec §10.7)", _ {
		for (const Genre* genre : allGenres()) {
			bool used[kNumLanes] = {};
			for (const PartSpec& part : genre->parts) {
				for (const LaneSpec& lane : part.lanes) {
					const auto index = static_cast<int32_t>(lane.lane);
					expect(used[index]).to_be_false();
					used[index] = true;
				}
			}
		}
	});

	it("starts every part at 1–100 percent of its preset's volume knob, never above it (spec §10.6)", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				expect(part.volume).to_be_greater_than(uint8_t{0});
				expect(part.volume).to_be_less_than(uint8_t{101});
			}
		}
	});

	it("gives every pitched part a Register at least 12 semitones wide", _ {
		// So a Sustain or Stab voicing finds an instance of every pitch class inside it (spec §5.2).
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				if (part.lanes.empty()) {
					Register reg = part.registerOrDefault();
					expect(reg.spanHigh - reg.spanLow).to_be_greater_than(11);
				}
			}
		}
	});

	it("gives every pitched role a default Register at least 12 semitones wide", _ {
		for (Role role : {Role::Bass, Role::Chords, Role::Pad, Role::Lead}) {
			Register reg = defaultRegister(role);
			expect(reg.anchor).to_be_greater_than(uint8_t{0});
			expect(reg.spanHigh - reg.spanLow).to_be_greater_than(11);
		}
	});

	it("names a Degree set only on Bass and Lead parts that play Loop, Arp or Motif", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				bool mayNarrow = (part.role == Role::Bass || part.role == Role::Lead)
				                 && (part.flavour == Flavour::Loop || part.flavour == Flavour::Arp
				                     || part.flavour == Flavour::Motif);
				expect(mayNarrow || part.degreeSet == DegreeSet::Full).to_be_true();
			}
		}
	});

	it("fits its roster in the Generator block: its notes, step events and control sets", _ {
		for (const Genre* genre : allGenres()) {
			// rosterFits() is the rule; its parts are checked one by one too, so a failure names the one that broke.
			std::vector<Role> roster;
			for (int32_t p = 0; p < partCount(*genre); p++) {
				roster.push_back(genre->parts[p].role);
			}
			expect(soundingOf(roster)).to_be_less_than(SoundingNotes::kTotal + 1);
			for (bool barLine : {true, false}) {
				expect(stepEventsOf(roster, barLine)).to_be_less_than(stepEventsOf(kLargestRoster, barLine) + 1);
			}
			expect(controlSets(*genre)).to_be_less_than(kMaxControlSets + 1);
			expect(controlEventsOf(*genre)).to_be_less_than(kMaxControlEventsPerStep + 1);
			expect(rosterFits(*genre)).to_be_true();
		}
	});

	it("rejects a roster past the largest one's notes (rosterFits())", _ {
		const PartSpec pads[] = {{.role = Role::Pad}, {.role = Role::Pad}, {.role = Role::Pad},
		                         {.role = Role::Pad}, {.role = Role::Pad}, {.role = Role::Pad}};
		expect(rosterFits(Genre{.name = "pads", .parts = pads})).to_be_false();
		const PartSpec largest[] = {{.role = Role::Drums}, {.role = Role::Bass},
		                            {.role = Role::Pad},   {.role = Role::Chords},
		                            {.role = Role::Lead},  {.role = Role::FX}};
		expect(rosterFits(Genre{.name = "largest", .parts = largest})).to_be_true();
	});

	it("rejects a roster past kMaxParts, whose parts past it would never play (rosterFits())", _ {
		const PartSpec six[] = {{.role = Role::Bass}, {.role = Role::Bass}, {.role = Role::Bass},
		                        {.role = Role::Bass}, {.role = Role::Bass}, {.role = Role::Bass}};
		expect(rosterFits(Genre{.name = "six", .parts = six})).to_be_true();
		const PartSpec seven[] = {{.role = Role::Bass}, {.role = Role::Bass}, {.role = Role::Bass}, {.role = Role::Bass},
		                          {.role = Role::Bass}, {.role = Role::Bass}, {.role = Role::Bass}};
		expect(rosterFits(Genre{.name = "seven", .parts = seven})).to_be_false();
	});

	it("holds every genre to genreValid(), which fails when one of its rules does", _ {
		int32_t broken = 0;
		int32_t rollCuts = 0;
		int32_t rolling = 0; // genres with a roll lane
		for (const Genre* genre : allGenres()) {
			expect(genreValid(*genre)).to_be_true();
			Genre noPalette = *genre;
			noPalette.palette = nullptr;
			expect(genreValid(noPalette)).to_be_false();
			const bool misfits = genre->phraseBars > 1; // a 1-bar phrase fits every period
			const uint8_t unfit = unfitBars(genre->phraseBars);
			const WeightedBars unfitBuild[] = {{unfit, 1}};
			if (misfits) {
				expect(fitsPhrase(unfit, genre->phraseBars)).to_be_false();
				Genre oddBuild = *genre;
				oddBuild.arc.buildBars = unfitBuild; // buildBarsValid()
				expect(genreValid(oddBuild)).to_be_false();
			}
			Genre phraseCut = *genre; // cutAndGapUnderBuild(): a cut over a whole phrase
			phraseCut.arc.buildCutBars = genre->phraseBars;
			expect(rollUncut(phraseCut)).to_be_true(); // fails on its own rule, not the roll's
			expect(genreValid(phraseCut)).to_be_false();
			const Lane roll = genre->arc.roll.lane;
			if (roll != Lane::None) {
				rolling++;
				Genre cutRoll = *genre; // rollUncut(): the cut by lane
				cutRoll.arc.buildCutBars = 1;
				cutRoll.arc.buildCut.lanes |= laneBit(roll);
				expect(rollUncut(cutRoll)).to_be_false(); // its own rule, whatever a 1-bar Build makes of the cut
				expect(genreValid(cutRoll)).to_be_false();
				for (size_t p = 0; p < genre->parts.size(); p++) {
					const PartSpec& part = genre->parts[p];
					if (std::ranges::any_of(part.lanes, [&](const LaneSpec& lane) { return lane.lane == roll; })) {
						cutRoll = *genre; // and by the part that holds it
						cutRoll.arc.buildCutBars = 1;
						cutRoll.arc.buildCut.parts |= partBit(static_cast<int32_t>(p));
						expect(genreValid(cutRoll)).to_be_false();
						rollCuts++;
					}
				}
			}
			for (size_t p = 0; p < genre->parts.size(); p++) {
				std::vector<PartSpec> parts(genre->parts.begin(), genre->parts.end());
				Genre edited = *genre;
				edited.parts = parts;
				if (parts[p].flavour == Flavour::Sustain) {
					if (misfits) {
						parts[p].sustain.retriggerBars = unfit; // sustainValid()
						expect(genreValid(edited)).to_be_false();
						broken++;
					}
				}
				else if (parts[p].flavour == Flavour::Stab) {
					parts[p].stab.gateTicks = 0; // stabNotesValid()
					expect(genreValid(edited)).to_be_false();
					broken++;
				}
				parts[p] = genre->parts[p];
				if (misfits) {
					parts[p].drift.sweepBars = unfit; // sweepsValid()
					expect(genreValid(edited)).to_be_false();
				}
			}
		}
		expect(broken).to_be_greater_than(1);
		expect(rollCuts).to_equal(rolling); // one part holds a genre's roll lane; a genre may have none
	});

	it("names a palette its sounds are rolled from (spec §10.1)", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->palette != nullptr).to_be_true();
		}
	});

	it("lets a Bass or Lead note overlap only the next onset's: a plain note lasts a step at most (spec §8.5, §8.6)", _ {
		for (const Genre* genre : allGenres()) {
			expect(loopNotesValid(*genre)).to_be_true();
		}
		// A plain note lasts at most a step, wherever it leans.
		expect(loopValid({.slide = 0, .gateTicks = kTicksPerStep}, {.lean = {12, 0, -12, 0}})).to_be_true();
		// A plain note may last longer than a step when every note ends before the pattern's next onset, the pattern's
		// end counting as one (#221): before thinning, from its latest start to that onset's earliest.
		constexpr LoopSpec kSixSteps{.slide = 0, .gateTicks = 6 * kTicksPerStep};
		expect(loopValid(kSixSteps, {}, kLongNotes)).to_be_true();
		expect(loopValid({.slide = 0, .gateTicks = 6 * kTicksPerStep + 1}, {}, kLongNotes)).to_be_false();
		// A late Lean on the 1's note, or an early one on the & of 3's next, is counted.
		expect(loopValid(kSixSteps, {.lean = {0, 0, 1, 0}}, kLongNotes)).to_be_false();
		expect(loopValid(kSixSteps, {.lean = {-1, 0, 0, 0}}, kLongNotes)).to_be_false();
		expect(loopValid(kSixSteps, {.lean = {1, 0, 0, 0}}, kLongNotes)).to_be_true(); // a late 1 leaves more room
		// A 2-bar pattern's wrap gap counts too: its last note, 4 steps from the end, can't last 6.
		expect(loopValid(kSixSteps, {}, kTwoBarLongNotes)).to_be_false();
		expect(loopValid({.slide = 0, .gateTicks = 4 * kTicksPerStep}, {}, kTwoBarLongNotes)).to_be_true();
		expect(loopValid({.slide = 0, .gateTicks = 4 * kTicksPerStep + 1}, {}, kTwoBarLongNotes)).to_be_false();
		// Hits that ratchet fall within a step.
		constexpr Ornaments kRatchets{.rateFloor = 500, .rateCeiling = 500, .ratchet = {1, 0, 0}};
		expect(loopValid(kSixSteps, {.ornaments = kRatchets}, kLongNotes)).to_be_false();
		// A slid note leaned +12 on the 1 ends as the & starts with 12 ticks of overlap (12 + 24 + 12 = 48), and 13
		// reach past it. Lean past ±12 counts as ±12, as the Generator clamps it.
		expect(loopValid(sliding(12), {.lean = {12, 0, 0, 0}})).to_be_true();
		expect(loopValid(sliding(13), {.lean = {12, 0, 0, 0}})).to_be_false();
		expect(loopValid(sliding(12), {.lean = {20, 0, 0, 0}})).to_be_true();
		expect(loopValid(sliding(13), {.lean = {20, 0, 0, 0}})).to_be_false();
		// Lean that crosses 16ths: the & leaned +12 and the 1 after it −12, an overlap of 6 already reaches it.
		expect(loopValid(sliding(6), {.lean = {-12, 0, 12, 0}})).to_be_false();
		// A part that never slides has no slid notes to fit.
		expect(loopValid({.slide = 0}, {.lean = {-12, 0, 12, 0}})).to_be_true();
		// A flam's grace hit starts its gap before the & (6 − 6 = 0), so the note on the 1 must end by then.
		constexpr Ornaments kFlam{.rateFloor = 1000, .rateCeiling = 1000, .flam = 1, .flamGap = 6};
		expect(loopValid(sliding(18), {.lean = {12, 0, 6, 0}})).to_be_true();
		expect(loopValid(sliding(12), {.lean = {12, 0, 6, 0}, .ornaments = kFlam})).to_be_true();
		expect(loopValid(sliding(13), {.lean = {12, 0, 6, 0}, .ornaments = kFlam})).to_be_false();
		// A Section's first step clamps the 1's early Lean to the line (spec §8.5): a step plus 12 ticks from 0 ends as
		// the & leaned −12 starts.
		expect(loopValid(sliding(12), {.lean = {-12, 0, -12, 0}})).to_be_true();
		expect(loopValid(sliding(13), {.lean = {-12, 0, -12, 0}})).to_be_false();
		// A ratchet leaned late can end on the line, and a hit never starts before one on its key, so a part that may
		// ratchet starts its early-leaning notes on the line too.
		constexpr Ornaments kRatchet{.rateFloor = 500, .rateCeiling = 500, .ratchet = {1, 0, 0}};
		expect(loopValid(sliding(13), {.lean = {12, -12, 12, -12}})).to_be_true();
		expect(loopValid(sliding(12), {.lean = {12, -12, 12, -12}, .ornaments = kRatchet})).to_be_true();
		expect(loopValid(sliding(13), {.lean = {12, -12, 12, -12}, .ornaments = kRatchet})).to_be_false();
		// Without ratchets, an e leaned −12 after a 1 leaned +12 starts a tick after it, at −11.
		expect(loopValid(sliding(23), {.lean = {12, -12, 12, -12}})).to_be_true();
		expect(loopValid(sliding(24), {.lean = {12, -12, 12, -12}})).to_be_false();
	});

	it("gives each Arp part a walk per pool entry, a Register that holds its widest, and the note bound (spec §5.2, §7.3)", _ {
		// Every note ends by the time the onset after its next one starts, over the Arp's own onset gaps: arp_spec
		// covers the edges.
		for (const Genre* genre : allGenres()) {
			expect(arpNotesValid(*genre)).to_be_true();
		}
	});

	it("gives each Motif part chord positions to draw and the note bound over its own onset gaps (spec §5.2)", _ {
		// motif_spec covers the edges. No built-in genre plays a Motif before Detroit techno (#220), so the stand-in's
		// Motif is held to it too.
		for (const Genre* genre : allGenres()) {
			expect(motifNotesValid(*genre)).to_be_true();
		}
		expect(motifNotesValid(harness::kTechnoMotif)).to_be_true();
	});

	it("weighs the macro layer within ×0..×2", _ {
		for (const Genre* genre : allGenres()) {
			for (const auto& row : genre->soundWeight) {
				for (int16_t weight : row) {
					expect(weight >= 0 && weight <= 2000).to_be_true();
				}
			}
		}
	});

	it("gives each part gesture depths and a drift weight within ×0..×2, and Throw a hold", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->throwHoldSteps).to_be_greater_than(0);
			for (const PartSpec& part : genre->parts) {
				for (int16_t depth : part.gestures.depth) {
					expect(depth >= 0 && depth <= 2000).to_be_true();
				}
				expect(part.drift.weight >= 0 && part.drift.weight <= 2000).to_be_true();
			}
		}
	});

	it("gives every Sweep a period that divides the phrase or is a whole multiple of it (spec §11.5)", _ {
		for (const Genre* genre : allGenres()) {
			expect(sweepsValid(*genre)).to_be_true();
		}
	});

	it("rejects a Sweep period that neither divides the phrase nor is a whole multiple of it (sweepsValid())", _ {
		for (uint8_t bars : {uint8_t{3}, uint8_t{12}}) {
			const PartSpec parts[] = {{.role = Role::Bass, .drift = {.sweepBars = bars}}};
			expect(sweepsValid(Genre{.name = "bass", .phraseBars = 8, .parts = parts})).to_be_false();
		}
		for (uint8_t bars : {uint8_t{0}, uint8_t{2}, uint8_t{8}, uint8_t{16}}) {
			const PartSpec parts[] = {{.role = Role::Bass, .drift = {.sweepBars = bars}}};
			expect(sweepsValid(Genre{.name = "bass", .phraseBars = 8, .parts = parts})).to_be_true();
		}
	});

	it("gives every Sustain a retrigger period that fits the phrase, and anchors no Extension on a pedal (spec §5.2)", _ {
		for (const Genre* genre : allGenres()) {
			expect(sustainValid(*genre)).to_be_true();
		}
	});

	it("rejects a retrigger period that neither divides the phrase nor is a whole multiple of it", _ {
		for (uint8_t bars : {uint8_t{3}, uint8_t{12}}) {
			const PartSpec parts[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.retriggerBars = bars}}};
			expect(sustainValid(Genre{.name = "pad", .phraseBars = 8, .parts = parts})).to_be_false();
		}
		for (uint8_t bars : {uint8_t{0}, uint8_t{2}, uint8_t{8}, uint8_t{16}}) {
			const PartSpec parts[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.retriggerBars = bars}}};
			expect(sustainValid(Genre{.name = "pad", .phraseBars = 8, .parts = parts})).to_be_true();
		}
	});

	it("rejects an anchored Extension on a pedal, which voices none", _ {
		const PartSpec pedal[] = {
		    {.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.pedal = true, .anchorExtension = true}}};
		expect(sustainValid(Genre{.name = "pedal", .parts = pedal})).to_be_false();
		const PartSpec chords[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.anchorExtension = true}}};
		expect(sustainValid(Genre{.name = "chords", .parts = chords})).to_be_true();
	});

	it("rejects an open voicing on a part that isn't a Sustain, on a pedal, which voices no chord, or with a doubled root", _ {
		const PartSpec stab[] = {{.role = Role::Chords, .flavour = Flavour::Stab, .sustain = {.openVoicing = true}}};
		expect(sustainValid(Genre{.name = "stab", .parts = stab})).to_be_false();
		const PartSpec pedal[] = {
		    {.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.pedal = true, .openVoicing = true}}};
		expect(sustainValid(Genre{.name = "pedal", .parts = pedal})).to_be_false();
		const PartSpec doubled[] = {
		    {.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.doubleRoot = true, .openVoicing = true}}};
		expect(sustainValid(Genre{.name = "doubled", .parts = doubled})).to_be_false();
		const PartSpec pad[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.openVoicing = true}}};
		expect(sustainValid(Genre{.name = "pad", .parts = pad})).to_be_true();
	});

	it("rejects a hold across Section lines on a part that isn't a Sustain, which only decideSustain() honours", _ {
		const PartSpec stab[] = {
		    {.role = Role::Chords, .flavour = Flavour::Stab, .sustain = {.holdsAcrossSections = true}}};
		expect(sustainValid(Genre{.name = "stab", .parts = stab})).to_be_false();
		const PartSpec pad[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.holdsAcrossSections = true}}};
		expect(sustainValid(Genre{.name = "pad", .parts = pad})).to_be_true();
	});

	it("rejects held common tones on a part that isn't a Sustain, which only decideSustain() honours", _ {
		const PartSpec stab[] = {{.role = Role::Chords, .flavour = Flavour::Stab, .sustain = {.holdsCommonTones = true}}};
		expect(sustainValid(Genre{.name = "stab", .parts = stab})).to_be_false();
		const PartSpec pad[] = {{.role = Role::Pad, .flavour = Flavour::Sustain, .sustain = {.holdsCommonTones = true}}};
		expect(sustainValid(Genre{.name = "pad", .parts = pad})).to_be_true();
	});

	it("gives each part a flavour its role allows, which its sounding-note table is sized for (spec §5.2, §2.7)", _ {
		// Drums and FX have none; Bass plays Loop; Lead Loop, Arp or Motif; Chords Stab; Pad Sustain.
		const std::map<Role, std::set<Flavour>> allowed = {
		    {Role::Drums, {Flavour::None}},
		    {Role::FX, {Flavour::None}},
		    {Role::Bass, {Flavour::Loop}},
		    {Role::Lead, {Flavour::Loop, Flavour::Arp, Flavour::Motif}},
		    {Role::Chords, {Flavour::Stab}},
		    {Role::Pad, {Flavour::Sustain}},
		};
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				expect(allowed.at(part.role).count(part.flavour)).to_equal(size_t{1});
			}
		}
	});

	it("gives each Stab part a pool to play and a gate of at least a tick (spec §5.2, §8.1)", _ {
		int32_t stabs = 0;
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				stabs += part.flavour == Flavour::Stab ? 1 : 0;
			}
			expect(stabNotesValid(*genre)).to_be_true();
		}
		expect(stabs).to_be_greater_than(1); // Dub techno's and Detroit techno's, at least
		static constexpr Pattern kOne[] = {{"..X.............", "................", 0}};
		const auto valid = [](const PartSpec& stab) {
			const PartSpec parts[] = {stab};
			return stabNotesValid(Genre{.name = "stab", .parts = parts});
		};
		expect(valid({.role = Role::Chords, .flavour = Flavour::Stab, .pool = kOne})).to_be_true();
		expect(valid({.role = Role::Chords, .flavour = Flavour::Stab})).to_be_false();
		expect(valid({.role = Role::Chords, .flavour = Flavour::Stab, .pool = kOne, .stab = {.gateTicks = 0}}))
		    .to_be_false();
	});

	it("keeps its Extension floor at or below its ceiling, and the ceiling at 2 or below", _ {
		for (const Genre* genre : allGenres()) {
			expect(genre->harmony.extensionFloor).to_be_less_than(genre->harmony.extensionCeiling + 1);
			expect(genre->harmony.extensionCeiling).to_be_less_than(uint8_t{3});
		}
	});

	it("gives every Chord graph a harmonic rhythm that fits the phrase, and moves between degrees (spec §7.4)", _ {
		for (const Genre* genre : allGenres()) {
			expect(harmonyValid(*genre)).to_be_true();
		}
	});

	it("rejects a harmonic rhythm that neither divides the phrase nor is a whole multiple of it", _ {
		constexpr ChordMove moves[] = {{0, 4, 10}};
		auto fits = [&](uint8_t bars, uint8_t phraseBars) {
			const ChordGraph graph{.moves = moves, .cadenceTarget = 4, .barsPerChord = bars};
			return harmonyValid(Genre{.phraseBars = phraseBars, .harmony = {.graph = &graph}});
		};
		for (uint8_t bars : {1, 2, 4, 8, 16, 24, 32}) {
			expect(fits(bars, 8)).to_be_true();
		}
		for (uint8_t bars : {0, 3, 5, 6, 12, 20}) {
			expect(fits(bars, 8)).to_be_false();
		}
		expect(fits(6, 12)).to_be_true();
		expect(fits(36, 12)).to_be_true();
		expect(fits(8, 12)).to_be_false();
	});

	it("rejects a looping Chord graph whose half-phrase loop doesn't hold 2 whole chords, or whose chances are off 0-1000 (spec §7.4)", _ {
		constexpr ChordMove moves[] = {{0, 4, 10}};
		auto valid = [&](uint8_t bars, uint8_t phraseBars, int16_t loop, int16_t half, int16_t back) {
			const ChordGraph graph{.moves = moves,
			                       .cadenceTarget = 4,
			                       .barsPerChord = bars,
			                       .loopChance = loop,
			                       .halfLoopChance = half,
			                       .loopReturnChance = back};
			return harmonyValid(Genre{.phraseBars = phraseBars, .harmony = {.graph = &graph}});
		};
		expect(valid(2, 8, 500, 333, 333)).to_be_true();
		expect(valid(1, 8, 0, 0, 0)).to_be_true();
		expect(valid(4, 16, 1000, 1000, 1000)).to_be_true();
		expect(valid(4, 8, 500, 333, 333)).to_be_false();  // a half-phrase loop of one chord
		expect(valid(8, 8, 500, 0, 0)).to_be_false();      // a phrase-long chord can't loop
		expect(valid(16, 8, 500, 333, 333)).to_be_false(); // nor one longer than the phrase
		expect(valid(2, 6, 500, 333, 333)).to_be_false();  // half of 6 bars isn't whole 2-bar chords
		expect(valid(2, 8, 1001, 333, 333)).to_be_false();
		expect(valid(2, 8, 500, -1, 333)).to_be_false();
		expect(valid(2, 8, 500, 333, 1001)).to_be_false();
		expect(valid(4, 8, -1, 333, 333)).to_be_true(); // no loop: the walk as before
	});

	it("rejects an Extension order that doesn't try each Extension once, or tries the 6 before the 7 (spec §7.4)", _ {
		auto valid = [](ExtensionOrder order) { return harmonyValid(Genre{.harmony = {.extensionOrder = order}}); };
		using enum Extension;
		expect(valid(ExtensionOrder{})).to_be_true();
		expect(valid(kNinthFirst)).to_be_true();
		expect(valid(ExtensionOrder{{Ninth, Seventh, Sixth, Sus4}})).to_be_true();
		expect(valid(ExtensionOrder{{Ninth, Ninth, Seventh, Sixth}})).to_be_false(); // the sus4 never tried
		expect(valid(ExtensionOrder{{Sixth, Seventh, Ninth, Sus4}})).to_be_false();  // the 6 before the 7 never plays
		expect(valid(ExtensionOrder{{Seventh, Ninth, Sus4, static_cast<Extension>(4)}})).to_be_false(); // the 5th
	});

	it("rejects a Chord graph with a move off the scale's 7 degrees, too many moves, or a cadence target off them", _ {
		auto valid = [](std::span<const ChordMove> moves, uint8_t target, uint8_t period) {
			const ChordGraph graph{.moves = moves, .cadenceTarget = target, .cadencePeriod = period};
			return harmonyValid(Genre{.harmony = {.graph = &graph}});
		};
		constexpr ChordMove good[] = {{0, 6, 10}, {6, 0, 10}};
		constexpr ChordMove offScale[] = {{0, 7, 10}};
		constexpr ChordMove toItself[] = {{3, 3, 10}};
		constexpr ChordMove nine[] = {{0, 1, 1}, {0, 2, 1}, {0, 3, 1}, {0, 4, 1}, {0, 5, 1},
		                              {0, 6, 1}, {1, 0, 1}, {2, 0, 1}, {3, 0, 1}};
		expect(valid(good, 6, 4)).to_be_true();
		expect(valid(good, 6, 0)).to_be_true(); // no regular cadence: only the Drop cadence leans to the target
		expect(valid(offScale, 6, 4)).to_be_false();
		expect(valid(toItself, 6, 4)).to_be_false(); // a repeat is the repeat weight
		expect(valid(nine, 6, 4)).to_be_false();
		expect(valid(good, 0, 4)).to_be_true();                // home may be the target
		expect(valid(good, kNoCadenceTarget, 4)).to_be_true(); // or there is none
		expect(valid(good, 7, 4)).to_be_false();
	});

	it("only has Build lengths that land the Drop on a phrase line", _ {
		// 1, 2, 4, 8, 16 or 32 bars, each dividing the phrase or a whole number of phrases (spec §9.3).
		for (const Genre* genre : allGenres()) {
			for (const WeightedBars& build : genre->arc.buildBars) {
				expect(build.weight).to_be_greater_than(uint8_t{0});
			}
			expect(buildBarsValid(*genre)).to_be_true();
			expect(genre->arc.groovePhrases.low).to_be_greater_than(uint8_t{0});
			expect(genre->arc.breakPhrases.low).to_be_greater_than(uint8_t{0});
		}
	});

	it("rejects a Build length that isn't 1 to 32 bars by doubling or doesn't land its Drop on a phrase line "
	   "(buildBarsValid())", _ {
		// 24 and 64 bars are whole numbers of 8-bar phrases, but no Build length.
		for (uint8_t bars : {uint8_t{0}, uint8_t{3}, uint8_t{12}, uint8_t{24}, uint8_t{64}}) {
			const WeightedBars builds[] = {{4, 1}, {bars, 1}};
			expect(buildBarsValid(Genre{.name = "builds", .phraseBars = 8, .arc = {.buildBars = builds}})).to_be_false();
		}
		const WeightedBars fitting[] = {{1, 1}, {2, 1}, {4, 1}, {8, 1}, {16, 1}, {32, 1}};
		expect(buildBarsValid(Genre{.name = "builds", .phraseBars = 8, .arc = {.buildBars = fitting}})).to_be_true();
	});

	it("ends a Build with a cut and a Gap each shorter than its shortest Build and its phrase (spec §9.1, §9.5)", _ {
		for (const Genre* genre : allGenres()) {
			expect(cutAndGapUnderBuild(*genre)).to_be_true();
		}
	});

	it("rejects a Build cut or Gap that covers a whole phrase or the shortest Build (cutAndGapUnderBuild())", _ {
		static constexpr WeightedBars kEight[] = {{8, 1}};
		static constexpr WeightedBars kOneOrEight[] = {{8, 2}, {1, 1}};
		static constexpr WeightedBars kTwo[] = {{2, 1}};
		static constexpr WeightedBars kSixteen[] = {{16, 1}};
		const auto ends = [](uint8_t phraseBars, std::span<const WeightedBars> builds, uint8_t cutBars,
		                     GapLength gap) {
			return cutAndGapUnderBuild(Genre{.name = "ends",
			                                 .phraseBars = phraseBars,
			                                 .arc = {.buildBars = builds, .buildCutBars = cutBars, .gap = gap}});
		};
		expect(ends(8, kEight, 7, GapLength::Bar)).to_be_true();
		expect(ends(8, kEight, 8, GapLength::None)).to_be_false();
		expect(ends(8, kEight, 9, GapLength::None)).to_be_false();
		expect(ends(2, kTwo, 1, GapLength::Bar)).to_be_true();
		expect(ends(1, kOneOrEight, 0, GapLength::Beat)).to_be_true();
		expect(ends(1, kOneOrEight, 0, GapLength::Bar)).to_be_false();
		expect(ends(1, kOneOrEight, 1, GapLength::None)).to_be_false();
		// A 1-bar Build in an 8-bar phrase: a 1-bar cut or Gap would cover all of it (spec §9.5).
		expect(ends(8, kOneOrEight, 1, GapLength::None)).to_be_false();
		expect(ends(8, kOneOrEight, 0, GapLength::Bar)).to_be_false();
		expect(ends(8, kOneOrEight, 0, GapLength::Beat)).to_be_true();
		expect(ends(8, kTwo, 1, GapLength::Bar)).to_be_true();
		expect(ends(8, kTwo, 2, GapLength::None)).to_be_false();
		// A Build longer than the phrase: the phrase still bounds them, as the Arc plan leaves them out of it.
		expect(ends(8, kSixteen, 8, GapLength::None)).to_be_false();
		expect(ends(8, kSixteen, 7, GapLength::Bar)).to_be_true();
	});

	it("holds a Siren's note 1 bar up to a phrase, and no other lane's (sirensValid(), spec §9.5)", _ {
		for (const Genre* genre : allGenres()) {
			expect(sirensValid(*genre)).to_be_true();
		}
		const auto valid = [](uint8_t sirenBars, uint8_t fallBars, Role role) {
			const LaneSpec lanes[] = {{.lane = Lane::Fall, .heldBars = fallBars},
			                          {.lane = Lane::Siren, .heldBars = sirenBars}};
			const PartSpec parts[] = {{.role = role, .lanes = lanes}};
			return sirensValid(Genre{.name = "sirens", .phraseBars = 8, .parts = parts});
		};
		expect(valid(2, 0, Role::FX)).to_be_true();
		expect(valid(8, 0, Role::FX)).to_be_true();
		expect(valid(0, 0, Role::FX)).to_be_false();
		expect(valid(9, 0, Role::FX)).to_be_false();
		expect(valid(2, 1, Role::FX)).to_be_false();
		expect(valid(2, 0, Role::Drums)).to_be_false();
	});

	it("keeps every pitched anchor a MIDI note two octaves either way (registersShiftValid(), spec §7.3)", _ {
		for (const Genre* genre : allGenres()) {
			expect(registersShiftValid(*genre)).to_be_true();
		}
		const auto valid = [](uint8_t anchor, Role role) {
			const PartSpec parts[] = {{.role = role, .reg = {anchor, -5, 12}}};
			return registersShiftValid(Genre{.name = "registers", .parts = parts});
		};
		expect(valid(24, Role::Bass)).to_be_true();
		expect(valid(103, Role::Lead)).to_be_true();
		expect(valid(23, Role::Bass)).to_be_false();
		expect(valid(104, Role::Lead)).to_be_false();
		expect(valid(10, Role::Drums)).to_be_true(); // no Register: the anchor is never read
	});

	it("has a phrase whose every Glide length fits a SetTempo or SetSwing command's 16 bits (spec §13.3)", _ {
		using deluge::generative::perform::GlideLength;
		using deluge::generative::perform::glideTicks;
		for (const Genre* genre : allGenres()) {
			for (GlideLength length : {GlideLength::Instant, GlideLength::Beat, GlideLength::Bar, GlideLength::TwoBars,
			                           GlideLength::FourBars, GlideLength::Phrase}) {
				expect(glideTicks(length, genre->phraseSteps())).to_be_less_than(int32_t{UINT16_MAX} + 1);
			}
		}
	});

	it("names section-rule lanes its roster has, and keeps the roll lane out of the Build cut", _ {
		for (const Genre* genre : allGenres()) {
			auto inRoster = [&](Lane lane) {
				for (const PartSpec& part : genre->parts) {
					for (const LaneSpec& spec : part.lanes) {
						if (spec.lane == lane) {
							return true;
						}
					}
				}
				return false;
			};
			const ArcSpec& arc = genre->arc;
			expect(arc.roll.lane == Lane::None || inRoster(arc.roll.lane)).to_be_true();
			expect(arc.accentLane == Lane::None || inRoster(arc.accentLane)).to_be_true();
			expect(arc.roll.top == RollStage::Sixteenth || arc.roll.top == RollStage::ThirtySecond
			       || arc.roll.top == RollStage::SixtyFourth)
			    .to_be_true();
		}
	});

	it("only has fill entries a fill can play: lengths, weights, masks and a held-fill entry (spec §8.7)", _ {
		for (const Genre* genre : allGenres()) {
			const auto& fills = genre->fills;
			expect(fills.empty() || genre->heldFill < fills.size()).to_be_true();
			expect(genre->fillChance).to_be_between(int16_t{0}, int16_t{1000});
			expect(genre->sectionEndChance == kSameAsFillChance
			       || (genre->sectionEndChance >= 0 && genre->sectionEndChance <= 1000))
			    .to_be_true();
			for (const FillEntry& fill : fills) {
				expect(fill.beats == 1 || fill.beats == 2 || fill.beats == 4).to_be_true();
				expect(fill.weight).to_be_greater_than(uint8_t{0});
				// Shorter than a phrase, so it never covers the phrase's first step.
				expect(fill.beats * kStepsPerBeat).to_be_less_than(genre->phraseSteps());
				expect(fill.mask.parts != 0 || fill.mask.lanes != 0).to_be_true();
				for (int32_t p = 0; p < kMaxParts; p++) {
					const bool named = (fill.mask.parts & partBit(p)) != 0;
					// Whole parts only in a Rest, and only pitched ones: Bass, Chords, Pad or Lead.
					expect(named && (fill.kind != FillKind::Rest || p >= static_cast<int32_t>(genre->parts.size())
					                 || !genre->parts[p].lanes.empty()))
					    .to_be_false();
				}
				for (int32_t l = 0; l < kNumLanes; l++) {
					const auto lane = static_cast<Lane>(l);
					if ((fill.mask.lanes & laneBit(lane)) == 0) {
						continue;
					}
					expect(isFxLane(lane)).to_be_false(); // section rules own FX lanes
					// A masked lane is in the roster, and a Swap's has fill patterns to play.
					const LaneSpec* found = nullptr;
					for (const PartSpec& part : genre->parts) {
						for (const LaneSpec& spec : part.lanes) {
							found = spec.lane == lane ? &spec : found;
						}
					}
					expect(found != nullptr).to_be_true();
					expect(fill.kind == FillKind::Swap && found != nullptr && found->fillPool.empty()).to_be_false();
				}
			}
		}
	});

	it("fits its Drop decay in its shortest Groove at Motion 50, but for Psytrance's named exception", _ {
		for (const Genre* genre : allGenres()) {
			// Spec §9.3's named exception (#349): Psytrance's 2-phrase decay keeps its Lead (gate 30) in for a Drop's
			// whole first phrase. In a Groove shorter than the decay the lift stops at the next Section
			// (psytrance_spec shows it).
			if (genre == &genres::psytranceClassic) {
				continue;
			}
			// The first phrase line where phrases × the Groove multiple at Motion 50 reaches the shortest base, less
			// the bars a Build shorter than a phrase takes from it after Groove → Build (spec §9.3).
			int64_t multiple = motionMultiple(curves::grooveLength, kMacroFull);
			int32_t phrases = 1;
			while (phrases * multiple < int64_t{genre->arc.groovePhrases.low} * kCurveOne) {
				phrases++;
			}
			int32_t shortestBars = phrases * genre->phraseBars;
			if (genre->arc.grooveToBuild > 0) {
				int32_t taken = 0;
				for (const WeightedBars& build : genre->arc.buildBars) {
					taken = build.bars < genre->phraseBars && build.bars > taken ? build.bars : taken;
				}
				shortestBars -= taken;
			}
			expect(genre->arc.dropDecayPhrases * genre->phraseBars).to_be_less_than(shortestBars + 1);
		}
	});

	it("keeps an off-beat hit in the top half of every tier 1+ pattern's importances", _ {
		for (const Pattern& pattern : kSyncopated) {
			expect(keepsSyncopation(pattern)).to_be_true();
		}
		for (const Pattern& pattern : kStraight) {
			expect(keepsSyncopation(pattern)).to_be_false();
		}
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					if (pattern.tier >= 1) {
						expect(keepsSyncopation(pattern)).to_be_true();
					}
				}
			}
		}
	});

	it("plays every tier 1+ pattern's off-beat hit at the genre's default Density, whatever its jitter", _ {
		for (const Genre* genre : allGenres()) {
			int32_t threshold = genre->densityCurve.threshold(genre->defaults[Macro::Density]);
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					int32_t offBeat = strongestOffBeat(pattern);
					if (pattern.tier >= 1 && offBeat != kAnchor) {
						expect(offBeat * 100 - genre->densityJitter).to_be_greater_than(threshold - 1);
					}
				}
			}
		}
	});

	it("plays the off-beat hit that carries the authoring rule above ghost level", _ {
		// Ghosts are low-importance steps (spec §8.1): the hit that keeps a pattern's syncopation isn't one.
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : pools(*genre)) {
				for (const Pattern& pattern : pool) {
					int32_t offBeat = strongestOffBeat(pattern);
					if (pattern.tier == 0 || offBeat == 0) {
						continue;
					}
					bool audible = false;
					for (int32_t i = 0; i < pattern.length; i++) {
						audible = audible
						          || (i % kStepsPerBeat != 0 && pattern.importance[i] == offBeat
						              && pattern.level[i] != VelocityLevel::Ghost);
					}
					expect(audible).to_be_true();
				}
			}
		}
	});

	it("lists every pool and fill pool by tier, lowest first, with every entry weighted above 0", _ {
		for (const Genre* genre : allGenres()) {
			for (std::span<const Pattern> pool : poolsAndFillPools(*genre)) {
				for (size_t i = 0; i < pool.size(); i++) {
					expect(pool[i].weight).to_be_greater_than(uint8_t{0});
					if (i > 0) {
						expect(pool[i].tier).to_be_greater_than(pool[i - 1].tier - 1);
					}
				}
			}
		}
	});

	it("has a pool and fill pool entry at or below every band's lowest tier", _ {
		for (const Genre* genre : allGenres()) {
			for (const PartSpec& part : genre->parts) {
				std::vector<std::pair<std::span<const Pattern>, TierBand>> banded;
				if (!part.pool.empty()) {
					banded.emplace_back(part.pool, part.band);
				}
				for (const LaneSpec& lane : part.lanes) {
					if (!lane.pool.empty()) { // FX lanes have no pool: section rules play them (spec §5.4)
						banded.emplace_back(lane.pool, lane.band);
					}
					if (!lane.fillPool.empty()) {
						banded.emplace_back(lane.fillPool, lane.band);
					}
				}
				for (const auto& [pool, band] : banded) {
					expect(band.high).to_be_less_than(kMaxTier + 1);
					bool covered = false;
					for (const Pattern& pattern : pool) {
						covered = covered || pattern.tier <= band.low;
					}
					expect(covered).to_be_true();
				}
			}
		}
	});

	it("keeps Lean within ±12 ticks, flam gaps at 2–6 ticks and Ornament rates within 0–1000, floor to ceiling", _ {
		for (const Genre* genre : allGenres()) {
			std::vector<const Expression*> expressions;
			for (const PartSpec& part : genre->parts) {
				expressions.push_back(&part.expression);
				for (const LaneSpec& lane : part.lanes) {
					expressions.push_back(&lane.expression);
				}
			}
			for (const Expression* expression : expressions) {
				for (int8_t lean : expression->lean) {
					expect(static_cast<int32_t>(lean)).to_be_between(-kMaxLean, kMaxLean);
				}
				const Ornaments& ornaments = expression->ornaments;
				expect(ornaments.flamGap).to_be_between(uint8_t{2}, uint8_t{6});
				expect(ornaments.rateFloor).to_be_between(int16_t{0}, ornaments.rateCeiling);
				expect(ornaments.rateCeiling).to_be_less_than(int16_t{1001});
			}
		}
	});

	it("only has interplay rules that read earlier parts and lanes", _ {
		for (const Genre* genre : allGenres()) {
			expect(interplayRulesValid(*genre)).to_be_true();
		}
	});

	it("anchors no Avoid source on a step its target can play, and lists Avoid on no Sustain nor a source that may flam", _ {
		for (const Genre* genre : allGenres()) {
			expect(avoidRulesValid(*genre)).to_be_true();
		}
	});

	it("plays each Roll fill on one Drums lane", _ {
		for (const Genre* genre : allGenres()) {
			expect(rollFillsValid(*genre)).to_be_true();
		}
	});

	it("thins to the anchors alone at Density 0 and lets every step through at full Density", _ {
		for (const Genre* genre : allGenres()) {
			const DensityCurve& curve = genre->densityCurve;
			expect(genre->densityJitter).to_be_between(int16_t{0}, int16_t{99});
			// The top non-anchor importance, jittered up, stays below the threshold at Density 0.
			expect(curve.threshold(0)).to_be_greater_than(900 + genre->densityJitter);
			// The lowest, jittered down, clears it at full Density.
			expect(curve.threshold(kMacroFull)).to_be_less_than(100 - genre->densityJitter + 1);
			for (int32_t density = 1; density <= kMacroFull; density++) {
				expect(curve.threshold(density)).to_be_less_than(curve.threshold(density - 1) + 1);
			}
		}
	});
	context("drop-out entries (dropOutsValid(), spec §8.8)", _ {
		// Acid techno's roster: Drums, Bass, Drone (a Pad) and FX.
		constexpr int32_t kAcidDrums = 0;
		constexpr int32_t kAcidBass = 1;
		constexpr int32_t kAcidDrone = 2;
		const auto withEntries = [](std::span<const DropOutEntry> entries) {
			Genre genre = genres::technoAcid;
			genre.dropOuts = entries;
			return genre;
		};
		const PartsAndLanes bass{.parts = partBit(kAcidBass)};
		const PartsAndLanes drone{.parts = partBit(kAcidDrone)};
		const PartsAndLanes pitched{.parts = static_cast<uint8_t>(partBit(kAcidBass) | partBit(kAcidDrone))};
		/// Acid's Drums lanes, those with a pool or every one.
		const auto drumLanes = [](bool pooledOnly) {
			uint16_t lanes = 0;
			for (const LaneSpec& lane : genres::technoAcid.parts[kAcidDrums].lanes) {
				lanes |= pooledOnly && lane.pool.empty() ? 0 : laneBit(lane.lane);
			}
			return lanes;
		};

		it("holds every built-in genre", _ {
			for (const Genre* genre : allGenres()) {
				expect(dropOutsValid(*genre)).to_be_true();
			}
		});

		it("refuses an entry that silences every pitched part and every Drums lane with a pool", _ {
			const DropOutEntry everything[] = {{.mask = {.parts = pitched.parts, .lanes = drumLanes(true)}}};
			// Pool-less lanes don't count: dropping them too still silences everything.
			const DropOutEntry withPoolless[] = {{.mask = {.parts = pitched.parts, .lanes = drumLanes(false)}}};
			const DropOutEntry leavesTheDrone[] = {{.mask = {.parts = bass.parts, .lanes = drumLanes(false)}}};
			const DropOutEntry leavesTheKick[] = {
			    {.mask = {.parts = pitched.parts,
			              .lanes = static_cast<uint16_t>(drumLanes(true) & ~laneBit(Lane::Kick))}}};
			expect(dropOutsValid(withEntries(everything))).to_be_false();
			expect(dropOutsValid(withEntries(withPoolless))).to_be_false();
			expect(dropOutsValid(withEntries(leavesTheDrone))).to_be_true();
			expect(dropOutsValid(withEntries(leavesTheKick))).to_be_true();
			// genreValid() holds it.
			expect(genreValid(withEntries(everything))).to_be_false();
			expect(genreValid(withEntries(leavesTheDrone))).to_be_true();
		});

		it("refuses a length but 1, 2, 4 or 8 bars or one past the phrase, an empty mask, an FX lane, a whole Kit, a "
		   "target outside the mask or more than one", _ {
			for (uint8_t bars : {1, 2, 4, 8}) {
				const DropOutEntry ok[] = {{.mask = bass, .bars = bars, .throwTarget = bass}};
				expect(dropOutsValid(withEntries(ok))).to_be_true();
			}
			{
				const DropOutEntry eight[] = {{.mask = bass, .bars = 8}};
				Genre shortPhrases = withEntries(eight);
				shortPhrases.phraseBars = 4;
				expect(dropOutsValid(shortPhrases)).to_be_false();
			}
			for (uint8_t bars : {0, 3, 6, 16}) {
				const DropOutEntry wrong[] = {{.mask = bass, .bars = bars}};
				expect(dropOutsValid(withEntries(wrong))).to_be_false();
			}
			const DropOutEntry empty[] = {{.mask = {}}};
			const DropOutEntry fxLane[] = {{.mask = {.lanes = laneBit(Lane::Riser)}}};
			const DropOutEntry wholeKit[] = {{.mask = {.parts = partBit(kAcidDrums)}}};
			const DropOutEntry targetOutside[] = {{.mask = bass, .throwTarget = drone}};
			const DropOutEntry twoTargets[] = {{.mask = pitched, .throwTarget = pitched}};
			const DropOutEntry laneAndPart[] = {
			    {.mask = {.parts = bass.parts, .lanes = laneBit(Lane::Kick)},
			     .throwTarget = {.parts = bass.parts, .lanes = laneBit(Lane::Kick)}}};
			for (std::span<const DropOutEntry> entries :
			     {std::span<const DropOutEntry>(empty), std::span<const DropOutEntry>(fxLane),
			      std::span<const DropOutEntry>(wholeKit),
			      std::span<const DropOutEntry>(targetOutside), std::span<const DropOutEntry>(twoTargets),
			      std::span<const DropOutEntry>(laneAndPart)}) {
				expect(dropOutsValid(withEntries(entries))).to_be_false();
			}
		});

		it("refuses an entry longer than the phrase, which the Arc plan's leaving it out relies on", _ {
			const DropOutEntry fourBars[] = {{.mask = bass, .bars = 4}};
			Genre genre = withEntries(fourBars);
			expect(dropOutsValid(genre)).to_be_true();
			genre.phraseBars = 4;
			expect(dropOutsValid(genre)).to_be_true();
			genre.phraseBars = 2;
			expect(dropOutsValid(genre)).to_be_false();
		});

		it("refuses a gap under a bar and a chance outside 0 to 1000", _ {
			const DropOutEntry entries[] = {{.mask = bass}};
			Genre genre = withEntries(entries);
			expect(dropOutsValid(genre)).to_be_true();
			genre.dropOutGapBars = 0;
			expect(dropOutsValid(genre)).to_be_false();
			genre.dropOutGapBars = 1;
			genre.dropOutChance = 1001;
			expect(dropOutsValid(genre)).to_be_false();
			genre.dropOutChance = -1;
			expect(dropOutsValid(genre)).to_be_false();
		});
	});
});

CPPSPEC_SPEC(genre_data)
