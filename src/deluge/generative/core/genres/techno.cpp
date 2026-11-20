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
// Its four subgenres (#218–#221): Acid techno (#218), the default genre, Melodic techno (#226, with its Pad from #123
// and its Arp from #219), Dub techno (#221) and Detroit techno (#220).

#include "generative/core/capacity.h"
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
/// stem of its subgenres' names; no Recipe names it (findGenre() lists only playable genres). Its Break keeps the
/// kick, rim and closed hat, so a subgenre that lists no Break keeps of its own breaks down to those, not a full
/// Groove; each subgenre adds its pitched parts or sets its own.
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
            .breakKeeps = {.lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim) | laneBit(Lane::ClosedHat)},
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
        // Volume 50 (tuning), the knob at half the preset's position, is about -12 dB as played: at the preset's level
        // the held root and fifth sat about 5 dB over the rest of the mix together (delugemu capture, seed 1).
        .volume = 50,
    },
    kTechnoFx,
};

/// A Break that dips Density by 5 rather than the parent's 20, for a subgenre whose kept parts have few anchors, so
/// the parent's dip would thin the Break to almost nothing at the default Density (tuning). Energy dips as the
/// parent's does.
constexpr void softBreakDip(Genre& genre) {
	SectionOffsets& breakDip = genre.arc.offsets[static_cast<int32_t>(SectionKind::Break)];
	breakDip.densityFrom = macroPoints(-5);
	breakDip.densityTo = macroPoints(-5);
}

constexpr Scale kAcidScales[] = {Scale::Minor, Scale::Phrygian};

/// The default genre (spec §4.5). Its drums and acid line play as Techno did before the split, with a Drone under them
/// and a soft Break dip (#120 round 1).
constexpr Genre kTechnoAcid = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-acid";
	genre.oledName = "ACID TECHNO";
	genre.shortName = "ACID";
	genre.version = 2; // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
	genre.facts.tempoLowBpm = 125;
	genre.facts.tempoHighBpm = 135;
	genre.facts.scales = kAcidScales;
	genre.parts = kAcidParts;
	genre.palette = &technoAcidPalette;
	// The breakdown is the Drone under the parent's kick, rim and closed hat.
	genre.arc.breakKeeps.parts = partBit(2);
	// With the parent's dip the Drone lost its 5th and the drums thinned to a few hits (tuning, #120 round 1).
	softBreakDip(genre);
	return genre;
}();
static_assert(technoRosterValid(kTechnoAcid),
              "Drums first, the bass second (the parent's Rest fill drops part 1), FX last");
static_assert(genreValid(kTechnoAcid),
              "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

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
        // Volume 50 (tuning), the knob at half the preset's position, is about -12 dB as played: a 4-note chord at the
        // preset's level sat over the bass and lead, and the player lowered it every time (user's listening, 2026-11).
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
	genre.version = 3; // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
	genre.facts.tempoLowBpm = 122;
	genre.facts.tempoHighBpm = 128;
	genre.facts.scales = kMelodicScales;
	genre.parts = kMelodicParts;
	genre.harmony = {.graph = &kMelodicGraph, .extensionCeiling = 1};
	genre.palette = &technoMelodicPalette;
	// The breakdown is the Pad and the Arp alone, over the closed hat.
	genre.arc.breakKeeps = {.parts = partBit(2) | partBit(3), .lanes = laneBit(Lane::ClosedHat)};
	// The Pad's root and the Arp's 1 are its only anchors: with the soft dip the Arp's 8ths still play and the Pad
	// keeps its 5th and 3rd.
	softBreakDip(genre);
	return genre;
}();
static_assert(technoRosterValid(kTechnoMelodic),
              "Drums first, the bass second (the parent's Rest fill drops part 1), FX last");
