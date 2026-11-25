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
// Dub (#131, spec §4.5): roots dub, a genre of its own with no parent, not to be confused with Dub techno. One-drop
// drums (the kick and a cross-stick Rim on beats 2 and 4), an offbeat organ skank on every &, a bass that carries the
// tune in bursts anchored on the chord root, a melodica that breathes across 2-bar phrases, and drop-outs with delay
// Throws as the main motion, over a 4-bar Minor or Dorian chord loop. A Siren wails on Drops. Retuned in #133 round 2.

#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
/// The one drop: the kick on beats 2 and 4, with the Rim (spec §4.5, round 2 S1). A pickup on the a of 4 as Complexity rises.
constexpr Pattern kKick[] = {
    {"....X.......X...", "................", 0, 2},
    {"....X.......X..7", "................", 1},
};

/// Rockers (#460 S5 C): the kick anchored on 2 and 4, and on 1 and 3 at importance 8, so Density takes those away
/// first. As many entries as kKick, each as long, so a frozen Drums part keeps its pick across a Drop that changes the
/// style.
constexpr Pattern kKickRockers[] = {
    {"8...X...8...X...", "................", 0, 2},
    {"8...X...8...X..7", "................", 1},
};

/// Steppers (#460 S5 C): the kick on every 8th, 2 and 4 anchored.
constexpr Pattern kKickSteppers[] = {
    {"8.8.X.8.8.8.X.8.", "................", 0, 2},
    {"8.8.X.8.8.8.X.87", "................", 1},
};

/// The cross-stick on beats 2 and 4, anchored, at every tier: the Rim holds the backbeat and the Snare never does in
/// its step tables (it has none), so exactly one of them is on the drop at each tier. Beat 4 falls in the last beat so
/// the drum and Rim drop-outs' Throws always have a hit to spike on.
/// Ghosted pickups off the beat as Complexity rises.
constexpr Pattern kRim[] = {
    {"....X.......X...", "................", 0},
    {"....X.......X.7.", "................", 1},
    {".5..X.....5.X.7.", ".g........g.....", 2},
};

/// 8ths and 16ths with the off-beat &s ranked first (9, accented) so low Density keeps them (round 2 S1, S9).
constexpr Pattern kClosedHat[] = {
    {"8.9.8.9.8.9.8.9.", "..a...a...a...a.", 0, 2},
    {"8.968.968.968.96", "..ag..ag..ag..ag", 1, 2},
    {"8696869686968696", ".gag.gag.gag.gag", 1},
    {"8.968796869.8796", "..ag.gag.ga..gag", 2},
};

/// An open hat on the & of 4 now and then: it replaces the closed hat there (spec §5.7).
constexpr Pattern kOpenHat[] = {
    {"................", "................", 0, 2},
    {"..............7.", "................", 0},
    {"..............7.", "................", 1},
    {"......6.......7.", "................", 1},
};

/// The shaker on the 8ths (off-beat &s at 7 at tier 1 for the off-beat authoring rule, spec §8.1), behind its gate.
constexpr Pattern kShaker[] = {
    {"6.6.6.6.6.6.6.6.", "................", 0},
    {"6.766.766.766.76", "...g...g...g...g", 1},
};

/// Bongos and congas in a heartbeat pattern, behind their gate (round 2 S1, S8).
constexpr Pattern kHandDrums[] = {
    {"6.6.....6.6.....", "................", 0},
    {"6.6..5..6.6...7.", ".....g..........", 1},
};

// Fill patterns: only a Swap plays them. The Snare's holds beats 2 and 4 (the Rim and Snare Swap moves the backbeat
// there); the Rim's is empty, so the Rim falls silent under that Swap. The toms run over the last beats.
constexpr Pattern kSnareFill[] = {
    {"....X.......X...", "................", 0},
    {"....X.......X.7.", "................", 1},
};
constexpr Pattern kRimFill[] = {
    {"................", "................", 0},
};
constexpr Pattern kTomFill[] = {
    {"..........8.8.X.", "................", 0},
    {"........7.8.88XX", "........g.......", 1},
};

