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
/// (osc_octaves.py). The owner dropped 135 Soothing Growth Pad and 147 Resonant Filter Pad by ear (#133, round 1),
/// and 118 Small Bridge Pad (round 2's I3-118).
constexpr PaletteEntry kPad[] = {
    {"SYNTHS/131 Warm Strings.XML", 2},
    {"SYNTHS/151 Radiant FM Pad.XML", 2},
    {"SYNTHS/157 Rich FM Pad 4.XML"},
    {"SYNTHS/145 Eerie High Pad.XML"},
};
/// The Drone (the key's root and fifth, held for whole Sections): the pack's three drones (owner, Ambient sounds round
/// 1, #313: tape strings, a breathing filter, a glass fifth), then 125's ringmod drone and 148, which stacks an open
/// fifth on each note, harmless on a pedal; equal weights until a later round weighs them. The pack's come first, so
/// the fallback chain (spec §10.1) reaches the factory ones on a card without the pack. The owner dropped 126 Dark FM
/// Pad by ear, too rumbly (#133, round 1). Not the draft's 056 FM Bell Modulation: its oscillators sit an octave up, so
/// it would drone over the Pad (osc_octaves.py).
constexpr PaletteEntry kDrone[] = {
    {"SYNTHS/GENERATIVE/GEN Ambient Drone 1.XML", 1},
    {"SYNTHS/GENERATIVE/GEN Ambient Drone 2.XML", 1},
    {"SYNTHS/GENERATIVE/GEN Ambient Drone 3.XML", 1},
    {"SYNTHS/125 Evolving Pad.XML", 1},
    {"SYNTHS/148 Warm 5th Pad.XML", 1},
};
/// The bells: struck sounds whose long tails are welcome here. The owner dropped 054 Ghostly Sines and 121 Tiny Lights
/// by ear (#133, round 1). Not 068 FM Bells 1: its oscillator sits an octave up, so it plays an octave over the
/// Register (spec §10.6, osc_octaves.py).
constexpr PaletteEntry kBells[] = {
    {"SYNTHS/070 Glockenspiel.XML", 2},
    {"SYNTHS/106 Hang Drum.XML", 2},
    {"SYNTHS/168 Hang Drum.XML", 2},
    {"SYNTHS/069 FM Bells 2.XML", 1},
};
/// The Bass: the pack's three Ambient basses (owner, Ambient sounds round 1, #313: a felt pulse, a folded sine, a tape
/// bass), equal weights, in place of GEN Sine Sub, 169 and 002, which the owner dropped by ear (#133, round 1). 002
/// Basic Square Bass follows at weight 0: never rolled, it loads only through the fallback chain (spec §10.1) on a card
/// without the pack.
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/GENERATIVE/GEN Ambient Bass 2.XML", 1},
    {"SYNTHS/GENERATIVE/GEN Ambient Bass 3.XML", 1},
    {"SYNTHS/GENERATIVE/GEN Ambient Bass 4.XML", 1},
    {"SYNTHS/002 Basic Square Bass.XML", 0},
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