static_assert(genreValid(kTechnoMelodic),
              "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

// ---- Dub techno (#221): one m7 or m9 chord stabbed on the & over a sub bass and a pedal Pad, with lighter drums ----

// clang-format off
/// The rim's anchor on the a of 2, so a Break that keeps it still plays it; ghosts around it as Complexity rises.
constexpr Pattern kDubRim[] = {
    {".......X........", "................", 0, 2},
    {".......X......4.", "..............g.", 0},
    {"...4...X...5..X.", "...g.......g....", 1},
    {".......X.3..5..X", ".........g..g...", 1},
};

/// Offbeat hats with no 16ths between them: Techno's tier 1–2 16th runs would crowd the stab's ghosts.
constexpr Pattern kDubClosedHat[] = {
    {"..8...8...8...8.", "................", 0, 2},
    {"..8...8...8...83", "...............g", 0},
    {"..8.3.8...8.3.8.", "....g.......g...", 1},
    {"..8..48...8..48.", ".....g.......g..", 2},
};

constexpr Pattern kDubOpenHat[] = {
    {"..............6.", "................", 0, 2},
    {"......6.......6.", "................", 0},
};

/// The shaker on Perc 2: the e and the a around the hats' &, then 16ths with the & pushed.
constexpr Pattern kShaker[] = {
    {".5.8.5.8.5.8.5.8", ".g...g...g...g..", 0, 2},
    {".4.8.4.8.4.8.4.9", ".g.g.g.g.g.g.g..", 0},
    {"3586358635863586", "gg.ggg.ggg.ggg.g", 1},
};

/// Long notes, each held 6 steps (kSubLoop's gate), so every gap is 6 steps or more, the bar line counted: the 1 and the
/// 3, the 1 and the & of 3, the & of 2 pushed, and two bars with a late answer.
constexpr Pattern kSub[] = {
    {"X.......X.......", "................", 0, 2},
    {"X.........X.....", "................", 0},
    {"X.....8.........", "................", 1},
    {"X.........8.....", "................", 1},
    {"X.........X.....X.....8.........", "................................", 2},
};

/// The & and the 16th after it, its ghost (spec §4.5): an anchored & every bar, and ghosted repeats on later &s and
/// a's, as the chord's echo would fall, more of them as Complexity rises.
constexpr Pattern kDubStab[] = {
    {"......X8......5.", ".......g......g.", 0, 2},
    {"..X8..........5.", "...g..........g.", 0},
    {"......X8..6...47", ".......g..g...gg", 1},
    {"..X8......X8....", "...g.......g....", 1},
    {"..X8..6...X8..47", "...g..g....g..gg", 2},
    {"......X8..6.......5.......X8..4.", ".......g..g.......g........g..g.", 2},
};
// clang-format on

/// The parent's drum lane `lane`, for a subgenre's own Drums part to take as it is.
constexpr LaneSpec parentLane(Lane lane) {
	for (const LaneSpec& spec : kDrumLanes) {
		if (spec.lane == lane) {
			return spec;
		}
	}
	throw "the parent's Drums have no such lane";
}

/// Kick, rim, hats and a shaker, with the parent's snare and tom (their fill pools for its Swap and Build roll) and
/// its Crash for the accent. The kick holds the beats at every Complexity.
constexpr LaneSpec kDubDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kFourOnTheFloor},
    parentLane(Lane::Snare), // the Build roll's lane, and a Swap's
    {.lane = Lane::Rim, .pool = kDubRim, .band = {0, 1}, .expression = {.spread = 10, .spreadMode = SpreadMode::Fresh}},
    {
        .lane = Lane::ClosedHat,
        .pool = kDubClosedHat,
        .band = {0, 2},
        .rules = kHatsOffKick,
        .expression = {.lean = {0, 1, -2, 1}, .spread = 12},
    },
    {
        .lane = Lane::OpenHat,
        .pool = kDubOpenHat,
        .rules = kHatsOffKick,
        .expression = {.lean = {0, 0, -2, 0}, .spread = 6},
    },
    parentLane(Lane::Crash), // the Phrase accent's lane
    parentLane(Lane::Tom),   // only a Swap plays it
    {.lane = Lane::Perc2, .pool = kShaker, .band = {0, 1}, .expression = {.lean = {0, 2, 0, 2}, .spread = 14}},
};