/// The bass line: two bars, anchored on each bar's 1 (always the chord root via rootOnAnchors), lines in bursts
/// leaving holes of 2 to 4 beats (round 2 S6; QM1 ghosts kept on the second tier-2 line's steps 21 and 25).
constexpr Pattern kBassLine[] = {
    {"X.....8.8.......X.....8.8.......", "................................", 0, 2},
    {"X.8.8...........X.8.8.....8.....", "................................", 0, 2},
    {"X.......8.8.....X.......8.8.8...", "................................", 0},
    {"X...8.8.........X...8.8.....8.8.", "................................", 0},
    {"X.....8.8.......X.....8.8.......", "................................", 1},
    {"X.8.8...........X.8.8.....8.....", "................................", 1},
    {"X.8...8.8.......X.8...8.8.....7.", "..............................g.", 1, 2},
    {"X.....8.8.8.....X..8..8.8.......", "................................", 1, 2},
    {"X.8.8...8.......X.8.8...8.8.....", "................................", 1},
    {"X..8..8.....8...X..8..8...8.8...", "................................", 1},
    {"X.....8.8...8.8.X.....8.8.......", "................................", 1},
    {"X.8.8.8...8.8...X.8.8.8.8...8.7.", "..............................g.", 2},
    {"X.8.7.8.7.8.8.8.X..8.7.8.7.8.8..", "....g...g............g...g......", 2},
};

/// The BASS set's rolling line (variant ideas 1): a fuller 8th-note line, onsets on the 8ths only so each 40-tick note
/// ends before the next (`longLoopNotesFit`). As many entries as the bursts pool, in the same tiers, so a frozen pick
/// reads the same entry across a live switch.
constexpr Pattern kBassRolling[] = {
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 0, 2},
    {"X.8.8.8.8...8.8.X.8.8.8.8...8.8.", "................................", 0, 2},
    {"X.8.8...8.8.8.8.X.8.8...8.8.8.8.", "................................", 0},
    {"X...8.8.8.8.8.8.X...8.8.8.8.8.8.", "................................", 0},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 1},
    {"X.8.8.8.8...8.8.X.8.8.8.8.8.8.8.", "................................", 1},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 1, 2},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 1, 2},
    {"X.8.8.8.8.8.8.8.X.8.8.8...8.8.8.", "................................", 1},
    {"X.8.8...8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 1},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 1},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 2},
    {"X.8.8.8.8.8.8.8.X.8.8.8.8.8.8.8.", "................................", 2},
};

/// The BASS set's heavy line (variant ideas 1): sparse long notes, onsets at least a beat apart so each 88-tick note
/// ends before the next. As many entries as the bursts pool, in the same tiers.
constexpr Pattern kBassHeavy[] = {
    {"X.......8.......X.......8.......", "................................", 0, 2},
    {"X...........8...X...........8...", "................................", 0, 2},
    {"X.......8.......X...............", "................................", 0},
    {"X.......8...8...X.......8...8...", "................................", 0},
    {"X.......8.......X.......8.......", "................................", 1},
    {"X...........8...X...........8...", "................................", 1},
    {"X.......8.......X...........8...", "................................", 1, 2},
    {"X...............X...............", "................................", 1, 2},
    {"X.......8...8...X.......8.......", "................................", 1},
    {"X...........8...X.......8.......", "................................", 1},
    {"X.......8.......X.......8...8...", "................................", 1},
    {"X.......8...8...X.......8.......", "................................", 2},
    {"X...8...8.......X.......8...8...", "................................", 2},
};

/// The skank: every &, anchored so a dip keeps it and the last & spikes its drop-out's Throw; its "chk-a" double on
/// the a after each as Complexity rises (round 2 S1).
constexpr Pattern kSkank[] = {
    {"..X...X...X...X.", "................", 0, 2},
    {"..X...X...X...X6", "................", 0},
    {"..X...X...X...X.", "................", 1, 2},
    {"..X6..X...X6..X.", "................", 1, 2},
    {"..X6..X6..X6..X6", "................", 1},
    {"..X6.5X6..X6.5X6", ".....g.......g..", 2},
};

/// The melodica's phrases: two bars, unanchored, bar 2 mostly empty so it breathes (round 2 S4). Every line has an
/// onset in bar 1's last beat at importance 8 so drop-outs starting after bar 1 throw it into its echo.
constexpr Pattern kMelodicaPhrases[] = {
    {"8........7...8.." "................", "................................", 0, 2},
    {"8......7.....8.." "8...............", "................................", 0},
    {"8...7....7...8.." "................", "................................", 1, 2},
    {"8....7...7...8.." "....8...........", "................................", 1},
    {".......7...7.8.." "8...............", "...........g....................", 1},
    {"8......7.....8.." "....8........8..", "................................", 1},
};
// clang-format on

