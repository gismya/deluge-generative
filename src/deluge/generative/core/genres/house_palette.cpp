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
// House's built-in palette (slot 8, spec §10.1, §18.3): the factory card content draft (§6.2 of the research), with the
// pack's plain chord stab as the Stab's main pick, the pack's claps and rides on Clap and Ride, and only the pack
// Risers of 6 s or less on the Riser lane (spec §18.2). Pitched parts load at whole-sound transpose 0 (ADR 0007), so a
// preset's saved octave doesn't matter. Weights: 3 the main pick, 2 common, 1 an occasional variation. Every entry is
// starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/genre.h"
#include "generative/core/genres/drum_machines.h"

namespace deluge::generative::genres {

namespace {

// ---- Synth parts ----

/// The offbeat bass: 080, the card's house organ bass, first; then round, short basses. 000 Rich Saw Bass dropped by
/// ear (listening round 2).
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/080 House.XML", 3},
    {"SYNTHS/002 Basic Square Bass.XML", 2},
    {"SYNTHS/050 FM Basic Bass.XML", 2},
    {"SYNTHS/012 Blunt Sync Bass.XML", 1},
};
/// The stab: the pack's plain saw and square (no built-in interval, no portamento, sustain 0) first, then 078's FM
/// organ stab, the Rhodes, the piano and an organ, which carry 7th and 9th voicings. 074 Electric Piano dropped by ear
/// (listening round 2).
constexpr PaletteEntry kStab[] = {
    {"SYNTHS/GENERATIVE/GEN House Stab.XML", 3},
    {"SYNTHS/078 House.XML", 2},
    {"SYNTHS/071 Rhodes.XML", 2},
    {"SYNTHS/073 Piano.XML", 1},
    {"SYNTHS/026 PW Organ.XML", 1},
};
/// Strings under the stabs. 136's saved portamento is cleared at load (spec §10.6). 143 Chillout Pad dropped by ear
/// (listening round 2).
constexpr PaletteEntry kStrings[] = {
    {"SYNTHS/131 Warm Strings.XML", 3},
    {"SYNTHS/139 Detuned Saw Pad.XML", 1},
    {"SYNTHS/136 Synthwave Pad.XML", 1},
};
/// Short answering voices: the marimba first, an arp lead (its saved arp cleared at load), a bell and a plain lead.
/// The bell is 069, not the draft's 068 FM Bells 1: its oscillator sits an octave up, so it would play an octave over
/// the Register (spec §10.6, osc_octaves.py), as Detroit's palette found.
constexpr PaletteEntry kLead[] = {
    {"SYNTHS/067 Marimba.XML", 2},
    {"SYNTHS/163 Crisp Pop Arp.XML", 2},
    {"SYNTHS/069 FM Bells 2.XML", 1},
    {"SYNTHS/021 80s TV Lead.XML", 1},
};

constexpr PartSounds kParts[] = {{"BASS", kBass}, {"STAB", kStab}, {"STRINGS", kStrings}, {"LEAD", kLead}};
/// Each role's presets too, so an override file's role reaches its parts.
constexpr RoleSounds kRoles[] = {
    {Role::Bass, kBass},
    {Role::Chords, kStab},
    {Role::Pad, kStrings},
    {Role::Lead, kLead},
};

// ---- Drums lanes: the draft's machines by tag, the pack's claps and rides added ----

/// The plain 909 and 808 kicks by their paths, not their distorted, dry, sustained or descending ones (a tag matches
/// any part of a name).
constexpr TagWeight kKickTags[] = {
    {"SAMPLES/DRUMS/Kick/909 Kick.wav", 3}, {"SAMPLES/DRUMS/Kick/808 Kick.wav", 1}, {"Linndrum", 1}};
/// The draft lists none: the 909 snare for the Swap and the rolls, the 808's and the Linndrum's now and then.
constexpr TagWeight kSnareTags[] = {{"909", 3}, {"808", 1}, {"Linndrum", 1}};
constexpr TagWeight kClapTags[] = {{"909", 3}, {"GEN Clap", 2}, {"808", 2}, {"Linndrum", 1}};
/// The draft's two machines and the Linndrum's, so the lane has 3 files.
constexpr TagWeight kRimTags[] = {{"808", 2}, {"909", 2}, {"Linndrum", 1}};
// The Open hat first tries the Closed hat's tag (spec §10.3), so both list the same machines.
constexpr TagWeight kClosedHatTags[] = {{"909", 3}, {"808", 2}, {"Linndrum", 1}};
constexpr TagWeight kOpenHatTags[] = {{"909", 3}, {"808", 1}, {"Linndrum", 1}};
constexpr TagWeight kRideTags[] = {{"909", 3}, {"GEN Ride", 2}, {"Linndrum", 1}};
constexpr TagWeight kCrashTags[] = {{"909", 2}, {"808", 1}, {"Linndrum", 1}};
constexpr const char* const kPerc1Folders[] = {"SAMPLES/DRUMS/Shaker", "SAMPLES/DRUMS/Tambourine"};
constexpr TagWeight kPerc1Tags[] = {{"Cabasa", 2}, {"Maraca", 1}, {"Tambourine", 1}};
constexpr const char* const kPerc2Folders[] = {"SAMPLES/DRUMS/ConL", "SAMPLES/DRUMS/ConM", "SAMPLES/DRUMS/ConH",
                                               "SAMPLES/DRUMS/Cowbell"};
constexpr TagWeight kPerc2Tags[] = {{"808", 2}, {"Linndrum", 1}};

// ---- Drum kits (spec §10.2, drum-kit listening session round 1) ----

/// Each piece plays the 909, the 808 or the Linndrum, most often the 909, on every lane they have a file for.
constexpr MachineWeight kMachines[] = {{&machines::k909, 3}, {&machines::k808, 2}, {&machines::kLinndrum, 1}};
/// Chicago and New York house ran a 909 and took the rim and congas from an 808 beside it, and a sampled drum
/// machine's tambourine for the 16ths.
constexpr KitSwap kSwaps[] = {
    {Lane::Rim, "808 Rim"}, {Lane::Perc2, "808 Conga low"}, {Lane::Perc1, "Linndrum Tambourine"}};

// ---- FX lanes (spec §10.5) ----

/// Riser: the pack's 6 s and 4 s risers from the default SAMPLES/FX/Riser. Not the 8 s one: it overshoots a 4-bar
/// Build at 126 BPM (7.6 s), which the Riser fit catches. The pack's synth Riser if they're missing.
constexpr TagWeight kRiserTags[] = {{"GEN Riser 2", 1}, {"GEN Riser 3", 1}};
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser.XML"}};
constexpr const char* const kFallFolders[] = {"SAMPLES/Artists/Danny Taurus/MoDrama"};
constexpr TagWeight kFallTags[] = {{"Downer", 3}};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};
/// Impact: the pack's factory 808 boom alone (owner, #472 impact round 3's H1). It lists no folder, and its default
/// SAMPLES/FX/Impact holds no files, so the lane loads its preset (spec §10.5).
constexpr PaletteEntry kImpactSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Impact Boom.XML"}};

constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .tags = kKickTags},
    {.lane = Lane::Snare, .tags = kSnareTags},
    {.lane = Lane::Clap, .tags = kClapTags},
    {.lane = Lane::Rim, .tags = kRimTags},
    {.lane = Lane::ClosedHat, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .tags = kOpenHatTags},
    {.lane = Lane::Ride, .tags = kRideTags},
    {.lane = Lane::Crash, .tags = kCrashTags},
    {.lane = Lane::Perc1, .folders = kPerc1Folders, .tags = kPerc1Tags},
    {.lane = Lane::Perc2, .folders = kPerc2Folders, .tags = kPerc2Tags},
    {.lane = Lane::Riser, .tags = kRiserTags, .synths = kRiserSynths},
    {.lane = Lane::Fall, .folders = kFallFolders, .tags = kFallTags, .synths = kFallSynths},
    {.lane = Lane::Impact, .synths = kImpactSynths},
};

} // namespace

/// Its Bass, Stab, Strings and Lead over the house drums and the Riser, Fall and Impact.
const Palette housePalette{
    .parts = kParts, .roles = kRoles, .lanes = kLanes, .kits = {.machines = kMachines, .swaps = kSwaps}};

} // namespace deluge::generative::genres
