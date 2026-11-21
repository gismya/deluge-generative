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
// Dub (#131, spec §4.5): roots dub, a genre of its own with no parent, not to be confused with Dub techno. One-drop
// drums (the kick and a cross-stick Rim on beat 3), an offbeat organ skank, a bass that carries the tune, a melodica,
// and drop-outs with delay Throws as the main motion, over a slow minor Chord graph. A Siren wails on Drops. Every
// value is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
/// The one drop: the kick on beat 3 alone, with the Rim (spec §4.5). A pickup on the & of 4 as Complexity rises.
constexpr Pattern kKick[] = {
    {"........X.......", "................", 0, 2},
    {"........X.....7.", "................", 1},
};

/// The cross-stick on beat 3, anchored, at every tier: the Rim holds the backbeat and the Snare never does in its
/// step tables (it has none), so exactly one of them is on beat 3 at each tier. A stroke on the & of 4 on every line, at
/// importance 8 so it plays from about Density 10: the drum drop-out's Throw spikes on the Rim's hits in the beat
/// before it, and with none there it wouldn't throw.
/// Ghosted strokes off the beat (the & of 1, then the & of 3) as Complexity rises.
constexpr Pattern kRim[] = {
    {"........X.....8.", "................", 0},
    {"..5.....X.....8.", "..g.............", 1},
    {"..5.....X.6...8.", "..g.......g.....", 2},
};

/// 8ths, the & leaned late (kHatLean) so they swing; a ghosted 16th now and then as Complexity rises.
constexpr Pattern kClosedHat[] = {
    {"8.8.8.8.8.8.8.8.", "................", 0, 2},
    {"8.8.8.858.8.8.8.", ".......g........", 1},
    {"8.858.8.8.858.86", "...g.......g...g", 2},
};

/// An open hat on the & of 4 now and then: it replaces the closed hat there (spec §5.7).
constexpr Pattern kOpenHat[] = {
    {"..............6.", "................", 0},
    {"......6.......7.", "................", 1},
};

/// The shaker on the &s, behind its gate.
constexpr Pattern kShaker[] = {
    {"..6...6...6...6.", "................", 0},
    {".46.4.6.4.6.4.67", ".g..g...g...g...", 1},
};

/// Bongos and congas off the beat, behind their gate.
constexpr Pattern kHandDrums[] = {
    {"...6......6.....", "................", 0},
    {"...6..5...6..7..", "......g.........", 1},
};

// Fill patterns: only a Swap plays them. The Snare's holds the backbeat (the Rim and Snare Swap moves it there); the
// Rim's is empty, so the Rim falls silent under that Swap. The toms run over the last beats.
constexpr Pattern kSnareFill[] = {
    {"........X.......", "................", 0},
    {"........X.....7.", "................", 1},
};
constexpr Pattern kRimFill[] = {
    {"................", "................", 0},
};
constexpr Pattern kTomFill[] = {
    {"...........8.8X.", "................", 0},
    {"..........7.X8XX", "..........g.....", 1},
};

/// The bass line: two bars, anchored on each bar's 1, notes at least an 8th apart so each rings to the next
/// (gateTicks), beat 3 left to the drop on the tier-0 lines but one.
constexpr Pattern kBassLine[] = {
    {"X..8..8.....8...X.....8...8.8...", "................................", 0, 2},
    {"X.....8...8.8...X..8..8.....8.8.", "................................", 0, 2},
    {"X.......8...8...X..8..8.....8...", "................................", 0},
    {"X..8..8.7...8.8.X..8..8...8.8.7.", "........g.....................g.", 1},
    {"X..8..8...8.7.8.X.....8.8.7.8...", "............g.............g.....", 1},
    {"X.8.7.8.7.8.8.8.X..8.7.8.7.8.8..", "....g...g............g...g......", 2},
};