/// A lazy snare: late on every 16th (spec §4.5).
constexpr Expression kLazySnare{.lean = {2, 2, 2, 2}, .spread = 8};
/// No extra hat lean: the song swing (+5 to +15) swings the 16ths directly (round 2 S1).
constexpr int8_t kHatLean = 0;

/// The drop-out and Throw echoes: a dotted 8th with the analog delay (spec §10.6, tuning).
constexpr DelaySetup kWideEcho{
    .on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true, .analog = true};
constexpr DelaySetup kRimEcho{.on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .analog = true};

constexpr int16_t kPercGate = macroPoints(35);

/// In the fixed lane order (spec §5.4). The Snare has no pool of its own: it plays Swaps, the Roll fill, the Build
/// roll, and 20 % of Groove bars in place of the Rim (Rim's alternateLane, #460 Q-RIM B); the Crash plays only the
/// Phrase accent; the Tom only the Swap.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 1}, .expression = {.spread = 4}},
    {.lane = Lane::Snare, .fillPool = kSnareFill, .band = {0, 1}, .expression = kLazySnare},
    {
        .lane = Lane::Rim,
        .pool = kRim,
        .fillPool = kRimFill,
        .band = {0, 2},
        .expression = {.spread = 8},
        .delay = kRimEcho, // the Rim and drum drop-outs' throw target
        .alternateLane = Lane::Snare,
        .alternateChance = 200, // ~1 in 5 Groove bars on Snare (tuning); Rim stays the norm (#460 Q-RIM B)
    },
    {
        .lane = Lane::ClosedHat,
        .pool = kClosedHat,
        .band = {0, 2},
        .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 10},
    },
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 6}},
    {.lane = Lane::Crash}, // the Phrase accent's lane
    {.lane = Lane::Tom, .fillPool = kTomFill, .band = {0, 1}},
    {
        .lane = Lane::Perc1,
        .pool = kShaker,
        .band = {0, 1},
        .densityGate = kPercGate,
        .expression = {.lean = {0, 0, kHatLean, 0}, .spread = 10},
    },
    {.lane = Lane::Perc2,
     .pool = kHandDrums,
     .band = {0, 1},
     .densityGate = macroPoints(28),
     .expression = {.spread = 10}},
};

/// The Drums lanes with another Kick pool: a drum style (#460 S5 C).
template <size_t N>
constexpr std::array<LaneSpec, N> withKick(const LaneSpec (&lanes)[N], std::span<const Pattern> kick) {
	std::array<LaneSpec, N> out{};
	for (size_t i = 0; i < N; i++) {
		out[i] = lanes[i];
	}
	out[0].pool = kick;
	return out;
}
static_assert(kDrumLanes[0].lane == Lane::Kick);
constexpr auto kRockersLanes = withKick(kDrumLanes, kKickRockers);
constexpr auto kSteppersLanes = withKick(kDrumLanes, kKickSteppers);

/// FX lanes have no pools: only section rules play them (spec §5.4). The Siren holds one note for 2 bars on a Break's
/// first step and on a Drop, gated at 30: at the default macros it sounds on Drops, whose lift takes Density to 33
/// (spec §9.5). Its release always throws, into its own echo.
constexpr LaneSpec kFxLanes[] = {
    {.lane = Lane::Riser},
    {.lane = Lane::Fall},
    {.lane = Lane::Impact},
    {.lane = Lane::Siren, .densityGate = macroPoints(30), .delay = kWideEcho, .heldBars = 2},
};

/// The Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};
/// On Drums the Throw acts on the Rim's row: it has the echo, and it is the drum drop-out's throw target.
constexpr GestureSpec kDrumGestures = [] {
	GestureSpec gestures = kGestures;
	gestures.aim[static_cast<int32_t>(Gesture::Throw)] = Lane::Rim;
	return gestures;
}();

