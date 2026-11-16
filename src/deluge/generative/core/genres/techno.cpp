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

// Techno (spec §4.5, ADR 0006): a parent genre, never played, and its subgenres. The parent holds what they share:
// straight time, a four-on-the-floor kick, offbeat hats and a rim, no claps on 2 and 4, the fills (a kick and bass
// drop, and a snare and tom run), the Arc and its section rules (a snare roll, the kick cut for the Build's last bar, a
// beat's Gap, a Crash accent on Drops and after fills), the FX part (Riser, Fall, Impact), the gestures, and the drum
// and FX lanes of the palette (techno_palette.cpp). Each subgenre sets its tempo range, scales, pitched parts, Harmony
// slot, Break keeps and palette. Every value is starting data, tuned by ear later (spec §21.5).
//
// Two of its four subgenres so far (#218–#221): Acid techno (#218), the default genre, and Melodic techno (#226, with
// its Pad from #123 and its Arp from #219).

#include "generative/core/genre.h"

#include <algorithm>
#include <array>
#include <iterator>

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

/// FX lanes have no pools: only section rules play them (spec §5.4).
constexpr LaneSpec kFxLanes[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};

/// Techno's Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};

/// The Techno Drums every subgenre plays, unless it needs its own kick (Melodic techno).
constexpr PartSpec kTechnoDrums{.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kGestures};

constexpr PartSpec kTechnoFx{.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures};

/// What every subgenre's roster starts and ends with. A subgenre lists its pitched parts between them, its bass first
/// (the fills' Rest drops part 1).
constexpr PartSpec kParentParts[] = {kTechnoDrums, kTechnoFx};

/// The kick drop with the bass (part 1 in every subgenre's roster), the main fill and the held-fill entry, and a snare
/// and tom run (spec §8.7).
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Rest, .mask = {.parts = partBit(1), .lanes = laneBit(Lane::Kick)}, .beats = 2, .weight = 3},
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 2, .weight = 1},
};

constexpr WeightedBars kBuildBars[] = {{8, 2}, {16, 1}};

/// The parent: never played, so it carries no version (spec §4.4). Its name is the group's in the GENRE menu and the
/// stem of its subgenres' names; no Recipe names it (findGenre() lists only playable genres).
constexpr Genre kTechnoParent{
    .name = "techno",
    .oledName = "TECHNO",
    .version = 0,
    .parts = kParentParts,
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
            .buildCut = {.lanes = laneBit(Lane::Kick)},
            .buildCutBars = 1,
            .gap = GapLength::Beat,
            .roll = {.lane = Lane::Snare, .top = RollStage::ThirtySecond},
            .accentLane = Lane::Crash,
        },
};

// ---- Acid techno (#218): a mutating acid Loop over the parent's drums, and a Drone under it ----

// clang-format off
/// The acid line's rhythm.
constexpr Pattern kBass[] = {
    {"2.8.2.8.2.8.2.8.", "................", 0},
    {"2.8.5.8.2.8.6.8.", "........g.......", 1, 2},
    {"..8.4.8...8.5.86", "....g.......g...", 1},
    {"3584659537846597", ".g.g.g.g...g.g..", 2},
};
// clang-format on

constexpr LineWeight kBassDegrees[] = {{0, 6}, {2, 1}, {3, 1}, {4, 2}, {6, 2}};

/// Octave and fifth Answers at low Complexity, reverse at the default, rotate at the top (spec §5.2).
constexpr Answer kBassAnswers[] = {
    {AnswerKind::Octave, 3}, {AnswerKind::Fifth}, {AnswerKind::Reverse}, {AnswerKind::Rotate}};

/// The acid line: mostly the root, leaping wider, jumping octaves and sliding more as Complexity rises. At the
/// default Complexity (25) it rolls as it did before Complexity shaped it.
constexpr LoopSpec kBassLoop{
    .degrees = kBassDegrees,
    .draws = {.leap = {2, 10}, .octaveUp = {100, 200}, .octaveDown = {60, 140}, .accent = 250},
    .slide = {120, 280},
    .mutation = 150,
    .answers = kBassAnswers,
    .answerBars = 4,
};

/// Drums, the acid Loop, the Drone and FX. The Drone is a pedal Sustain Pad: the key's root and the 5th above, held
/// through the piece (Acid has no Chord graph, so nothing changes its notes), re-struck only where a cut ends them (a
/// Section line, a key change, a new sound). Not gated, so it plays under everything from the Intro on, where Density
/// brings its 5th in after the root. It opens over the phrase with an 8-bar Brightness Sweep, as every Techno Pad does
/// (spec §4.5).
constexpr PartSpec kAcidParts[] = {
    kTechnoDrums,
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
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "DRONE",
        .sustain = {.pedal = true},
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
    },
    kTechnoFx,
};

