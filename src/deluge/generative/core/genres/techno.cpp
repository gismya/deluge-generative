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
// Filled so far: a Drums part (Kick, Rim, Closed hat, Open hat) and a Loop Bass, each with a small tiered pool,
// and the first palette entry per part and lane. Later M1 slices fill the rest of the slots.

#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal.

constexpr Pattern kKick[] = {
    {"X...X...X...X...", "................", 0},
    {"X...X...X...X.3.", "..............g.", 2},
};

constexpr Pattern kRim[] = {
    {".......3......2.", ".......g......g.", 0},
    {"...3...4..2...5.", "...g...g..g...g.", 1},
};

constexpr Pattern kClosedHat[] = {
    {"..8...8...8...8.", "................", 0},
    {"2584258425842584", "gg.ggg.ggg.ggg.g", 1},
    {"3795379537953796", "gg.ggg.ggg.ggg.g", 2},
};

constexpr Pattern kOpenHat[] = {
    {"......6.......6.", "................", 0},
    {"..5...7...5...7.", "..g.......g.....", 1},
};

constexpr Pattern kBass[] = {
    {"2.8.2.8.2.8.2.8.", "................", 0},
    {"2.8.5.8.2.8.6.8.", "........g.......", 1},
    {"3584659537846597", "g.g.g.g.g.g.g.g.", 2},
};

constexpr PaletteEntry kKickSounds[] = {{"SAMPLES/DRUMS/Kick/909 Kick.wav"}};
constexpr PaletteEntry kRimSounds[] = {{"SAMPLES/DRUMS/Rim/909 Rim.wav"}};
constexpr PaletteEntry kClosedHatSounds[] = {{"SAMPLES/DRUMS/HatC/909 Closed hihat.wav"}};
constexpr PaletteEntry kOpenHatSounds[] = {{"SAMPLES/DRUMS/HatO/909 Open hihat.wav"}};
// 165 Acid Arp with its arp cleared at load (spec §18.3).
constexpr PaletteEntry kBassSounds[] = {{"SYNTHS/165 Acid Arp.XML"}};

/// In the fixed lane order (spec §5.4).
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 2}, .palette = kKickSounds},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}, .palette = kRimSounds},
    {.lane = Lane::ClosedHat, .pool = kClosedHat, .band = {0, 2}, .palette = kClosedHatSounds},
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .palette = kOpenHatSounds},
};

constexpr DegreeWeight kBassDegrees[] = {{0, 6}, {2, 1}, {3, 1}, {4, 2}, {6, 2}};

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
        .palette = kBassSounds,
    },
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
