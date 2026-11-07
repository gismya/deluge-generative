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
// Filled so far: a Drums part (Kick, Rim, Closed hat, Open hat) and a Loop Bass, each with a small tiered pool (two
// entries in the tiers the default Complexity reaches, so picks and takes have a choice), an FX part (Riser, Fall,
// Impact) for section rules to play, and the palette (techno_palette.cpp). Later M1 slices fill the rest of the slots.

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

constexpr Pattern kBass[] = {
    {"2.8.2.8.2.8.2.8.", "................", 0},
    {"2.8.5.8.2.8.6.8.", "........g.......", 1, 2},
    {"..8.4.8...8.5.86", "....g.......g...", 1},
    {"3584659537846597", ".g.g.g.g.g.g.g.g", 2},
};
// clang-format on

/// The hats keep to the & steps between four-on-the-floor kicks, less so as Complexity rises (spec §5.5).
constexpr InterplayRule kHatsOffKick[] = {
    {.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .off = true, .strength = 90, .sensitivity = -40},
};

/// In the fixed lane order (spec §5.4).
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 2}},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}},
    {.lane = Lane::ClosedHat, .pool = kClosedHat, .band = {0, 2}, .rules = kHatsOffKick},
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .rules = kHatsOffKick},
};

constexpr DegreeWeight kBassDegrees[] = {{0, 6}, {2, 1}, {3, 1}, {4, 2}, {6, 2}};

/// FX lanes have no pools: only section rules play them (spec §5.4).
constexpr LaneSpec kFxLanes[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};

constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "ACID",
        .pool = kBass,
        .band = {0, 2},
        .loop =
            {.degrees = kBassDegrees, .octaveUp = 150, .octaveDown = 100, .accent = 250, .slide = 200, .mutation = 150},
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes},
};

constexpr Scale kScales[] = {Scale::Minor, Scale::Phrygian};
constexpr WeightedBars kBuildBars[] = {{8, 2}, {16, 1}};

} // namespace

const Genre techno{
    .name = "techno",
    .oledName = "TECHNO",
    .version = 1,
    .facts = {.tempoLowBpm = 125, .tempoHighBpm = 135, .scales = kScales},
    .parts = kParts,
    .palette = &technoPalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 1,
            .groovePhrases = {4, 8},
            .breakPhrases = {1, 2},
            .buildBars = kBuildBars,
        },
};

} // namespace deluge::generative::genres
