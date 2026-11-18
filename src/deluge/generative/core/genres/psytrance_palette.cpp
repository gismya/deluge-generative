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
// Psytrance's built-in palette (slot 8, spec §10.1, §18.3): the factory card content draft (§6.4 of the research),
// with the pack's psy bass as the Bass's main pick (spec §18.2). The drums reach the Psy set
// (`SAMPLES/Artists/Dean Daughters/Psy`) by its filename tags, topped up with the draft's factory tags so every lane
// that always plays has at least 3 files (spec §10.1). Pitched parts load at whole-sound transpose 0 (ADR 0007), so a
// preset's saved octave doesn't matter. Weights: 3 the main pick, 2 common, 1 an occasional variation. Every entry is
// starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// ---- Synth parts ----

/// The rolling bass: the pack's tight saw (sustain 0) is the main pick, so each 16th dies on its own. The factory
/// basses all hold their sustain; the Loop's short gate does that work for them. Not the draft's 048 Thin Pulse Bass:
/// its oscillators sit two and three octaves down (osc_octaves.py).
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/GENERATIVE/GEN Psy Bass.XML", 3}, {"SYNTHS/047 Basic Dirty Bass.XML", 2},
    {"SYNTHS/050 FM Basic Bass.XML", 2},       {"SYNTHS/008 FM Rich Distorted Bass.XML", 1},
    {"SYNTHS/095 Harsh FM Feedback.XML", 1},
};
/// The pedal tone under everything: dark and slow.
constexpr PaletteEntry kDrone[] = {
    {"SYNTHS/126 Dark FM Pad.XML", 2},
    {"SYNTHS/149 Cold 5th Pad.XML", 2},
    {"SYNTHS/120 High Harsh Pad.XML", 1},
};
/// The swept Lead: mono, saturated, sync and FM leads with room for the Brightness Sweep. 098 has saturation saved.
/// Not the draft's main pick 087 Define Leader: its oscillators sit an octave up and an octave down, none at the note
/// (osc_octaves.py).
constexpr PaletteEntry kLead[] = {
    {"SYNTHS/057 FM Lead.XML", 3},
    {"SYNTHS/098 Saturated Sync.XML", 2},
    {"SYNTHS/045 Square Sync.XML", 1},
    {"SYNTHS/059 Distorted Lead Guitar.XML", 1},
};
/// The Arp: arp-shaped sounds, each saved arp cleared at load (spec §10.6). Not the draft's 165 Acid Arp: its
/// oscillators sit two and three octaves down, so it would sound under the Bass in the Arp's Register (osc_octaves.py
/// allows it for Acid techno's ACID alone, whose Register is the Bass's).
constexpr PaletteEntry kArp[] = {
    {"SYNTHS/162 Busy Arp.XML", 2},
    {"SYNTHS/167 FM Metallic Bass Arp.XML", 2},
    {"SYNTHS/166 Harpsichord Cyborg.XML", 1},
};

constexpr PartSounds kParts[] = {{"BASS", kBass}, {"DRONE", kDrone}, {"LEAD", kLead}, {"ARP", kArp}};
/// Each role's presets too, so an override file's role reaches its parts: the Lead role reaches LEAD (ARP keeps its
/// own entry, being listed by name).
constexpr RoleSounds kRoles[] = {
    {Role::Bass, kBass},
    {Role::Pad, kDrone},
    {Role::Lead, kLead},
};

// ---- Drums lanes: the Psy set by tag, then factory machines ----