constexpr PartSpec kDubDrums = [] {
	PartSpec drums = kTechnoDrums;
	drums.lanes = kDubDrumLanes;
	return drums;
}();

constexpr LineWeight kSubDegrees[] = {{0, 1}};

/// The root alone, now and then an octave down, never sliding, each note held for 6 steps (tuning): every kSub note
/// ends before the next starts, so the sub plays one note at a time (loopNotesValid()).
constexpr LoopSpec kSubLoop{
    .degrees = kSubDegrees,
    .draws = {.leap = 0, .octaveUp = 0, .octaveDown = {80, 200}, .accent = 100},
    .slide = 0,
    .mutation = 60,
    .gateTicks = 6 * kTicksPerStep,
};

/// Drums, a sub bass, the Stab, a pedal Pad and FX. The Stab plays the home chord, m7 or m9 (Extension floor 1), its
/// sound moving over a 16-bar Brightness Sweep; the Pad holds the key's root and fifth under it, with an 8-bar one,
/// as every Techno Pad has (spec §4.5).
constexpr PartSpec kDubParts[] = {
    kDubDrums,
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "SUB",
        .pool = kSub,
        .band = {0, 2},
        // The tonic within 6 semitones of C2, and room for the octave under it: the root sounds between F#1 and F2.
        .reg = {.anchor = 36, .spanLow = -12, .spanHigh = 7},
        .loop = kSubLoop,
        .expression = {.spread = 4},
        .gestures = kGestures,
    },
    {
        .role = Role::Chords,
        .flavour = Flavour::Stab,
        .name = "STAB",
        .pool = kDubStab,
        .band = {0, 2},
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
        // The preset's volume knob (tuning): soft by itself, its stabs sat about 5 dB under the drums and sub
        // together (delugemu capture, seed 1).
        .volume = 100,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "PAD",
        // An octave under the Pad Register's default, so the root and fifth wash under the Stab.
        .reg = {.anchor = 48, .spanLow = -12, .spanHigh = 12},
        .sustain = {.pedal = true},
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
        // Volume 50 (tuning), the knob at half the preset's position, is about -12 dB as played: at the preset's level
        // the held root and fifth sat about 2 dB over the rest of the mix together (delugemu capture, seed 1).
        .volume = 50,
    },
    kTechnoFx,
};

constexpr Scale kDubScales[] = {Scale::Minor, Scale::Dorian};

constexpr Genre kTechnoDub = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-dub";
	genre.oledName = "DUB TECHNO";
	genre.shortName = "DUB";
	genre.version = 2; // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
	genre.facts.tempoLowBpm = 115;
	genre.facts.tempoHighBpm = 125;
	genre.facts.scales = kDubScales;
	genre.parts = kDubParts;
	// No Chord graph: the home chord throughout, always with a 7th, and a 9th as Complexity rises (spec §7.4).
	genre.harmony = {.extensionFloor = 1, .extensionCeiling = 2};
	genre.palette = &technoDubPalette;
	// The breakdown is the Stab and the Pad over the rim.
	genre.arc.breakKeeps = {.parts = partBit(2) | partBit(3), .lanes = laneBit(Lane::Rim)};
	return genre;
}();
static_assert(technoRosterValid(kTechnoDub),
              "Drums first, the bass second (the parent's Rest fill drops part 1), FX last");