/// The skank: the backbeats of the half-time feel, beats 2 and 4, anchored so a dip keeps it; its "chk-a" double on
/// the & after each as Complexity rises.
constexpr Pattern kSkank[] = {
    {"....X.......X...", "................", 0, 2},
    {"....X.......X.6.", "................", 0},
    {"....X.7.....X.7.", "................", 1},
    {"..5.X.7...5.X.7.", "..g.......g.....", 2},
};

/// The melodica's phrases, one and two bars from an anchored 1: few long notes, on the steps every tier-0 and tier-1
/// Bass line leaves free but the 1, so the melodica answers the bass instead of doubling it; a 2-bar line's second bar
/// few notes, ending on the last beat.
/// Every line has an onset in each bar's last beat, at importance 8: the drop-out's Throw spikes on the melodica's
/// notes there.
constexpr Pattern kMelodicaPhrases[] = {
    {"X........7...8..", "................", 0, 2},
    {"X......7.....8.." "....8........8..", "................................", 0},
    {"X...7....7...8..", "................", 1},
    {"X....7...7...8.." "....8....7...8..", "................................", 1},
    {"X......7...7.8.." ".......7.....8..", "...........g....................", 1},
};
// clang-format on

/// A lazy snare: late on every 16th (spec §4.5).
constexpr Expression kLazySnare{.lean = {2, 2, 2, 2}, .spread = 8};
/// The hats' & sits late, so the 8ths swing over the song swing's 16ths (spec §8.5).
constexpr int8_t kHatLean = 6;

/// The drop-out and Throw echoes: a dotted 8th with the analog delay (spec §10.6, tuning).
constexpr DelaySetup kWideEcho{
    .on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true, .analog = true};
constexpr DelaySetup kRimEcho{.on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .analog = true};

constexpr int16_t kPercGate = macroPoints(35);

/// In the fixed lane order (spec §5.4). The Snare plays only Swaps and the Build roll; the Crash only the Phrase
/// accent; the Tom only the Swap.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 1}, .expression = {.spread = 4}},
    {.lane = Lane::Snare, .fillPool = kSnareFill, .band = {0, 1}, .expression = kLazySnare},
    {
        .lane = Lane::Rim,
        .pool = kRim,
        .fillPool = kRimFill,
        .band = {0, 2},
        .expression = {.spread = 8},
        .delay = kRimEcho, // the second drop-out entry's throw target
    },
    {
        .lane = Lane::ClosedHat,
        .pool = kClosedHat,
        .band = {0, 2},
        .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 10},
    },
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 6}},
    {.lane = Lane::Crash}, // the Phrase accent's lane
    {.lane = Lane::Tom, .fillPool = kTomFill, .band = {0, 1}},
    {
        .lane = Lane::Perc1,
        .pool = kShaker,
        .band = {0, 1},
        .densityGate = kPercGate,
        .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 10},
    },
    {.lane = Lane::Perc2, .pool = kHandDrums, .band = {0, 1}, .densityGate = kPercGate, .expression = {.spread = 10}},
};

/// FX lanes have no pools: only section rules play them (spec §5.4). The Siren holds one note for 2 bars on a Break's
/// first step and on a Drop, gated at 30: at the default macros it sounds on Drops, whose lift takes Density to 33
/// (spec §9.5). Its release always throws, into its own echo.
constexpr LaneSpec kFxLanes[] = {
    {.lane = Lane::Riser},
    {.lane = Lane::Fall},
    {.lane = Lane::Impact},
    {.lane = Lane::Siren, .densityGate = macroPoints(30), .delay = kWideEcho, .heldBars = 2},
};

/// The Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};
/// On Drums the Throw acts on the Rim's row: it has the echo, and it is the drum drop-out's throw target.
constexpr GestureSpec kDrumGestures = [] {
	GestureSpec gestures = kGestures;
	gestures.aim[static_cast<int32_t>(Gesture::Throw)] = Lane::Rim;
	return gestures;
}();

