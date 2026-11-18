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
// Dub's built-in palette (slot 8, spec §10.1, §18.3): the factory card content draft (§6.3 of the research), with the
// pack's dub sub and melodica as the Bass's and Melodica's main picks, `GEN Siren` alone on the Siren lane and
// `GEN Riser Short` on the Riser lane (spec §18.2). Pitched parts load at whole-sound transpose 0 (ADR 0007), so a
// preset's saved octave doesn't matter. Weights: 3 the main pick, 2 common, 1 an occasional variation. Every entry is
// starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// ---- Synth parts ----

/// The bass that carries the tune: the pack's soft sine sub first, then 004 (round, named for the job), the plain
/// square and the sampled upright. Not the draft's 006 Vaporwave Bass: its saved portamento glides every leap, and
/// Dub's bass never slides.
constexpr PaletteEntry kBass[] = {
    {"SYNTHS/GENERATIVE/GEN Dub Sub.XML", 3},
    {"SYNTHS/004 Dubby Bass.XML", 2},
    {"SYNTHS/002 Basic Square Bass.XML", 1},
    {"SYNTHS/169 Double Bass.XML", 1},
};
/// The skank: percussive organ first, then a short stab feeding its own delay, the organs and a piano.
constexpr PaletteEntry kSkank[] = {
    {"SYNTHS/077 FM Perc-Organ.XML", 3}, {"SYNTHS/082 Short Sharp Delay.XML", 2},
    {"SYNTHS/026 PW Organ.XML", 2},      {"SYNTHS/073 Piano.XML", 1},
    {"SYNTHS/076 Organ.XML", 1},
};
/// The melodica: the pack's reed first, then the nearest factory reeds, the dub horn and the panpipes.
constexpr PaletteEntry kMelodica[] = {
    {"SYNTHS/GENERATIVE/GEN Melodica.XML", 3},
    {"SYNTHS/064 Reeds-Flute-Oboe.XML", 2},
    {"SYNTHS/062 Trumpet.XML", 1},
    {"SYNTHS/061 Blown-Staccato-Panpipes.XML", 1},
};

constexpr PartSounds kParts[] = {{"BASS", kBass}, {"SKANK", kSkank}, {"MELODICA", kMelodica}};
/// Each role's presets too, so an override file's role reaches its parts.
constexpr RoleSounds kRoles[] = {
    {Role::Bass, kBass},
    {Role::Chords, kSkank},
    {Role::Lead, kMelodica},
};

// ---- Drums lanes: the draft's machines by tag ----

/// A round, short one-drop kick: the plain 808 kick by its path, not its distorted or sustained ones.
constexpr TagWeight kKickTags[] = {{"SAMPLES/DRUMS/Kick/808 Kick.wav", 2}, {"CR78", 1}, {"Linndrum", 1}, {"R100", 1}};
/// The cross-stick backbeat.
constexpr TagWeight kRimTags[] = {{"808", 2}, {"CR78", 2}, {"Linndrum", 1}, {"XV5080", 1}};
/// Snares that carry their own room, and a dry one.
constexpr TagWeight kSnareTags[] = {{"wet", 2}, {"XV5080 Snare dry", 1}};
constexpr TagWeight kClosedHatTags[] = {{"CR78", 2}, {"808", 2}, {"Linndrum", 1}};
/// The Closed hat's machines, so the Open hat pairs with the closed one it rolled (spec §10.3): the draft's two and
/// Linndrum, so the lane has 3 files.
constexpr TagWeight kOpenHatTags[] = {{"CR78", 2}, {"808", 1}, {"Linndrum", 1}};
/// The Drop's Crash: the draft lists none, so the machines the hats come from.
constexpr TagWeight kCrashTags[] = {{"808", 2}, {"CR78", 1}, {"XV5080", 1}};
/// Simmons toms, the nearest to a syndrum.
constexpr TagWeight kTomTags[] = {{"SDS5", 3}, {"808", 1}};
constexpr const char* const kPerc1Folders[] = {"SAMPLES/DRUMS/Shaker", "SAMPLES/DRUMS/Claves"};
constexpr TagWeight kPerc1Tags[] = {{"Cabasa", 2}, {"Claves", 1}};
/// The RX-5's bongos live in `Block`.
constexpr const char* const kPerc2Folders[] = {"SAMPLES/DRUMS/Block", "SAMPLES/DRUMS/ConL", "SAMPLES/DRUMS/ConM",
                                               "SAMPLES/DRUMS/ConH"};
constexpr TagWeight kPerc2Tags[] = {{"Bongo", 2}, {"Conga", 1}};

// ---- FX lanes (spec §10.5) ----

/// Riser: the pack's short riser alone, by tag in its default folder SAMPLES/FX/Riser: the only one that fits the
/// 1-bar Build at 80 BPM (3 s), so it ends on the Drop (spec §10.5's Riser fit). The pack's synth Riser if it's
/// missing.
constexpr TagWeight kRiserTags[] = {{"GEN Riser Short", 1}};
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser.XML"}};
constexpr const char* const kFallFolders[] = {"SAMPLES/Artists/Danny Taurus/MoDrama"};
constexpr TagWeight kFallTags[] = {{"Downer", 2}};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};
/// The dub gunshot, and a factory boom (spec §18.3).
constexpr const char* const kImpactFolders[] = {"SAMPLES/Artists/JS-Clipping", "SAMPLES/DRUMS/Kick"};
constexpr TagWeight kImpactTags[] = {{"Gunshot", 2}, {"WaterJug-Hit", 1}, {"808 Kick descending", 1}};
constexpr PaletteEntry kImpactSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Impact.XML"}};
/// The Siren: always the pack's preset (spec §10.5). It lists no folder, and its default `SAMPLES/FX/Siren` holds no
/// files, so it loads through the FX lane's synth fallback.
constexpr PaletteEntry kSirenSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Siren.XML"}};

constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .tags = kKickTags},
    {.lane = Lane::Snare, .tags = kSnareTags},
    {.lane = Lane::Rim, .tags = kRimTags},
    {.lane = Lane::ClosedHat, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .tags = kOpenHatTags},
    {.lane = Lane::Crash, .tags = kCrashTags},
    {.lane = Lane::Tom, .tags = kTomTags},
    {.lane = Lane::Perc1, .folders = kPerc1Folders, .tags = kPerc1Tags},
    {.lane = Lane::Perc2, .folders = kPerc2Folders, .tags = kPerc2Tags},
    {.lane = Lane::Riser, .tags = kRiserTags, .synths = kRiserSynths},
    {.lane = Lane::Fall, .folders = kFallFolders, .tags = kFallTags, .synths = kFallSynths},
    {.lane = Lane::Impact, .folders = kImpactFolders, .tags = kImpactTags, .synths = kImpactSynths},
    {.lane = Lane::Siren, .synths = kSirenSynths},
};

} // namespace

/// Its Bass, Skank and Melodica over the one-drop drums and the Riser, Fall, Impact and Siren.
const Palette dubPalette{.parts = kParts, .roles = kRoles, .lanes = kLanes};

} // namespace deluge::generative::genres