constexpr Scale kAcidScales[] = {Scale::Minor, Scale::Phrygian};

/// The default genre (spec §4.5). Its drums, acid line and Arc play as Techno did before the split; the Drone is new
/// (acid_fingerprint_spec).
constexpr Genre kTechnoAcid = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-acid";
	genre.oledName = "ACID TECHNO";
	genre.shortName = "ACID";
	genre.facts.tempoLowBpm = 125;
	genre.facts.tempoHighBpm = 135;
	genre.facts.scales = kAcidScales;
	genre.parts = kAcidParts;
	genre.palette = &technoAcidPalette;
	// The breakdown is the Drone under the kick, rim and closed hat.
	genre.arc.breakKeeps = {.parts = partBit(2),
	                        .lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim) | laneBit(Lane::ClosedHat)};
	return genre;
}();
static_assert(kTechnoAcid.parts[1].role == Role::Bass, "the parent's Rest fill drops part 1, the bass");
static_assert(interplayRulesValid(kTechnoAcid), "every interplay rule reads an earlier part or lane (spec §5.5)");
static_assert(loopNotesValid(kTechnoAcid), "every Loop note overlaps only the next onset's (spec §8.5, §8.6)");

// ---- Melodic techno (#226, #123, #219): the Chord graph walked under a rolling bass, a Sustain Pad and an Arp ----

// clang-format off
/// Four on the floor at every Complexity: the parent's pickups off the beats would land on the rolling bass's 16ths.
constexpr Pattern kFourOnTheFloor[] = {
    {"X...X...X...X...", "................", 0},
};

/// The three 16ths after each beat, never the beat itself: the kick's steps stay empty, so no Avoid rule is needed.
constexpr Pattern kRollingBass[] = {
    {".888.888.888.888", "................", 0, 2},
    {".888.888.888.88.", "................", 0},
    {".878.878.878.878", "...g...g...g...g", 1, 2},
    {".888.878.888.868", "...............g", 1},
    {".887.887.887.886", "...g...g...g...g", 2},
};

/// One bar of 16ths, each tier with its own walk (kArpWalks). At the default Density the 8s and 9s play, the 8ths and
/// the syncopated 16ths of tiers 1–2; more Density fills in the rest. The 1 is an anchor, so the Break's dip never
/// thins the Arp to nothing.
constexpr Pattern kArpLead[] = {
    {"X384838493848384", "a.......a.......", 0, 2}, // 8ths, filling in to 16ths
    {"X485848494858484", "a...............", 0},
    {"X387.87.9387.874", "a.......a.......", 1}, // syncopated pairs of 16ths
    {"X.786.789.786.78", "a.......a.......", 1},
    {"X787678797876787", "a...a...a...a...", 2}, // 16ths throughout
    {"X868786898687868", "a.......a.......", 2},
};
// clang-format on

/// Mostly the root and its octave, the 5th now and then.
constexpr LineWeight kRollingDegrees[] = {{0, 9}, {4, 2}};
constexpr Answer kRollingAnswers[] = {{AnswerKind::Octave, 3}, {AnswerKind::Fifth}};

/// Rare slides, a tight gate, and a line that changes slowly: the bass rolls rather than talks.
constexpr LoopSpec kRollingLoop{
    .degrees = kRollingDegrees,
    .draws = {.leap = 4, .octaveUp = {150, 300}, .octaveDown = 0, .accent = 150},
    .slide = {20, 50},
    .mutation = 60,
    .answers = kRollingAnswers,
    .answerBars = 8,
    .gateTicks = 14,
};

/// Each kArpLead entry's walk (spec §4.5): Up or Up-down over one octave in the plain tiers, over two in the busier.
constexpr ArpWalk kArpWalks[] = {
    {.order = ArpOrder::Up, .octaves = 1},     // tier 0
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 0
    {.order = ArpOrder::Up, .octaves = 2},     // tier 1
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 1
    {.order = ArpOrder::UpDown, .octaves = 2}, // tier 2
    {.order = ArpOrder::Up, .octaves = 2},     // tier 2
};
static_assert(std::size(kArpWalks) == std::size(kArpLead), "a walk for each Arp pool entry");

/// The parent's drum lanes, but the kick holds only the beats.
constexpr auto kMelodicDrumLanes = [] {
	std::array<LaneSpec, std::size(kDrumLanes)> lanes{};
	std::copy(std::begin(kDrumLanes), std::end(kDrumLanes), lanes.begin());
	static_assert(kDrumLanes[0].lane == Lane::Kick);
	lanes[0].pool = kFourOnTheFloor;
	lanes[0].band = {0, 0};
	return lanes;
}();