/// Scale steps from the chord root, snapped to the Pentatonic set: mostly the root and the 5th, the 3rd, then the rest.
constexpr LineWeight kBassDegrees[] = {{0, 5}, {2, 2}, {4, 3}, {1, 1}, {3, 1}, {5, 1}, {6, 1}};
constexpr Answer kBassAnswers[] = {{AnswerKind::Octave}};

/// A line that leaps, an octave up now and then, anchored on the chord root, mutating slowly, never sliding; each note
/// held to the next onset's 8th (round 2 S2, S2b).
constexpr LoopSpec kBassLoop{
    .degrees = kBassDegrees,
    .draws = {.leap = {3, 6}, .octaveUp = {40, 150}, .octaveDown = 0, .accent = 120},
    .rootOnAnchors = true,
    .slide = 0,
    .mutation = 15,
    .answers = kBassAnswers,
    .answerBars = 8,
    .gateTicks = 40,
};

/// The heavy line's Loop: the root and the 5th only, each note held 88 ticks (5.5 steps, the most
/// `longLoopNotesFit` allows with onsets a beat apart). Both are read per note (`decideLoop()`, Avoid's plain gate),
/// so a live switch changes only the notes after the Drop.
constexpr LineWeight kHeavyDegrees[] = {{0, 5}, {4, 4}};
constexpr LoopSpec kHeavyLoop = [] {
	LoopSpec loop = kBassLoop;
	loop.degrees = kHeavyDegrees;
	loop.gateTicks = 88;
	return loop;
}();

/// Chord positions: mostly the root, the 3rd and the 5th, the 7th and the note under the root now and then.
constexpr LineWeight kMelodicaPositions[] = {{-1, 1}, {0, 4}, {1, 3}, {2, 3}, {3, 1}};

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kSkankPart = 2;
constexpr int32_t kMelodicaPart = 3;

/// Drums, Bass, Skank, Melodica and FX (spec §4.5). The sixth MUTE pad stays free.
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kDrumGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kBassLine,
        .band = {0, 2},
        // The Bass Register's default: the tonic sounds between F#1 and F2.
        .degreeSet = DegreeSet::Pentatonic,
        .loop = kBassLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Chords,
        .flavour = Flavour::Stab,
        .name = "SKANK",
        .pool = kSkank,
        .band = {0, 2},
        // Short, so each chop stands apart (tuning).
        .stab = {.gateTicks = 20},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // 80 % of the preset's volume knob (tuning): at the preset's level the skank sat about 3 dB under the rest of
        // the mix, over the bass (delugemu A/B capture, seed 1, a card with the pack).
        .volume = 80,
        .delay = kWideEcho, // the Skank and tops drop-outs' throw target
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "MELODICA",
        .pool = kMelodicaPhrases,
        .band = {0, 1},
        .motif = {.positions = kMelodicaPositions,
                  .draws = {.leap = {2, 3}, .octaveUp = {0, 100}, .accent = 150},
                  .reroll = 50,
                  .gateTicks = 168},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // 80 % of the preset's volume knob (tuning, round 2 I1: 70 too quiet, 100 too loud).
        .volume = 80,
        .delay = kWideEcho, // the Melodica drop-outs' throw target
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};
static_assert(kParts[kDrumsPart].role == Role::Drums);
static_assert(kParts[kBassPart].role == Role::Bass);
static_assert(kParts[kSkankPart].role == Role::Chords && kParts[kSkankPart].flavour == Flavour::Stab);
static_assert(kParts[kMelodicaPart].role == Role::Lead && kParts[kMelodicaPart].flavour == Flavour::Motif);

/// Every Drums lane the roster lists but the Kick.
constexpr uint16_t kDrumsButKick = laneBit(Lane::Snare) | laneBit(Lane::Rim) | laneBit(Lane::ClosedHat)
                                   | laneBit(Lane::OpenHat) | laneBit(Lane::Crash) | laneBit(Lane::Tom)
                                   | laneBit(Lane::Perc1) | laneBit(Lane::Perc2);

/// A Swap on Snare and Tom (held fill), a Swap on Rim and Snare moving the backbeat for the phrase's last bar, and a
/// rare 1-beat Snare Roll into the 1 (weight 1/9, down from round 2 s7-roll's 1/2; owner: "rare, but nice at times").
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Snare) | laneBit(Lane::Tom)}, .beats = 2, .weight = 4},
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Rim) | laneBit(Lane::Snare)}, .beats = 4, .weight = 4},
    {.kind = FillKind::Roll, .mask = {.lanes = laneBit(Lane::Snare)}, .beats = 1, .weight = 1},
};