/// The lanes the Psy set holds look in its folder and in the lane's factory folder.
constexpr const char* const kPsy = "SAMPLES/Artists/Dean Daughters/Psy";
constexpr const char* const kKickFolders[] = {"SAMPLES/DRUMS/Kick", kPsy};
constexpr TagWeight kKickTags[] = {{"KICK-", 3}, {"Deep Sky Kick HQ", 1}, {"XV5080 Kick", 1}};
constexpr const char* const kSnareFolders[] = {"SAMPLES/DRUMS/Snare", kPsy};
constexpr TagWeight kSnareTags[] = {{"SNARE-", 2}, {"909", 1}};
/// The Psy set has one clap: the 909's and the pack's top it up.
constexpr const char* const kClapFolders[] = {"SAMPLES/DRUMS/Clap", kPsy};
constexpr TagWeight kClapTags[] = {{"CLAP-", 2}, {"909", 1}, {"GEN Clap", 1}};
// The Open hat first tries the Closed hat's tag (spec §10.3), so both list the same set and machine.
constexpr const char* const kClosedHatFolders[] = {"SAMPLES/DRUMS/HatC", kPsy};
constexpr TagWeight kClosedHatTags[] = {{"CLHAT-", 3}, {"909", 1}};
constexpr const char* const kOpenHatFolders[] = {"SAMPLES/DRUMS/HatO", kPsy};
constexpr TagWeight kOpenHatTags[] = {{"OPHAT-", 3}, {"909", 1}};
constexpr const char* const kRideFolders[] = {"SAMPLES/DRUMS/Ride", kPsy};
constexpr TagWeight kRideTags[] = {{"RIDE-", 2}, {"909", 1}, {"GEN Ride", 1}};
/// The Drop's Crash: the Psy set has none.
constexpr TagWeight kCrashTags[] = {{"909", 2}, {"XV5080", 1}, {"DDD1", 1}};
constexpr const char* const kPerc1Folders[] = {kPsy, "SAMPLES/DRUMS/Other"};
constexpr TagWeight kPerc1Tags[] = {{"PERC-", 3}, {"Click", 1}};
constexpr const char* const kPerc2Folders[] = {kPsy, "SAMPLES/DRUMS/Shaker"};
constexpr TagWeight kPerc2Tags[] = {{"SHAKER-", 2}, {"Cabasa", 1}};

// ---- FX lanes (spec §10.5) ----

// Riser: only pack risers, from its default folder SAMPLES/FX/Riser. Each is shorter than the 16-bar Build at
// 146 BPM, so it starts late in the Build and ends on the Drop.
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser.XML"}};
constexpr const char* const kFallFolders[] = {"SAMPLES/Artists/Danny Taurus/MoDrama"};
constexpr TagWeight kFallTags[] = {{"Downer", 3}};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};
/// The draft's booms for hard Drops.
constexpr const char* const kImpactFolders[] = {"SAMPLES/DRUMS/Kick", "SAMPLES/Artists/JS-Clipping",
                                                "SAMPLES/Artists/Eric Mooney/Kit1"};
constexpr TagWeight kImpactTags[] = {{"Deep Sky Kick descending", 2}, {"CinderBlock", 1}, {"Braams", 1}};
constexpr PaletteEntry kImpactSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Impact.XML"}};

constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .folders = kKickFolders, .tags = kKickTags},
    {.lane = Lane::Snare, .folders = kSnareFolders, .tags = kSnareTags},
    {.lane = Lane::Clap, .folders = kClapFolders, .tags = kClapTags},
    {.lane = Lane::ClosedHat, .folders = kClosedHatFolders, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .folders = kOpenHatFolders, .tags = kOpenHatTags},
    {.lane = Lane::Ride, .folders = kRideFolders, .tags = kRideTags},
    {.lane = Lane::Crash, .tags = kCrashTags},
    {.lane = Lane::Perc1, .folders = kPerc1Folders, .tags = kPerc1Tags},
    {.lane = Lane::Perc2, .folders = kPerc2Folders, .tags = kPerc2Tags},
    {.lane = Lane::Riser, .synths = kRiserSynths},
    {.lane = Lane::Fall, .folders = kFallFolders, .tags = kFallTags, .synths = kFallSynths},
    {.lane = Lane::Impact, .folders = kImpactFolders, .tags = kImpactTags, .synths = kImpactSynths},
};

} // namespace

/// Its Bass, Drone, Lead and Arp over the Psy set's drums and the Riser, Fall and Impact.
const Palette psytrancePalette{.parts = kParts, .roles = kRoles, .lanes = kLanes};

} // namespace deluge::generative::genres
