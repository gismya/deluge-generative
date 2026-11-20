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

// Ambient's built-in palette (slot 8, spec §10.1, §18.3): the factory card content draft (§6.5 of the research), with
// the pack's sine sub, brushes, mallets, wood hits and pings (spec §18.2). Pitched parts load at whole-sound transpose
// 0 (ADR 0007), so a preset's saved octave doesn't matter. Weights: 3 the main pick, 2 common, 1 an occasional
// variation. Every entry is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/genre.h"
#include "generative/core/genres/drum_machines.h"

namespace deluge::generative::genres {

namespace {

// ---- Synth parts ----

/// The Pad: slow attacks and long releases. Not 127, 155 or 156: each adds a major 3rd to every note, a cluster under
/// a chord with its Extensions. Not 140 Slow Aural Swells: its oscillators sit one and two octaves down
/// (osc_octaves.py). 135 carries reverb 35 of its own, so Space has less room on it.
constexpr PaletteEntry kPad[] = {
    {"SYNTHS/131 Warm Strings.XML", 2},     {"SYNTHS/118 Small Bridge Pad.XML", 2},
    {"SYNTHS/151 Radiant FM Pad.XML", 2},   {"SYNTHS/135 Soothing Growth Pad.XML"},
    {"SYNTHS/157 Rich FM Pad 4.XML"},       {"SYNTHS/145 Eerie High Pad.XML"},
    {"SYNTHS/147 Resonant Filter Pad.XML"},
};
/// The Drone (the key's root and fifth, held for whole Sections): 125's ringmod drone is the main pick; 148 stacks an
/// open fifth on each note, harmless on a pedal; 126's dark FM stacks another, as in Acid techno's Drone. Not the
/// draft's 056 FM Bell Modulation: its oscillators sit an octave up, so it would drone over the Pad (osc_octaves.py).
constexpr PaletteEntry kDrone[] = {
    {"SYNTHS/125 Evolving Pad.XML", 3},
    {"SYNTHS/148 Warm 5th Pad.XML", 2},
    {"SYNTHS/126 Dark FM Pad.XML", 1},
};
/// The bells: struck sounds whose long tails are welcome here. 121's saved arp is cleared at load (spec §10.6). Not
/// 068 FM Bells 1: its oscillator sits an octave up, so it plays an octave over the Register (spec §10.6,
/// osc_octaves.py).
constexpr PaletteEntry kBells[] = {
    {"SYNTHS/070 Glockenspiel.XML", 2}, {"SYNTHS/106 Hang Drum.XML", 2},     {"SYNTHS/168 Hang Drum.XML", 2},
    {"SYNTHS/069 FM Bells 2.XML", 1},   {"SYNTHS/054 Ghostly Sines.XML", 1}, {"SYNTHS/121 Tiny Lights.XML", 1},
};
/// The Bass: the pack's soft sine sub (spec §18.2) is the main pick; 169's double bass and 002's round square vary it.
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/GENERATIVE/GEN Sine Sub.XML", 3},
    {"SYNTHS/169 Double Bass.XML", 2},
    {"SYNTHS/002 Basic Square Bass.XML", 1},
};

constexpr PartSounds kParts[] = {{"BASS", kBass}, {"PAD", kPad}, {"DRONE", kDrone}, {"BELLS", kBells}};
/// Each role's presets too, so an override file's role reaches its parts: the Pad role reaches both PAD and DRONE.
constexpr RoleSounds kRoles[] = {
    {Role::Bass, kBass},
    {Role::Pad, kPad},
    {Role::Lead, kBells},
};

// ---- Drums lanes: soft, short machines, and the pack's non-tonal one-shots ----

constexpr TagWeight kMachineTags[] = {{"CR78", 2}, {"KR55", 1}};
constexpr TagWeight kBrushTags[] = {{"GEN Brush", 1}};
constexpr const char* const kMalletFolders[] = {"SAMPLES/DRUMS/Other"};
constexpr TagWeight kMalletTags[] = {{"GEN Mallet", 2}, {"GEN Ping", 2}, {"Chime", 1}, {"Triangle", 1}};
constexpr const char* const kWoodFolders[] = {"SAMPLES/DRUMS/Block", "SAMPLES/DRUMS/ConL", "SAMPLES/DRUMS/ConM",
                                              "SAMPLES/DRUMS/ConH"};
constexpr TagWeight kWoodTags[] = {{"GEN Wood", 2}, {"Woodblock", 1}, {"Conga", 1}};

// ---- Drum kits (spec §10.2, drum-kit listening session round 1) ----

/// Each piece plays the CR78 or, less often, the KR55, on every lane they have a file for.
constexpr MachineWeight kMachines[] = {{&machines::kCR78, 2}, {&machines::kKR55, 1}};
/// The pack's brush for a soft swish, not a backbeat, and its mallets and wood for non-tonal texture.
constexpr KitSwap kSwaps[] = {{Lane::Snare, "GEN Brush 2"}, {Lane::Perc1, "GEN Mallet 1"}, {Lane::Perc2, "GEN Wood 1"}};

// ---- FX lanes (spec §10.5) ----

// Riser: only pack risers, from its default folder SAMPLES/FX/Riser; every one fits the 16-bar Build at 90 BPM.
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser.XML"}};
constexpr const char* const kFallFolders[] = {"SAMPLES/Artists/Danny Taurus/MoDrama"};
constexpr TagWeight kFallTags[] = {{"Downer", 3}};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};

constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .tags = kMachineTags},
    {.lane = Lane::Snare, .tags = kBrushTags},
    {.lane = Lane::Rim, .tags = kMachineTags},
    {.lane = Lane::ClosedHat, .tags = kMachineTags},
    {.lane = Lane::Perc1, .folders = kMalletFolders, .tags = kMalletTags},
    {.lane = Lane::Perc2, .folders = kWoodFolders, .tags = kWoodTags},
    {.lane = Lane::Riser, .synths = kRiserSynths},
    {.lane = Lane::Fall, .folders = kFallFolders, .tags = kFallTags, .synths = kFallSynths},
};

} // namespace

/// Its Bass, Pad, Drone and bells over soft drum lanes and the Riser and Fall.
const Palette ambientPalette{
    .parts = kParts, .roles = kRoles, .lanes = kLanes, .kits = {.machines = kMachines, .swaps = kSwaps}};

} // namespace deluge::generative::genres