/// Scale steps from the chord root, snapped to the Pentatonic set: mostly the root and the 5th, the 3rd, then the rest.
constexpr LineWeight kBassDegrees[] = {{0, 5}, {2, 2}, {4, 3}, {1, 1}, {3, 1}, {5, 1}, {6, 1}};
constexpr Answer kBassAnswers[] = {{AnswerKind::Octave, 2}, {AnswerKind::Fifth}, {AnswerKind::Rotate}};

/// A line that leaps, an octave up now and then, never sliding; each note held to the next onset's 8th.
constexpr LoopSpec kBassLoop{
    .degrees = kBassDegrees,
    .draws = {.leap = {3, 6}, .octaveUp = {40, 150}, .octaveDown = 0, .accent = 120},
    .slide = 0,
    .mutation = 80,
    .answers = kBassAnswers,
    .answerBars = 4,
    .gateTicks = 40,
};

/// Chord positions: mostly the root, the 3rd and the 5th, the 7th and the note under the root now and then.
constexpr LineWeight kMelodicaPositions[] = {{-1, 1}, {0, 4}, {1, 3}, {2, 3}, {3, 1}};

constexpr int32_t kBassPart = 1;
constexpr int32_t kSkankPart = 2;
constexpr int32_t kMelodicaPart = 3;

/// Drums, Bass, Skank, Melodica and FX (spec §4.5). The sixth MUTE pad stays free.
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kDrumGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kBassLine,
        .band = {0, 2},
        // The Bass Register's default: the tonic sounds between F#1 and F2.
        .degreeSet = DegreeSet::Pentatonic,
        .loop = kBassLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Chords,
        .flavour = Flavour::Stab,
        .name = "SKANK",
        .pool = kSkank,
        .band = {0, 2},
        // Short, so each chop stands apart (tuning).
        .stab = {.gateTicks = 20},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // 80 % of the preset's volume knob (tuning): at the preset's level the skank sat about 3 dB under the rest of
        // the mix, over the bass (delugemu A/B capture, seed 1, a card with the pack).
        .volume = 80,
        .delay = kWideEcho, // the first drop-out entry's throw target
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "MELODICA",
        .pool = kMelodicaPhrases,
        .band = {0, 1},
        .motif = {.positions = kMelodicaPositions,
                  .draws = {.leap = {2, 3}, .octaveUp = {0, 100}, .accent = 150},
                  .reroll = 100,
                  .gateTicks = 168},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // 70 % of the preset's volume knob (tuning): at the preset's level the melodica sat about 1 dB over the rest
        // of the mix (delugemu A/B capture, seed 1, a card with the pack).
        .volume = 70,
        .delay = kWideEcho, // the third drop-out entry's throw target
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};
static_assert(kParts[kBassPart].role == Role::Bass);
static_assert(kParts[kSkankPart].role == Role::Chords && kParts[kSkankPart].flavour == Flavour::Stab);
static_assert(kParts[kMelodicaPart].role == Role::Lead && kParts[kMelodicaPart].flavour == Flavour::Motif);

/// Every Drums lane the roster lists but the Kick.
constexpr uint16_t kDrumsButKick = laneBit(Lane::Snare) | laneBit(Lane::Rim) | laneBit(Lane::ClosedHat)
                                   | laneBit(Lane::OpenHat) | laneBit(Lane::Crash) | laneBit(Lane::Tom)
                                   | laneBit(Lane::Perc1) | laneBit(Lane::Perc2);

/// A Swap on Snare and Tom, the held-fill entry: the snare's backbeat under a tom run. A Swap on Rim and Snare moves
/// the backbeat from the Rim to the Snare for the phrase's last bar (spec §8.7).
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 2, .weight = 1},
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Rim) | laneBit(Lane::Snare)}, .beats = 4, .weight = 1},
};

