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

#include <algorithm>
#include <array>
#include <iterator>

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

// ---- Acid techno (#218) ----

constexpr PartSounds kAcidParts[] = {{"ACID", kAcid}};
/// The DRONE (root and fifth, held for whole Sections) takes the drone pads by its role, so an override file's Pad
/// role reaches it: 148's and 149's built-in 5th stacks another open fifth over it (spec §18.3: intervals suit
/// drones), 126 and 139 keep it plain.
constexpr RoleSounds kAcidRoles[] = {
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

// ---- Melodic techno (#226) ----

/// The rolling bass: short, plucky presets with no long tail, so the 16ths stay apart. 000's env2 filter pluck is the
/// main pick; 012's sync sweep and 050's plain FM vary it.
constexpr PaletteEntry kRollingBassSounds[] = {
    {"SYNTHS/000 Rich Saw Bass.XML", 3},
    {"SYNTHS/047 Basic Dirty Bass.XML", 2},
    {"SYNTHS/012 Blunt Sync Bass.XML", 1},
    {"SYNTHS/050 FM Basic Bass.XML", 1},
};
/// The Arp: short FM plucks and an arp lead (its saved arp cleared at load, spec §10.6), releases under a second and
/// one to three unison voices, so 16ths on a POLY voice stay within the voice budget. No stacked interval, pitch
/// wobble or heavy FX, and none of the acid presets (factory card research §2.5). 167's metallic FM is an arp sound
/// with a 0.5 s release; 058's FM pluck and 068's FM bell vary it; 067's mallet sits an octave lower.
constexpr PaletteEntry kArpLeadSounds[] = {
    {"SYNTHS/167 FM Metallic Bass Arp.XML", 3},
    {"SYNTHS/058 FM Rising Attack.XML", 2},
    {"SYNTHS/068 FM Bells 1.XML", 2},
    {"SYNTHS/067 Marimba.XML", 1},
};
/// The Pad, re-struck every 2 bars (about 3.8 s): soft pads with attacks well under the chord's length, releases under
/// 2 s, and no built-in interval (a stacked 5th, 3rd or 2nd turns a 4-note chord into a cluster), saved arp, heavy FX
/// or off-key tuning (factory card research §2.5). 153's soft chorused FM is the main pick: it attacks in 0.1 s, so
/// each chord speaks at once, and settles at sustain 20, under the bass and lead. 139's detuned saws swell in over
/// 1.1 s, and 151's FM in 1.8 s, blooming within the chord. 142's phaser pad is the occasional variation. All four have
/// one or two unison voices, so a 4-note chord and its release stay within the voice budget.
constexpr PaletteEntry kMelodicPad[] = {
    {"SYNTHS/153 FM Modulation Pad.XML", 3},
    {"SYNTHS/139 Detuned Saw Pad.XML", 2},
    {"SYNTHS/151 Radiant FM Pad.XML", 2},
    {"SYNTHS/142 Phaser.XML", 1},
};

constexpr PartSounds kMelodicParts[] = {{"BASS", kRollingBassSounds}, {"PAD", kMelodicPad}, {"ARP", kArpLeadSounds}};
constexpr RoleSounds kMelodicRoles[] = {
    {Role::Bass, kRollingBassSounds},
    {Role::Pad, kMelodicPad},
    {Role::Lead, kArpLeadSounds},
};

// ---- Dub techno (#221) ----

/// The sub: round, plain basses that hold a long note, all transposed two octaves down, so the Sub's Register (C4)
/// puts each one's root in the same octave. 002's round square is the main pick; 050's plain FM and 047's plain dirty
/// bass vary it. (004 Dubby Bass, a dub bass by name, is transposed one octave down: it would sound an octave higher.)
constexpr PaletteEntry kSubSounds[] = {
    {"SYNTHS/002 Basic Square Bass.XML", 3},
    {"SYNTHS/050 FM Basic Bass.XML", 2},
    {"SYNTHS/047 Basic Dirty Bass.XML", 1},
};
/// The Stab: chord presets that carry their own echo, until delay setups and delay Throws arrive with Dub (#130).
/// 037's fifth on every note and its delay make the classic dub-techno chord (with one chord the stacked fifth does
/// no harm); 082 is a short stab into its own delay; 072 an FM pluck with a delay tail.
constexpr PaletteEntry kDubStabSounds[] = {
    {"SYNTHS/037 Echo Chord.XML", 3},
    {"SYNTHS/082 Short Sharp Delay.XML", 2},
    {"SYNTHS/072 Kyoto Phono.XML", 1},
};
/// The pedal Pad, root and fifth held for whole Sections: slow, dark pads with no transpose. 131's strings swell in
/// over 3 s, a wash; 139's detuned saws over 1.1 s; 126's dark FM stacks another fifth, harmless on a pedal.
constexpr PaletteEntry kDubPadSounds[] = {
    {"SYNTHS/131 Warm Strings.XML", 3},
    {"SYNTHS/139 Detuned Saw Pad.XML", 2},
    {"SYNTHS/126 Dark FM Pad.XML", 2},
};

constexpr PartSounds kDubParts[] = {{"SUB", kSubSounds}, {"STAB", kDubStabSounds}, {"PAD", kDubPadSounds}};
constexpr RoleSounds kDubRoles[] = {
    {Role::Bass, kSubSounds},
    {Role::Chords, kDubStabSounds},
    {Role::Pad, kDubPadSounds},
};

/// A shaker alone on Perc 2: the tambourine's jingle is too bright under the stab's ghosts.
constexpr const char* const kShakerFolders[] = {"SAMPLES/DRUMS/Shaker"};

/// Techno's lanes, but Perc 2.
constexpr auto kDubLanes = [] {
	std::array<LaneSounds, std::size(kLanes)> lanes{};
	std::copy(std::begin(kLanes), std::end(kLanes), lanes.begin());
	for (LaneSounds& lane : lanes) {
		if (lane.lane == Lane::Perc2) {
			lane.folders = kShakerFolders;
		}
	}
	return lanes;
}();

// ---- Detroit techno (#220) ----

/// The root-rhythm bass: plain, short basses, all transposed two octaves down, so the Bass's Register (C4) puts each
/// one's root in the same octave. 047's dirty saw is the main pick; 007's detuned saw, 050's plain FM and 012's sync
/// sweep vary it.
constexpr PaletteEntry kRootBassSounds[] = {
    {"SYNTHS/047 Basic Dirty Bass.XML", 3},
    {"SYNTHS/007 Detuned Saw Bass.XML", 2},
    {"SYNTHS/050 FM Basic Bass.XML", 1},
    {"SYNTHS/012 Blunt Sync Bass.XML", 1},
};
/// The Strings, re-struck with each chord every 2 bars (about 3.6 s): string pads with no transpose and no built-in
/// interval (a stacked 5th or 3rd turns a 7th chord into a cluster). 019's string pad speaks at once (0.08 s attack)
/// with a 2 s tail; 134 swells in over half a second; 131's 3 s swell barely opens within a chord, the occasional
/// variation.
constexpr PaletteEntry kStringsSounds[] = {
    {"SYNTHS/019 Fizzy Strings.XML", 3},
    {"SYNTHS/134 Melody String.XML", 2},
    {"SYNTHS/131 Warm Strings.XML", 1},
};
/// The Stab: chord presets with no transpose and no built-in interval, short enough for 16ths (factory card research
/// §2.5). 086's organ stab is the main pick; 083's chorused ringmod and 088's ringmod with a pitch yelp vary it, and
/// 071's Rhodes softens it.
constexpr PaletteEntry kDetroitStabSounds[] = {
    {"SYNTHS/086 Techno Organ.XML", 3},
    {"SYNTHS/083 Dark Chorus.XML", 2},
    {"SYNTHS/088 Yelp Chords.XML", 1},
    {"SYNTHS/071 Rhodes.XML", 1},
};
/// The Motif: bells and plain leads with no transpose, releases under a second, so a riff's notes stay apart. 069's
/// FM bell is the main pick; 068's brighter bell, 021's plain square and 058's FM pluck vary it.
constexpr PaletteEntry kMotifSounds[] = {
    {"SYNTHS/069 FM Bells 2.XML", 3},
    {"SYNTHS/068 FM Bells 1.XML", 2},
    {"SYNTHS/021 80s TV Lead.XML", 1},
    {"SYNTHS/058 FM Rising Attack.XML", 1},
};

constexpr PartSounds kDetroitParts[] = {
    {"BASS", kRootBassSounds}, {"STRINGS", kStringsSounds}, {"STAB", kDetroitStabSounds}, {"MOTIF", kMotifSounds}};
/// Each part's presets under its role too, so an override file's role reaches it.
constexpr RoleSounds kDetroitRoles[] = {
    {Role::Bass, kRootBassSounds},
    {Role::Pad, kStringsSounds},
    {Role::Chords, kDetroitStabSounds},
    {Role::Lead, kMotifSounds},
};

} // namespace

const Palette technoPalette{.lanes = kLanes};
/// The acid line and the Drone over Techno's drum and FX lanes.
const Palette technoAcidPalette{.parts = kAcidParts, .roles = kAcidRoles, .lanes = kLanes};
/// Its own bass, Pad and Arp over Techno's drum and FX lanes.
const Palette technoMelodicPalette{.parts = kMelodicParts, .roles = kMelodicRoles, .lanes = kLanes};
/// Its sub, Stab and Pad over Techno's drum and FX lanes, a shaker alone on Perc 2.
const Palette technoDubPalette{.parts = kDubParts, .roles = kDubRoles, .lanes = kDubLanes};
/// Its bass, Strings, Stab and Motif over Techno's drum and FX lanes.
const Palette technoDetroitPalette{.parts = kDetroitParts, .roles = kDetroitRoles, .lanes = kLanes};

} // namespace deluge::generative::genres
