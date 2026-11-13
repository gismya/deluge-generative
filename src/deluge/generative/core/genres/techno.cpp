/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Techno (spec §4.5, ADR 0006): a parent genre, never played, and its subgenres. For now the parent holds all of
// today's Techno, 125–135 BPM, Minor or Phrygian, straight: a four-on-the-floor kick, offbeat hats and a mutating acid
// line, no claps on 2 and 4. Every value is starting data, tuned by ear later (spec §21.5).
//
// Two of its four subgenres so far (#218–#221): `techno`, today's Techno under its old name (#218 renames it
// techno-acid and moves what only Acid plays out of the parent), and Melodic techno's first cut (#226), which #219
// gives its Pad and Arp.
//
// Filled so far: a Drums part (Kick, Rim, Closed hat, Open hat, and Snare and Tom for fills) and a Loop Bass, each
// with a small tiered pool (two entries in the tiers the default Complexity reaches, so picks and takes have a choice)
// and expression settings (Lean, Velocity spread, Ornaments), the hats' interplay rule, an FX part (Riser, Fall,
// Impact), the palette (techno_palette.cpp), fills (a kick and acid drop, and a snare and tom run) and the section
// rules: Breaks and Builds keep the kick, rim and closed hat, a snare roll, the kick cut for the Build's last bar, a
// beat's Gap, and a Crash accent on Drops and after fills. Later M1 slices fill the rest.

#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
constexpr Pattern kKick[] = {
    {"X...X...X...X...", "................", 0},
    {"X...X..2X...X..7", ".......g........", 1, 2},
    {"X...X...X..7X...", "................", 1},
    {"X...X...X...X.8.", "................", 2},
    {"X..7X...X..2X.8.", "...........g....", 2},
};

constexpr Pattern kRim[] = {
    {".......3......2.", ".......g......g.", 0},
    {"...3...4..2...7.", "...g...g..g.....", 1},
    {"......5...3..7..", "......g...g.....", 1},
};

constexpr Pattern kClosedHat[] = {
    {"..8...8...8...8.", "................", 0, 2},
    {"..8...8...8...83", "...............g", 0},
    {"2584258425842584", "gg.ggg.ggg.ggg.g", 1},
    {"..8.3.8...8.4.8.", "....g.......g...", 1},
    {"3795379537953796", "gg.ggg.ggg.ggg.g", 2},
};

constexpr Pattern kOpenHat[] = {
    {"......6.......6.", "................", 0, 2},
    {"..............7.", "................", 0},
    {"..5...7...5...7.", "..g.......g.....", 1},
};

// Fill patterns: only a Swap plays them, over the last beats of a phrase, so their hits sit at the end of the bar.
constexpr Pattern kSnareFill[] = {
    {"........7.8.X.XX", "........g.......", 0},
    {"........6878X8XX", "........g.g.....", 1},
};

constexpr Pattern kTomFill[] = {
    {"..........8.X.X.", "................", 0},
    {"........7.8.X8XX", "........g.....a.", 1},
};

constexpr Pattern kBass[] = {
    {"2.8.2.8.2.8.2.8.", "................", 0},
    {"2.8.5.8.2.8.6.8.", "........g.......", 1, 2},
    {"..8.4.8...8.5.86", "....g.......g...", 1},
    {"3584659537846597", ".g.g.g.g...g.g..", 2},
};
// clang-format on

/// The hats keep to the & steps between four-on-the-floor kicks, less so as Complexity rises (spec §5.5).
constexpr InterplayRule kHatsOffKick[] = {
    {.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .offBeat = true, .strength = 90, .sensitivity = -40},
};

/// In the fixed lane order (spec §5.4). Straight time: the & hats push a little ahead and the e and a sit a touch
/// late. The hats ratchet and the rim flams more as Complexity rises.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 2}},
    {.lane = Lane::Snare, .fillPool = kSnareFill, .band = {0, 1}}, // the Build roll's lane, and a Swap's
    {
        .lane = Lane::Rim,
        .pool = kRim,
        .band = {0, 1},
        .expression = {.spread = 10,
                       .spreadMode = SpreadMode::Fresh,
                       .ornaments = {.rateFloor = 0, .rateCeiling = 200, .flam = 1, .flamGap = 3}},
    },
    {
        .lane = Lane::ClosedHat,
        .pool = kClosedHat,
        .band = {0, 2},
        .rules = kHatsOffKick,
        .expression = {.lean = {0, 1, -2, 1},
                       .spread = 12,
                       .ornaments = {.rateFloor = 0, .rateCeiling = 150, .ratchet = {3, 1, 0}}},
    },
    {
        .lane = Lane::OpenHat,
        .pool = kOpenHat,
        .band = {0, 1},
        .rules = kHatsOffKick,
        .expression = {.lean = {0, 0, -2, 0}, .spread = 6},
    },
    {.lane = Lane::Crash},                                     // the Phrase accent's lane
    {.lane = Lane::Tom, .fillPool = kTomFill, .band = {0, 1}}, // only a Swap plays it
};

constexpr DegreeWeight kBassDegrees[] = {{0, 6}, {2, 1}, {3, 1}, {4, 2}, {6, 2}};