static_assert(genreValid(kTechnoDub), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

// ---- Detroit techno (#220): a Chord graph that matters, under a root-rhythm bass, Strings, a Stab and a Motif ----

// clang-format off
/// Root rhythm: the beats and the & before the next, the 1 an anchor; a 16th pickup into a beat with Complexity.
constexpr Pattern kRootRhythm[] = {
    {"X.8.X.8.X.8.X.8.", "................", 0, 2},
    {"X.8.X.8.X.8.6.8.", "................", 0},
    {"X.8.X.86X.8.X.87", ".......g.......g", 1, 2},
    {"X.8.6.87X.8.X.86", "....g..g.......g", 1},
    {"X.8.X.87X.86X.87", ".......g...g...g", 2},
};

/// Syncopated 16ths that keep off the 1: the a of 1 anchored, so the Break's Density dip still plays it, then &s and
/// a's; more 16ths between them as Complexity rises.
constexpr Pattern kDetroitStab[] = {
    {"...X..8...8.....", "................", 0, 2},
    {"...X..8..7..8...", ".........g......", 0},
    {"...X..8...8..7.6", ".............g.g", 1, 2},
    {"..7X..8..6.8..7.", "..g......g....g.", 1},
    {"...X.78..7.8.X.6", ".....g...g.....g", 2},
    {"...X..8...8..7.....X..8..7..8.6.", ".............g..................", 2},
};

/// One- and two-bar riffs, each opening on its anchored 1, stored as chord positions so they follow each change.
constexpr Pattern kDetroitRiffs[] = {
    {"X..8..X...8.X...", "................", 0, 2},
    {"X..8..8.X..8..7.X..8..8...X.....", "................................", 0},
    {"X..8..X.7.8.X.6.", "..........g...g.", 1},
    {"X..8..X..7..X.8.X..8..X...8..7.6", "..............g................g", 1},
    {"X.78..X.7.8.X.68", "..g.......g...g.", 2},
    {"X..8.7X..7.8X.6.X..8..X.7.8..9.6", ".....g.....g...................g", 2},
};
// clang-format on

/// The root, its 5th with Complexity (a leap of 4, from Complexity 34), and its octave by the octave jumps. Its only
/// Answer is the octave, so the 5th never plays at Complexity 0.
constexpr LineWeight kRootDegrees[] = {{0, 8}, {4, 2}};
constexpr Answer kRootAnswers[] = {{AnswerKind::Octave}};

/// The root on the beat, now and then its octave and 5th as Complexity rises, rare slides, short notes.
constexpr LoopSpec kRootLoop{
    .degrees = kRootDegrees,
    .draws = {.leap = {0, 6}, .octaveUp = {0, 250}, .octaveDown = 0, .accent = 120},
    .slide = {0, 40},
    .mutation = 60,
    .answers = kRootAnswers,
    .answerBars = 8,
    .gateTicks = 14,
};

/// Chord positions: mostly the root, the 3rd and the 5th, the 7th and the root an octave up, now and then the note
/// under the root.
constexpr LineWeight kRiffPositions[] = {{-1, 1}, {0, 4}, {1, 3}, {2, 3}, {3, 2}, {4, 1}};

/// Drums, a root-rhythm bass, Strings, a Stab, a Motif and FX: the parent's Drums and FX. The Strings are a Sustain Pad
/// voicing each chord, with the 8-bar Brightness Sweep every Techno Pad has (spec §4.5); the Stab plays the same chord
/// on syncopated 16ths; the Motif's riff follows each chord, gated above the default Density so the Arc brings it in
/// on Drops.
constexpr PartSpec kDetroitParts[] = {
    kTechnoDrums,
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kRootRhythm,
        .band = {0, 2},
        // The Bass Register's default: the tonic sounds between F#1 and F2. Its octave jumps go an octave over it.
        .loop = kRootLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "STRINGS",
        // Every chord with its Extension (Done-when): the 7th, or the 9th, plays with the root at any Density.
        .sustain = {.anchorExtension = true},
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
        // 60 % of the preset's volume knob (tuning): at the preset's level a 4-note chord sat less than 2 dB under the
        // rest of the mix together (delugemu capture, seed 1, Density 35).
        .volume = 60,
    },
    {
        .role = Role::Chords,
        .flavour = Flavour::Stab,
        .name = "STAB",
        .pool = kDetroitStab,
        .band = {0, 2},
        // Short stabs, each 16th on its own (tuning).
        .stab = {.gateTicks = 36},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // 80 % of the preset's volume knob (tuning): at the preset's level its stabs sat 5 dB under the rest of the
        // mix together, the Strings at full (delugemu capture, seed 1, Density 35).
        .volume = 80,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "MOTIF",
        .pool = kDetroitRiffs,
        .band = {0, 2},
        // Over the default Density (25), so it comes in on Drops, whose lift takes Density to 33. With the gate's
        // hysteresis (2) it stays open while any lift is left and closes on the bar where the lift has decayed to the
        // centre: the whole Drop and none of the Groove (tuning, #120 round 1).
        .densityGate = macroPoints(27),
        .motif = {.positions = kRiffPositions,
                  .draws = {.leap = {2, 4}, .octaveUp = {0, 150}, .accent = 150},
                  .reroll = 100,
                  .gateTicks = 36},
        .expression = {.spread = 6},
        .gestures = kGestures,
        // The preset's volume knob (tuning): its bell sat 20 dB under the rest of the mix (delugemu capture, seed 1,
        // Density 35), and no part plays above its preset's level.
        .volume = 100,
    },
    kTechnoFx,
};

