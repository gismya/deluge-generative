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

// Techno's built-in palette (slot 8, spec §10.1, §18.3): the factory card content draft (§6.1 of the research), with
// pack risers only on the Riser lane and pack claps and rides added. Weights: 3 the main pick, 2 common, 1 an
// occasional variation. Every entry is starting data, tuned by ear later (spec §21.5).

#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// ---- Synth parts ----

// 165 Acid Arp with its arp cleared at load (spec §10.6) is the first ear check (spec §18.3).
constexpr PaletteEntry kAcid[] = {
    {"SYNTHS/165 Acid Arp.XML", 3},
    {"SYNTHS/015 Resonant Filter Bass.XML", 2},
    {"SYNTHS/097 Saturated Filter.XML", 2},
    {"SYNTHS/005 Sweet Mono Bass.XML", 1},
};
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/047 Basic Dirty Bass.XML", 3},
    {"SYNTHS/000 Rich Saw Bass.XML", 2},
    {"SYNTHS/007 Detuned Saw Bass.XML", 1},
    {"SYNTHS/008 FM Rich Distorted Bass.XML", 1},
};
constexpr PaletteEntry kDrone[] = {
    {"SYNTHS/148 Warm 5th Pad.XML", 2},
    {"SYNTHS/126 Dark FM Pad.XML", 2},
    {"SYNTHS/139 Detuned Saw Pad.XML", 2},
    {"SYNTHS/149 Cold 5th Pad.XML", 1},
};
constexpr PaletteEntry kStab[] = {
    {"SYNTHS/086 Techno Organ.XML", 3},
    {"SYNTHS/037 Echo Chord.XML", 2},
    {"SYNTHS/026 PW Organ.XML", 1},
    {"SYNTHS/083 Dark Chorus.XML", 1},
};

constexpr PartSounds kParts[] = {{"ACID", kAcid}};
constexpr RoleSounds kRoles[] = {
    {Role::Bass, kBass},
    {Role::Lead, kAcid},
    {Role::Pad, kDrone},
    {Role::Chords, kStab},
};

// ---- Drums lanes ----

constexpr TagWeight kKickTags[] = {
    {"909 Kick", 3}, {"Deep Sky Kick HQ", 1}, {"Deep Sky Kick distorted", 1}, {"808 Kick distorted", 1}};
constexpr TagWeight kSnareTags[] = {{"909", 3}, {"XV5080 Snare dry", 1}, {"DDD1 Snare dry", 1}};
constexpr TagWeight kClapTags[] = {{"909", 3}, {"GEN Clap", 2}, {"808", 1}};
constexpr TagWeight kRimTags[] = {{"909", 2}, {"808", 1}, {"DDD1", 1}};
// The Open hat first tries the Closed hat's tag (spec §10.3), so both list the same machines.
constexpr TagWeight kClosedHatTags[] = {{"909", 3}, {"808", 1}, {"DDD1", 1}};
constexpr TagWeight kOpenHatTags[] = {{"909", 3}, {"808", 1}, {"DDD1", 1}};
constexpr TagWeight kRideTags[] = {{"909", 3}, {"GEN Ride", 2}, {"XV5080 Ride edge", 1}};
constexpr TagWeight kCrashTags[] = {{"909", 3}, {"XV5080", 1}, {"DDD1", 1}};
constexpr TagWeight kTomTags[] = {{"909", 2}, {"808", 1}};
constexpr const char* const kPerc1Folders[] = {"SAMPLES/DRUMS/Claves", "SAMPLES/DRUMS/Cowbell", "SAMPLES/DRUMS/Other"};
constexpr TagWeight kPerc1Tags[] = {{"808", 2}, {"Metal", 1}, {"Click", 1}};
constexpr const char* const kPerc2Folders[] = {"SAMPLES/DRUMS/Shaker", "SAMPLES/DRUMS/Tambourine"};

// ---- FX lanes (spec §10.5) ----

// Riser: only pack risers, from its default folder SAMPLES/FX/Riser.
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser.XML"}};
constexpr const char* const kFallFolders[] = {"SAMPLES/Artists/Danny Taurus/MoDrama"};
constexpr TagWeight kFallTags[] = {{"Downer", 3}};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};
constexpr const char* const kImpactFolders[] = {"SAMPLES/DRUMS/Kick", "SAMPLES/Artists/Chaz"};
constexpr TagWeight kImpactTags[] = {
    {"Deep Sky Kick descending", 2}, {"Cassette808_BD03", 1}, {"808 Kick sustained", 1}};
constexpr PaletteEntry kImpactSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Impact.XML"}};

constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .tags = kKickTags},
    {.lane = Lane::Snare, .tags = kSnareTags},
    {.lane = Lane::Clap, .tags = kClapTags},
    {.lane = Lane::Rim, .tags = kRimTags},
    {.lane = Lane::ClosedHat, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .tags = kOpenHatTags},
    {.lane = Lane::Ride, .tags = kRideTags},
    {.lane = Lane::Crash, .tags = kCrashTags},
    {.lane = Lane::Tom, .tags = kTomTags},
    {.lane = Lane::Perc1, .folders = kPerc1Folders, .tags = kPerc1Tags},
    {.lane = Lane::Perc2, .folders = kPerc2Folders},
    {.lane = Lane::Riser, .synths = kRiserSynths},
    {.lane = Lane::Fall, .folders = kFallFolders, .tags = kFallTags, .synths = kFallSynths},
    {.lane = Lane::Impact, .folders = kImpactFolders, .tags = kImpactTags, .synths = kImpactSynths},
};

} // namespace

const Palette technoPalette{.parts = kParts, .roles = kRoles, .lanes = kLanes};

} // namespace deluge::generative::genres