/// Octave and fifth Answers at low Complexity, reverse at the default, rotate at the top (spec §5.2).
constexpr Answer kBassAnswers[] = {
    {AnswerKind::Octave, 3}, {AnswerKind::Fifth}, {AnswerKind::Reverse}, {AnswerKind::Rotate}};

/// The acid line: mostly the root, leaping wider, jumping octaves and sliding more as Complexity rises. At the
/// default Complexity (25) it rolls as it did before Complexity shaped it.
constexpr LoopSpec kBassLoop{
    .degrees = kBassDegrees,
    .leap = {2, 10},
    .octaveUp = {100, 200},
    .octaveDown = {60, 140},
    .accent = 250,
    .slide = {120, 280},
    .mutation = 150,
    .answers = kBassAnswers,
    .answerBars = 4,
};

/// FX lanes have no pools: only section rules play them (spec §5.4).
constexpr LaneSpec kFxLanes[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};

/// Techno's Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};

constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "ACID",
        .pool = kBass,
        .band = {0, 2},
        .loop = kBassLoop,
        .expression = {.spread = 6, .ornaments = {.rateFloor = 0, .rateCeiling = 60, .ratchet = {1, 0, 0}}},
        .gestures = kGestures,
        .drift = {.sweepBars = 8}, // the acid Loop's filter sweeps over 8 bars (spec §11.5)
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};

/// The kick drop with the acid line, the main fill and the held-fill entry, and a snare and tom run (spec §8.7).
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Rest, .mask = {.parts = 1U << 1, .lanes = laneBit(Lane::Kick)}, .beats = 2, .weight = 3},
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 2, .weight = 1},
};

constexpr Scale kScales[] = {Scale::Minor, Scale::Phrygian};
constexpr WeightedBars kBuildBars[] = {{8, 2}, {16, 1}};

/// The parent: never played, so it carries no version (spec §4.4).
constexpr Genre kTechnoParent{
    .name = "techno",
    .oledName = "TECHNO",
    .version = 0,
    .facts = {.tempoLowBpm = 125, .tempoHighBpm = 135, .scales = kScales},
    .parts = kParts,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 350,
    .palette = &technoPalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 1,
            .groovePhrases = {4, 8},
            .breakPhrases = {1, 2},
            .buildBars = kBuildBars,
            .breakKeeps = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim) | laneBit(Lane::ClosedHat)},
            .buildCut = {.lanes = laneBit(Lane::Kick)},
            .buildCutBars = 1,
            .gap = GapLength::Beat,
            .roll = {.lane = Lane::Snare, .top = RollStage::ThirtySecond},
            .accentLane = Lane::Crash,
        },
};

/// Today's Techno, the parent's table as it is. Its name, OLED name and so its palette override file stay as they were
/// until #218 makes it Acid techno.
constexpr Genre kTechno = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno";
	genre.oledName = "TECHNO";
	genre.shortName = "ACID";
	genre.version = 1;
	return genre;
}();
static_assert(interplayRulesValid(kTechno), "every interplay rule reads an earlier part or lane (spec §5.5)");
static_assert(loopNotesValid(kTechno), "every Loop note overlaps only the next onset's (spec §8.5, §8.6)");

// ---- Melodic techno (#226, #123): the Chord graph walked under a rolling bass, a Sustain Pad and a pluck lead ----
// The lead is a Loop until #219's Arp replaces it.

// clang-format off
/// The three 16ths after each beat, never the beat itself: the kick's steps stay empty, so no Avoid rule is needed.
constexpr Pattern kRollingBass[] = {
    {".888.888.888.888", "................", 0, 2},
    {".888.888.888.88.", "................", 0},
    {".878.878.878.878", "...g...g...g...g", 1, 2},
    {".888.878.888.868", "...............g", 1},
    {".887.887.887.886", "...g...g...g...g", 2},
};

/// Two bars of 8ths and dotted figures, never neighbouring 16ths: at the default Density the 7s and 8s play, a sparse
/// line, and more Density fills in the 4s to 6s.
constexpr Pattern kPluckLead[] = {
    {"8..7..8...7.....6.....8...7.....", "................................", 0, 2},
    {"..8...7...8...6...7...8.....6...", "................................", 0},
    {"8..7..8.5.7.4...6..7..8.5.7...5.", "............g.................g.", 1},
    {"..8..7..6.8...5...7..8..6.7..5..", "..............g..............g..", 1},
    {"8..7..8.6.7.5.6.7..8..7.5.8.6.5.", "............g...........g.....g.", 2},
};
// clang-format on

/// Mostly the root and its octave, the 5th now and then.
constexpr DegreeWeight kRollingDegrees[] = {{0, 9}, {4, 2}};
constexpr Answer kRollingAnswers[] = {{AnswerKind::Octave, 3}, {AnswerKind::Fifth}};

/// Rare slides, a tight gate, and a line that changes slowly: the bass rolls rather than talks.
constexpr LoopSpec kRollingLoop{
    .degrees = kRollingDegrees,
    .leap = 4,
    .octaveUp = {150, 300},
    .octaveDown = 0,
    .accent = 150,
    .slide = {20, 50},
    .mutation = 60,
    .answers = kRollingAnswers,
    .answerBars = 8,
    .gateTicks = 14,
};