/// Drop-outs and spot echoes (spec §4.5, §8.8; round 2 S3, S3b; #460 Q-THROW C): skank, melodica, drum-and-bass (tops
/// and FX out), drums but kick, hats and percussion, bass out, 1-bar spot echoes on Rim and Skank, and everything but
/// the bass (FX included). Each thrown target has its dotted-8th echo.
constexpr PartsAndLanes kSkankOnly{.parts = partBit(kSkankPart)};
constexpr PartsAndLanes kRimOnly{.lanes = laneBit(Lane::Rim)};
constexpr PartsAndLanes kMelodicaOnly{.parts = partBit(kMelodicaPart)};
constexpr uint16_t kAllFxLanes =
    laneBit(Lane::Riser) | laneBit(Lane::Fall) | laneBit(Lane::Impact) | laneBit(Lane::Siren);
constexpr PartsAndLanes kTopsAndFx{.parts = partBit(kSkankPart) | partBit(kMelodicaPart), .lanes = kAllFxLanes};
constexpr uint16_t kHatsAndPerc =
    laneBit(Lane::ClosedHat) | laneBit(Lane::OpenHat) | laneBit(Lane::Perc1) | laneBit(Lane::Perc2);
constexpr DropOutEntry kDropOuts[] = {
    {.mask = kSkankOnly, .bars = 2, .weight = 2, .throwTarget = kSkankOnly},
    {.mask = kSkankOnly, .bars = 4, .weight = 1, .throwTarget = kSkankOnly},
    {.mask = kMelodicaOnly, .bars = 4, .weight = 3, .throwTarget = kMelodicaOnly},
    // Drum and bass: the skank, melodica and FX out, the skank thrown (#460 Q-THROW C).
    {.mask = kTopsAndFx, .bars = 4, .weight = 3, .throwTarget = kSkankOnly},
    {.mask = {.lanes = kDrumsButKick}, .bars = 1, .weight = 1, .throwTarget = kRimOnly},
    // The drop alone under the band: hats and percussion out.
    {.mask = {.lanes = kHatsAndPerc}, .bars = 2, .weight = 1},
    // The bass out, to come back on a 1.
    {.mask = {.parts = partBit(kBassPart)}, .bars = 2, .weight = 1},
    // Spot echoes: one part or lane out for a bar, its last hit ringing through it.
    {.mask = kRimOnly, .bars = 1, .weight = 1, .throwTarget = kRimOnly},
    {.mask = kSkankOnly, .bars = 1, .weight = 1, .throwTarget = kSkankOnly},
    // Everything but the bass: drums, skank, melodica and FX out, the melodica thrown (#460 Q-THROW C).
    {.mask = {.parts = partBit(kSkankPart) | partBit(kMelodicaPart),
              .lanes = kDrumsButKick | laneBit(Lane::Kick) | kAllFxLanes},
     .bars = 4,
     .weight = 1,
     .throwTarget = kMelodicaOnly},
};

/// Home i, with the brief's moves one way each: i–iv, i–♭VII and iv–v. The home pull brings ♭VII and v back to i, a
/// repeat holds a chord on, and Dorian's colour move strengthens i–IV (spec §7.4). Weights in the colour moves' units.
constexpr ChordMove kDubMoves[] = {
    {kI, kIV, 30},  // i–iv (i–IV in Dorian)
    {kI, kVII, 30}, // i–♭VII
    {kIV, kV, 20},  // iv–v
};

/// One 4-bar chord loop for the whole piece, 1 bar a chord, repeat 20, no cadence target (spec §4.5, §7.4; round 2
/// S2/S2b).
constexpr ChordGraph kDubGraph{
    .homePull = 20,
    .moves = kDubMoves,
    .repeatWeight = 20,
    .barsPerChord = 1,
    .loopChance = 0,
    .halfLoopChance = 1000,
};

constexpr Scale kScales[] = {Scale::Minor, Scale::Dorian};

/// The Generator-wide pair, and the Crash replacing the closed hat (spec §5.7).
constexpr ReplacePair kReplacePairs[] = {{Lane::OpenHat, Lane::ClosedHat}, {Lane::Crash, Lane::ClosedHat}};