constexpr PartSpec kMelodicDrums = [] {
	PartSpec drums = kTechnoDrums;
	drums.lanes = kMelodicDrumLanes;
	return drums;
}();

/// Drums, a rolling bass, a Pad, an Arp and FX: the parent's Drums with a four-on-the-floor kick, and its FX part. The
/// Pad is a Sustain voicing each chord (re-struck every 2 bars as the walk moves), with the Pad Register's default
/// span and no doubled root, so it never plays more than the chord's 4 notes. The Arp walks the same chord on 16ths
/// above it. Neither is gated: Density thins them instead. The Pad opens over the phrase with an 8-bar Brightness
/// Sweep, as every Techno Pad does, and the Arp with a 16-bar one (spec §4.5).
constexpr PartSpec kMelodicParts[] = {
    kMelodicDrums,
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kRollingBass,
        .band = {0, 2},
        .loop = kRollingLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "PAD",
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
        // Half the preset's volume knob (tuning): a 4-note chord at the preset's level sat over the bass and lead,
        // and the player lowered it every time (user's listening, 2026-11).
        .volume = 50,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Arp,
        .name = "ARP",
        .pool = kArpLead,
        .band = {0, 2},
        // 34 semitones (arpRange(2)): a two-octave walk from any chord root, starting between 3 below the tonic and
        // 8 above it, over the Pad.
        .reg = {.anchor = 62, .spanLow = -3, .spanHigh = 31},
        // Short notes, so each 16th speaks on its own (tuning); the preset's release rings on.
        .arp = {.walks = kArpWalks, .gateTicks = 18},
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
    },
    kTechnoFx,
};

/// Home i; i–♭VI–♭VII–i is the main loop, with iv on the way to ♭VI and a short cut from i to ♭VII. Weights in the
/// colour moves' units (20).
constexpr ChordMove kMelodicMoves[] = {
    {kI, kVI, 30},   // i–♭VI
    {kVI, kVII, 30}, // ♭VI–♭VII
    {kVII, kI, 30},  // ♭VII–i
    {kI, kIV, 20},   // i–iv
    {kIV, kVI, 20},  // iv–♭VI
    {kI, kVII, 20},  // i–♭VII
};

/// 2 bars a chord, the cadence on ♭VII every 4 chords. In Harmonic Minor ♭VII is diminished, so the guard swaps the
/// cadence to V and its leading tone (spec §4.5, §7.4).
constexpr ChordGraph kMelodicGraph{
    .homePull = 10,
    .moves = kMelodicMoves,
    .cadenceTarget = kVII,
    .cadencePeriod = 4,
    .barsPerChord = 2,
};

constexpr Scale kMelodicScales[] = {Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor};

constexpr Genre kTechnoMelodic = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-melodic";
	genre.oledName = "MELODIC TECHNO";
	genre.shortName = "MELODIC";
	genre.facts.tempoLowBpm = 122;
	genre.facts.tempoHighBpm = 128;
	genre.facts.scales = kMelodicScales;
	genre.parts = kMelodicParts;
	genre.harmony = {.graph = &kMelodicGraph, .extensionCeiling = 1};
	genre.palette = &technoMelodicPalette;
	// The breakdown is the Pad and the Arp alone, over the closed hat.
	genre.arc.breakKeeps = {.parts = partBit(2) | partBit(3), .lanes = laneBit(Lane::ClosedHat)};
	// The Pad's root and the Arp's 1 are its only anchors, so the parent's Density dip (-20) would leave the Break
	// almost bare at the default Density: dip Density by 5, so the Arp's 8ths still play and the Pad keeps its 5th and
	// 3rd. Energy dips as the parent's does.
	SectionOffsets& breakDip = genre.arc.offsets[static_cast<int32_t>(SectionKind::Break)];
	breakDip.densityFrom = macroPoints(-5);
	breakDip.densityTo = macroPoints(-5);
	return genre;
}();
static_assert(kTechnoMelodic.parts[1].role == Role::Bass, "the parent's Rest fill drops part 1, the bass");
static_assert(interplayRulesValid(kTechnoMelodic), "every interplay rule reads an earlier part or lane (spec §5.5)");
static_assert(loopNotesValid(kTechnoMelodic), "every Loop note overlaps only the next onset's (spec §8.5, §8.6)");
static_assert(harmonyValid(kTechnoMelodic), "the Chord graph's rhythm fits the phrase (spec §7.4)");
static_assert(arpNotesValid(kTechnoMelodic), "the Arp's Register holds its walks, and its notes overlap only the next "
                                             "onset's (spec §5.2, §7.3)");

} // namespace

const Genre technoAcid = kTechnoAcid;
const Genre technoMelodic = kTechnoMelodic;

} // namespace deluge::generative::genres