/// Chord tones first, a passing step now and then, the 7th only at high Complexity. Steps are from the chord root and
/// snap to the Pentatonic set, so the line follows each chord.
constexpr DegreeWeight kPluckDegrees[] = {{0, 5}, {2, 4}, {4, 4}, {1, 1}, {3, 1}, {6, 2}};
/// A sequence a third away, the line an octave away, or played backwards (spec §5.2).
constexpr Answer kPluckAnswers[] = {{AnswerKind::Octave}, {AnswerKind::Third, 2}, {AnswerKind::Reverse}};

/// A melody, not an acid line: no slides, few accents and a riff that holds, mutating slowly.
constexpr LoopSpec kPluckLoop{
    .degrees = kPluckDegrees,
    .leap = {4, 6},
    .octaveUp = {40, 120},
    .octaveDown = 0,
    .accent = 100,
    .slide = 0,
    .mutation = 50,
    .answers = kPluckAnswers,
    .answerBars = 4,
    .gateTicks = 20,
};

/// Drums, a rolling bass, a Pad, a pluck lead and FX: the parent's Drums and FX parts as they are. The Pad is a
/// Sustain voicing each chord (re-struck every 2 bars as the walk moves), with the Pad Register's default span and
/// no doubled root, so it never plays more than the chord's 4 notes. Not gated: Density thins its voicing instead,
/// and it opens over the phrase with an 8-bar Brightness Sweep, as every Techno Pad does (spec §4.5).
constexpr PartSpec kMelodicParts[] = {
    kParts[0],
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kRollingBass,
        .band = {0, 2},
        .loop = kRollingLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "PAD",
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Loop,
        .name = "PLUCK",
        .pool = kPluckLead,
        .band = {0, 2},
        // 15 semitones, narrower than the Lead's default 24, so the line moves within about an octave.
        .reg = {.anchor = 72, .spanLow = -5, .spanHigh = 10},
        .degreeSet = DegreeSet::Pentatonic,
        .loop = kPluckLoop,
        .expression = {.lean = {0, 0, 1, 0}, .spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
    },
    kParts[2],
};

/// Home i; i–♭VI–♭VII–i is the main loop, with iv on the way to ♭VI and a short cut from i to ♭VII. Weights in the
/// colour moves' units (20).
constexpr ChordMove kMelodicMoves[] = {
    {kI, kVI, 30},   // i–♭VI
    {kVI, kVII, 30}, // ♭VI–♭VII
    {kVII, kI, 30},  // ♭VII–i
    {kI, kIV, 20},   // i–iv
    {kIV, kVI, 20},  // iv–♭VI
    {kI, kVII, 20},  // i–♭VII
};

/// 2 bars a chord, the cadence on ♭VII every 4 chords. In Harmonic Minor ♭VII is diminished, so the guard swaps the
/// cadence to V and its leading tone (spec §4.5, §7.4).
constexpr ChordGraph kMelodicGraph{
    .homePull = 10,
    .moves = kMelodicMoves,
    .cadenceTarget = kVII,
    .cadencePeriod = 4,
    .barsPerChord = 2,
};

constexpr Scale kMelodicScales[] = {Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor};

constexpr Genre kTechnoMelodic = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-melodic";
	genre.oledName = "MELODIC TECHNO";
	genre.shortName = "MELODIC";
	genre.facts.tempoLowBpm = 122;
	genre.facts.tempoHighBpm = 128;
	genre.facts.scales = kMelodicScales;
	genre.parts = kMelodicParts;
	genre.harmony = {.graph = &kMelodicGraph, .extensionCeiling = 1};
	genre.palette = &technoMelodicPalette;
	// The Break is the Pad, the bass and the lead over the closed hat. #219 changes it to the Pad, the Arp and the
	// Closed hat.
	genre.arc.breakKeeps = {.parts = (1U << 1) | (1U << 2) | (1U << 3), .lanes = laneBit(Lane::ClosedHat)};
	// The Pad's root is an anchor, but the bass and lead have none, so the parent's Density dip (-20) would leave the
	// Break a bare root at the default Density: dip Density by 5, so the bass's and lead's 7s and 8s still play and
	// the Pad keeps its 5th and 3rd, and Energy as the parent does.
	genre.arc.offsets[static_cast<int32_t>(SectionKind::Break)] = {macroPoints(-10), macroPoints(-10), macroPoints(-5),
	                                                               macroPoints(-5)};
	return genre;
}();
static_assert(interplayRulesValid(kTechnoMelodic), "every interplay rule reads an earlier part or lane (spec §5.5)");
static_assert(loopNotesValid(kTechnoMelodic), "every Loop note overlaps only the next onset's (spec §8.5, §8.6)");
static_assert(harmonyValid(kTechnoMelodic), "the Chord graph's rhythm fits the phrase (spec §7.4)");

} // namespace

const Genre techno = kTechno;
const Genre technoMelodic = kTechnoMelodic;

} // namespace deluge::generative::genres