/// One bar: Dub barely builds (spec §4.5).
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// The Break's drum and bass: the Kick, the Rim and the Bass (spec §4.5).
constexpr PartsAndLanes kBreakKeeps{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick) | laneBit(Lane::Rim)};

/// The MIX set's tubby table (variant ideas 1): long drum-and-bass passages, the bass out more often, and everything
/// but the bass weighted up.
constexpr DropOutEntry kDropOutsTubby[] = {
    {.mask = kSkankOnly, .bars = 2, .weight = 1, .throwTarget = kSkankOnly},
    {.mask = kMelodicaOnly, .bars = 4, .weight = 2, .throwTarget = kMelodicaOnly},
    {.mask = kTopsAndFx, .bars = 4, .weight = 4, .throwTarget = kSkankOnly},
    {.mask = kTopsAndFx, .bars = 8, .weight = 3, .throwTarget = kSkankOnly},
    {.mask = {.lanes = kDrumsButKick}, .bars = 2, .weight = 2, .throwTarget = kRimOnly},
    {.mask = {.parts = partBit(kBassPart)}, .bars = 2, .weight = 3},
    {.mask = kRimOnly, .bars = 1, .weight = 1, .throwTarget = kRimOnly},
    {.mask = {.parts = partBit(kSkankPart) | partBit(kMelodicaPart),
              .lanes = kDrumsButKick | laneBit(Lane::Kick) | kAllFxLanes},
     .bars = 4,
     .weight = 4,
     .throwTarget = kMelodicaOnly},
};

/// The MIX set's light table (variant ideas 1): mostly 1-bar spot echoes.
constexpr DropOutEntry kDropOutsLight[] = {
    {.mask = kRimOnly, .bars = 1, .weight = 4, .throwTarget = kRimOnly},
    {.mask = kSkankOnly, .bars = 1, .weight = 4, .throwTarget = kSkankOnly},
    {.mask = kMelodicaOnly, .bars = 1, .weight = 2, .throwTarget = kMelodicaOnly},
    {.mask = kSkankOnly, .bars = 2, .weight = 1, .throwTarget = kSkankOnly},
};

/// The skank's chord (Q359, #359; ADR 0008): the 7ths every Skank stab played at the default Complexity before the
/// set, or plain triads (no Extension above the floor). The owner kept both ("both are good"), so they draw evenly.
/// It patches the genre's harmony, so the Melodica's chord tones lose the 7th with it. Live: the Extensions are read
/// only where a bar line voices the chord, and nothing reads them across a Drop's line before it (#495).
constexpr Variant kSkankVariants[] = {
    {.name = "sevenths", .oledName = "7THS", .weight = 1},
    {.name = "plain",
     .oledName = "PLAIN",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.genre.harmony.extensionCeiling = 0; }},
};

/// The drum style (#460 S5 C, ADR 0008): the one drop it played before the set, rockers or steppers, by the Kick's
/// pool. It may change at each Drop, never inside a Section; Perform may switch it there too. The weights 2:1:1 keep
/// the one drop the most common style (tuning).
constexpr Variant kDrumVariants[] = {
    {.name = "onedrop", .oledName = "ONE DROP", .weight = 2},
    {.name = "rockers",
     .oledName = "ROCKERS",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kDrumsPart].lanes = kRockersLanes; }},
    {.name = "steppers",
     .oledName = "STEPPERS",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kDrumsPart].lanes = kSteppersLanes; }},
};

/// The Break's length (Dub sign-off 1, BRK): 7 bars, the genre as written (a phrase, the 1-bar Build taking its last
/// bar), or 15 (two phrases), 2:1 toward the shorter (the owner, tuning). Not live: the Arc draws the Break's length
/// as it enters the Section before it, ahead of the line.
constexpr Variant kBreakVariants[] = {
    {.name = "seven", .oledName = "7 BARS", .weight = 2},
    {.name = "fifteen",
     .oledName = "15 BARS",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.genre.arc.breakPhrases = {2, 2}; }},
};

