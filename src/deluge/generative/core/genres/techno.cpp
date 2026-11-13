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

// Techno (spec §4.5): 125–135 BPM, Minor or Phrygian, straight. Four-on-the-floor kick, offbeat hats and a mutating
// acid line. No claps on 2 and 4. Every value is starting data, tuned by ear later (spec §21.5).
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

constexpr Genre kTechno{
    .name = "techno",
    .oledName = "TECHNO",
    .version = 1,
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
static_assert(interplayRulesValid(kTechno), "every interplay rule reads an earlier part or lane (spec §5.5)");
static_assert(loopNotesValid(kTechno), "every Loop note overlaps only the next onset's (spec §8.5, §8.6)");

} // namespace

const Genre techno = kTechno;

} // namespace deluge::generative::genres