/// Home i, with the brief's moves, each played both ways (the way back to i by the home pull), so every chord the walk
/// reaches has a way on that isn't home, and a phrase's last chord, which excludes home, never stalls into a repeat.
/// Dorian adds i–IV through its colour move. Weights in the colour moves' units (20).
constexpr ChordMove kDetroitMoves[] = {
    {kI, kVII, 30},  // i–♭VII
    {kI, kIV, 25},   // i–iv
    {kI, kV, 15},    // i–v
    {kIV, kV, 20},   // iv–v
    {kV, kIV, 10},   // v–iv
    {kVI, kVII, 30}, // ♭VI–♭VII
    {kVII, kVI, 20}, // ♭VII–♭VI
};

/// 2 bars a chord and no repeats; no cadence target, so the phrase rule (home off a phrase's last chord) and the home
/// pull shape the loop (spec §4.5, §7.4).
constexpr ChordGraph kDetroitGraph{
    .homePull = 20,
    .moves = kDetroitMoves,
    .repeatWeight = 0,
    .barsPerChord = 2,
};

constexpr Scale kDetroitScales[] = {Scale::Minor, Scale::Dorian};

constexpr Genre kTechnoDetroit = [] {
	Genre genre = subgenreOf(kTechnoParent);
	genre.name = "techno-detroit";
	genre.oledName = "DETROIT TECHNO";
	genre.shortName = "DETROIT";
	genre.version = 3; // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
	genre.facts.tempoLowBpm = 125;
	genre.facts.tempoHighBpm = 135;
	genre.facts.scales = kDetroitScales;
	genre.parts = kDetroitParts;
	// Every chord with a 7th, usually, and a second Extension as Complexity rises (spec §7.4).
	genre.harmony = {.graph = &kDetroitGraph, .extensionFloor = 1, .extensionCeiling = 2};
	genre.palette = &technoDetroitPalette;
	// The breakdown is the Strings and the Stab over the closed hat.
	genre.arc.breakKeeps = {.parts = partBit(2) | partBit(3), .lanes = laneBit(Lane::ClosedHat)};
	// The closed hat has no anchors, and the parent's dip would thin it out of the Break at the default Density.
	softBreakDip(genre);
	return genre;
}();
static_assert(technoRosterValid(kTechnoDetroit),
              "Drums first, the bass second (the parent's Rest fill drops part 1), FX last");
static_assert(genreValid(kTechnoDetroit),
              "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre technoAcid = kTechnoAcid;
const Genre technoMelodic = kTechnoMelodic;
const Genre technoDub = kTechnoDub;
const Genre technoDetroit = kTechnoDetroit;

} // namespace deluge::generative::genres