/// How hard the mix is cut (variant ideas 1, #532): the version as written, tubby (long drum-and-bass passages, chance
/// 450) or light (spot echoes, chance 120), 3:3:2 (the owner: often, often, sometimes). Live: the drop-out table and
/// its chance are read afresh at each draw, a bar ahead in a Groove, and no drop-out runs over a Drop's line.
constexpr Variant kMixVariants[] = {
    {.name = "version", .oledName = "VERSION", .weight = 3},
    {.name = "tubby",
     .oledName = "TUBBY",
     .weight = 3,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.genre.dropOuts = kDropOutsTubby;
	         resolved.genre.dropOutChance = 450;
         }},
    {.name = "light",
     .oledName = "LIGHT",
     .weight = 2,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.genre.dropOuts = kDropOutsLight;
	         resolved.genre.dropOutChance = 120;
         }},
};

/// The bass line's style (variant ideas 1, #532): bursts as written, a rolling 8th-note line, or heavy long notes on
/// the root and the 5th, evenly (the owner: often each). Live: each pool keeps the bursts pool's 13 entries in their
/// tiers and lengths, and the Loop's degrees and gate are read per note.
constexpr Variant kBassVariants[] = {
    {.name = "bursts", .oledName = "BURSTS", .weight = 3},
    {.name = "rolling",
     .oledName = "ROLLING",
     .weight = 3,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kBassPart].pool = kBassRolling; }},
    {.name = "heavy",
     .oledName = "HEAVY",
     .weight = 3,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.parts[kBassPart].pool = kBassHeavy;
	         resolved.parts[kBassPart].loop = kHeavyLoop;
         }},
};

constexpr VariantSet kVariantSets[] = {
    {.name = "skank", .oledName = "SKANK", .variants = kSkankVariants, .live = true},
    {.name = "drums",
     .oledName = "DRUMS",
     .variants = kDrumVariants,
     .live = true,
     .changeAt = ChangeAt::Drop,
     .changeChance = 400}, // per mille at each Drop (tuning)
    {.name = "break", .oledName = "BREAK", .variants = kBreakVariants},
    {.name = "mix", .oledName = "MIX", .variants = kMixVariants, .live = true},
    {.name = "bass", .oledName = "BASS", .variants = kBassVariants, .live = true},
};

constexpr Genre kDub{
    .name = "dub",
    .oledName = "ROOTS DUB", // never read as DUB TECHNO
    .version = 10,           // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 70, .tempoHighBpm = 80, .scales = kScales, .swingLow = 5, .swingHigh = 15},
    .phraseBars = 8,
    .parts = kParts,
    // The Crash replaces the closed hat on the Phrase accent's step, as the open hat does on its &: the Crash never
    // lands on a hat (spec §4.5, §5.7).
    .replacePairs = kReplacePairs,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 500,
    .dropOuts = kDropOuts,
    .dropOutChance = 250,
    .dropOutGapBars = 2,
    .harmony = {.graph = &kDubGraph, .extensionFloor = 0, .extensionCeiling = 1},
    .palette = &dubPalette,
    .arc =
        {
            .introPhrases = 1,
            .outroPhrases = 1,
            .groovePhrases = {4, 8},
            .breakPhrases = {1, 1},
            .buildBars = kBuildBars,
            .grooveToBuild = 0,
            .dropDecayPhrases = 1,
            .offsets =
                {
                    {macroPoints(-15), 0},                                                    // Intro
                    {},                                                                       // Groove
                    {macroPoints(-10), macroPoints(-10), macroPoints(-10), macroPoints(-10)}, // Break
                    {0, macroPoints(12), 0, macroPoints(10)},                                 // Build
                    {0, macroPoints(-15)},                                                    // Outro
                },
            // The breakdown is the drum and bass: the one drop and the line, Density dipping by 10, not 20, so the
            // bass keeps more than its anchors.
            .breakKeeps = kBreakKeeps,
            // No cut and no Gap: each would cover the whole 1-bar Build (cutAndGapUnderBuild()).
            .buildCutBars = 0,
            .gap = GapLength::None,
            // 16ths at most: under the song swing a straight 32nd roll warps into a gallop (Dub sign-off 1).
            .roll = {.lane = Lane::Snare, .top = RollStage::Sixteenth},
            .accentLane = Lane::Crash,
            .accentChance = 50, // a Crash on Drops, after a fill that ends a phrase, and at 50 per-mille
        },
    .variantSets = kVariantSets,
};
static_assert(genreValid(kDub), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre dub = kDub;

} // namespace deluge::generative::genres