/// The skank for 2 bars, thrown; every Drums lane but the Kick for 1 bar, the Rim thrown; everything but the Bass for
/// 4 bars, the Melodica thrown (spec §4.5, §8.8). Each target has its echo.
constexpr PartsAndLanes kSkankOnly{.parts = partBit(kSkankPart)};
constexpr PartsAndLanes kRimOnly{.lanes = laneBit(Lane::Rim)};
constexpr PartsAndLanes kMelodicaOnly{.parts = partBit(kMelodicaPart)};
constexpr DropOutEntry kDropOuts[] = {
    {.mask = kSkankOnly, .bars = 2, .weight = 1, .throwTarget = kSkankOnly},
    {.mask = {.lanes = kDrumsButKick}, .bars = 1, .weight = 1, .throwTarget = kRimOnly},
    {.mask = {.parts = partBit(kSkankPart) | partBit(kMelodicaPart), .lanes = kDrumsButKick | laneBit(Lane::Kick)},
     .bars = 4,
     .weight = 1,
     .throwTarget = kMelodicaOnly},
};

/// Home i, with the brief's moves one way each: i–iv, i–♭VII and iv–v. The home pull brings ♭VII and v back to i, a
/// repeat holds a chord on, and Minor's colour moves add the rest (spec §7.4). Weights in the colour moves' units.
constexpr ChordMove kDubMoves[] = {
    {kI, kIV, 30},  // i–iv
    {kI, kVII, 30}, // i–♭VII
    {kIV, kV, 20},  // iv–v
};

/// 2 bars a chord, repeats at 20, no cadence target (spec §4.5).
constexpr ChordGraph kDubGraph{
    .homePull = 20,
    .moves = kDubMoves,
    .repeatWeight = 20,
    .barsPerChord = 2,
};

constexpr Scale kScales[] = {Scale::Minor};

/// The Generator-wide pair, and the Crash replacing the closed hat (spec §5.7).
constexpr ReplacePair kReplacePairs[] = {{Lane::OpenHat, Lane::ClosedHat}, {Lane::Crash, Lane::ClosedHat}};

/// One bar: Dub barely builds (spec §4.5).
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// The Break's drum and bass: the Kick, the Rim and the Bass (spec §4.5).
constexpr PartsAndLanes kBreakKeeps{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim)};

constexpr Genre kDub{
    .name = "dub",
    .oledName = "ROOTS DUB", // never read as DUB TECHNO
    .version = 2,            // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 70, .tempoHighBpm = 80, .scales = kScales, .swing = 10},
    .phraseBars = 8,
    .parts = kParts,
    // The Crash replaces the closed hat on the Phrase accent's step, as the open hat does on its &: the Crash never
    // lands on a hat (spec §4.5, §5.7).
    .replacePairs = kReplacePairs,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 250,
    .dropOuts = kDropOuts,
    .dropOutChance = 120,
    .dropOutGapBars = 2,
    .harmony = {.graph = &kDubGraph, .extensionFloor = 0, .extensionCeiling = 1},
    .palette = &dubPalette,
    .arc =
        {
            .introPhrases = 1,
            .outroPhrases = 1,
            .groovePhrases = {4, 8},
            .breakPhrases = {1, 1},
            .buildBars = kBuildBars,
            .grooveToBuild = 0,
            .dropDecayPhrases = 1,
            .offsets =
                {
                    {macroPoints(-15), 0},                                                    // Intro
                    {},                                                                       // Groove
                    {macroPoints(-10), macroPoints(-10), macroPoints(-10), macroPoints(-10)}, // Break
                    {0, macroPoints(12), 0, macroPoints(10)},                                 // Build
                    {0, macroPoints(-15)},                                                    // Outro
                },
            // The breakdown is the drum and bass: the one drop and the line, Density dipping by 10, not 20, so the
            // bass keeps more than its anchors.
            .breakKeeps = kBreakKeeps,
            // No cut and no Gap: each would cover the whole 1-bar Build (cutAndGapUnderBuild()).
            .buildCutBars = 0,
            .gap = GapLength::None,
            .roll = {.lane = Lane::Snare, .top = RollStage::Sixteenth},
            .accentLane = Lane::Crash,
            .accentChance = 50, // a Crash on Drops, after a fill that ends a phrase, and at 50 per-mille
        },
};
static_assert(genreValid(kDub), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre dub = kDub;

} // namespace deluge::generative::genres
