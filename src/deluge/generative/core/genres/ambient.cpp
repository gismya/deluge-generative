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

// Ambient (#125, spec §4.5): a genre of its own, with no parent. Pads and a drone over a slow Chord graph, a held sub,
// sparse bells and soft percussion behind a high Density gate. Its Arc swells and recedes and never drops (spec §9.5):
// no gesture moves any part, so Reset has nothing to snap, and the Build's top equals the Drop lift, so nothing falls
// or closes on the Drop's step. Every value is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/capacity.h"
#include "generative/core/genre.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
/// Soft percussion: sparse two-bar patterns, more hits as Complexity rises. Each lane sits behind its own gate, from 28
/// to 47 (kDrumLanes), so at the default Density none plays and a raised one brings them in one at a time.
constexpr Pattern kKick[] = {
    {"X...............................", "................................", 0, 2},
    {"X.........................7.....", "................................", 1, 2},
    {"X.........7.....................", "................................", 1},
    {"X.........7.....", "................", 2},
};

/// Brushes: one stroke in two bars, then 2 and 4 with ghosted strokes as Complexity rises.
constexpr Pattern kBrush[] = {
    {"............................7...", "................................", 0, 2},
    {"....7.................7.........", "................................", 1, 2},
    {"............6.........7.........", "................................", 1},
    {"....7..3....7.7.", ".......g........", 2},
};

constexpr Pattern kRim[] = {
    {"......................6.........", "................................", 0},
    {"...5......7.....", "...g............", 1},
};

constexpr Pattern kClosedHat[] = {
    {"......6.......5.......6.........", "................................", 0, 2},
    {"..7...7...7...7.", "................", 1},
};

/// Mallets and pings: one hit in two bars, a few around it with Complexity.
constexpr Pattern kMallets[] = {
    {"........7.......................", "................................", 0, 2},
    {"..............6.........7.......", "................................", 0},
    {"..7.....7.....4.", "..............g.", 1},
};

/// Wood hits, off the beat.
constexpr Pattern kWood[] = {
    {"...........6....................", "................................", 0, 2},
    {"...6.......................5....", "................................", 0},
    {"...7..4....5..3.", "......g.......g.", 1},
};

/// The Bass: one note on the 1 of each two bars, held to the pattern's end (kSubLoop's gate). A Loop's gate is one
/// value for its part, so every pattern is two bars long.
constexpr Pattern kSub[] = {
    {"X...............................", "................................", 0},
};

/// The Lead's bells: two-bar figures with no anchor, about one note in two bars at the default Density, so whole bars
/// stay empty; the 5s play only as Density rises, the busier tiers add a few 8ths. Each tier 1+ figure keeps an
/// off-beat note at 8 or 9, which the default Density plays (spec §4.5, §8.1).
constexpr Pattern kBells[] = {
    {"9...............................", "................................", 0, 2},
    {"..........9.....................", "................................", 0},
    {"9.....................5.........", "................................", 0, 2},
    {"......9.........5...............", "................................", 0},
    {"5.........8...........5.........", "................................", 1, 2},
    {"....5.........8.................", "................................", 1},
    {"5.....8.........................", "................................", 1, 2},
    {"..9.....5.......5...............", "................................", 1},
    {"5...5.....8.....................", "................................", 2, 2},
    {"9.....5...8.........5...........", "................................", 2},
    {"....9...5.....8.......5.........", "................................", 2},
    {"9.5...8.....5.....5...5.........", "................................", 3},
    {"5...5.5...8.....5.....7.....5...", "................................", 3},
};
// clang-format on

constexpr int16_t kLaneGate = macroPoints(35);

/// Six soft lanes with no Ornaments, staggered so they arrive one at a time: mallets first, the kick last.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 2}, .densityGate = macroPoints(47), .expression = {.spread = 8}},
    {.lane = Lane::Snare, .pool = kBrush, .band = {0, 2}, .densityGate = macroPoints(44), .expression = {.spread = 10}},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}, .densityGate = macroPoints(40), .expression = {.spread = 8}},
    {.lane = Lane::ClosedHat,
     .pool = kClosedHat,
     .band = {0, 1},
     .densityGate = macroPoints(36),
     .expression = {.spread = 12}},
    // 29, not 28: the Build's top and the Drop lift reach 28 at the default macros (owner, round 1's Q336: none there).
    {.lane = Lane::Perc1,
     .pool = kMallets,
     .band = {0, 1},
     .densityGate = macroPoints(29),
     .expression = {.spread = 10}},
    {.lane = Lane::Perc2, .pool = kWood, .band = {0, 1}, .densityGate = macroPoints(32), .expression = {.spread = 10}},
};

/// Riser and Fall, each gated at 35; no Impact lane (spec §4.5).
constexpr LaneSpec kFxLanes[] = {
    {.lane = Lane::Riser, .densityGate = kLaneGate},
    {.lane = Lane::Fall, .densityGate = kLaneGate},
};

/// No gesture on any part (spec §9.5): Close, Wash, Rise and every Throw moment switched off, so Reset, which has no
/// switch, has no gesture offset to snap on the Drop or the first Groove. The sound moves by the macro layer, drift and
/// Sweeps alone.
constexpr uint16_t kAllMoments = (1U << kNumGestureMoments) - 1;
constexpr GestureSpec kNoGestures{.off = kAllMoments};

/// Drift ×1.5 on the Pad, the Drone and the Lead (spec §4.5).
constexpr int16_t kSlowDrift = 1500;

constexpr LineWeight kRootOnly[] = {{0, 1}};

/// The root alone, never sliding and with no octave down (owner, round 1's I2: the lowest octave is mud); each note
/// held two bars, to the pattern's end, ending before the next pick may start (loopNotesValid()).
constexpr LoopSpec kSubLoop{
    .degrees = kRootOnly,
    .draws = {.leap = 0, .octaveUp = 0, .octaveDown = 0, .accent = 0},
    .slide = 0,
    .mutation = 60,
    .gateTicks = 2 * kTicksPerBar,
};

/// Chord positions: mostly the root, the 3rd and the 5th, then the Extensions (spec §4.5).
constexpr LineWeight kBellPositions[] = {{-1, 1}, {0, 4}, {1, 3}, {2, 3}, {3, 2}, {4, 1}};

/// Small leaps, an octave up now and then, each note sounding to the next onset or the bar line.
constexpr MotifSpec kBellsMotif{
    .positions = kBellPositions,
    .draws = {.leap = {3, 4}, .octaveUp = {50, 200}, .octaveDown = 0, .accent = 100},
    .reroll = 350,
    .gateTicks = kTicksPerBar,
};

/// Drums, Bass, Pad, Drone, Lead and FX: the largest roster's six parts, a second Pad sized as Chords (rosterFits()).
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kNoGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kSub,
        .band = {0, 0},
        // Over the Intro floor (the Drone's 0), so it enters as the Intro's Density rises (tuning).
        .densityGate = macroPoints(15),
        // The key's root between F#2 and F3, the Drone's own (anchor C3: owner, Ambient sounds round 1's bass-up), and
        // every chord root at or over it, up to a 7th, so the Bass never sounds under F#2 (about 92 Hz; round 1's I2:
        // the octave down, under 45 Hz, was mud). The semitone under the root makes the Register 12 wide; it is the 7th
        // degree, no Ambient chord's root.
        .reg = {.anchor = 48, .spanLow = -1, .spanHigh = 11},
        .loop = kSubLoop,
        .expression = {.spread = 0},
        .gestures = kNoGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "PAD",
        // Each chord with its Extensions, no doubled root, held with no retrigger, voiced open: the root and the 5th
        // near the tonic, the 3rd and the Extensions an octave up, each voice then moving the least (rules review S9).
        // Over the Pad Register's default, whose −12 voiced close clusters down to G#2 and into the Drone. −4, not −5:
        // a 5th under the tonic would meet the Drone's 5th in unison where the Pad's tonic is an octave over the
        // Drone's. Lowest note G3–F#4 by key; +19 gives the upper voices their octave, the top reaching the bells'.
        .reg = {.anchor = 64, .spanLow = -4, .spanHigh = 19},
        // Held across Section lines, the Drop's included, and at every chord change: only a changed pitch strikes
        // (spec §5.2, §9.5).
        .sustain = {.holdsAcrossSections = true, .holdsCommonTones = true, .openVoicing = true},
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift, .sweepBars = 16},
        // With the Drone at its preset's level, 145 Eerie High Pad clipped the mix (tuning, spec §4.5).
        .volume = 70,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "DRONE",
        // An octave under the Pad Register's default (anchor C3), so the key's root and fifth sit under the Pad.
        .reg = {.anchor = 48, .spanLow = -12, .spanHigh = 12},
        .sustain = {.pedal = true, .holdsAcrossSections = true},
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift},
        // About 3 dB under the rest, so the Pad swells over it (tuning, spec §4.5).
        .volume = 60,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "BELLS",
        .pool = kBells,
        .band = {0, 3},
        // Under the Break's Density at the default macros (25 − 15), so the Break keeps the bells down to a Density of
        // 22 (owner, round 1's QBG, #340); the brief's "about 20" would close them there. They enter as the Intro's
        // Density passes 8 (tuning).
        .densityGate = macroPoints(8),
        .motif = kBellsMotif,
        .expression = {.spread = 10, .spreadMode = SpreadMode::Fresh},
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift},
        .volume = 100,
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kNoGestures},
};

constexpr int32_t kBassPart = 1;
constexpr int32_t kPadPart = 2;
constexpr int32_t kDronePart = 3;
constexpr int32_t kLeadPart = 4;

/// Layers come and go inside a Groove: the bells, the Bass or both rest for 4 or 8 bars; the Pad and the Drone never
/// do.
constexpr DropOutEntry kDropOuts[] = {
    {.mask = {.parts = partBit(kLeadPart)}, .bars = 4, .weight = 3},
    {.mask = {.parts = partBit(kLeadPart)}, .bars = 8, .weight = 3},
    {.mask = {.parts = partBit(kBassPart)}, .bars = 4, .weight = 2},
    {.mask = {.parts = partBit(kBassPart)}, .bars = 8, .weight = 3},
    {.mask = {.parts = partBit(kLeadPart) | partBit(kBassPart)}, .bars = 4, .weight = 1},
    {.mask = {.parts = partBit(kLeadPart) | partBit(kBassPart)}, .bars = 8, .weight = 2},
};

/// Home I, the brief's moves one way each; the home pull and the repeat bring the walk back (spec §4.5). Weights in
/// the colour moves' units (20). Lydian's and Dorian's colour moves come from the table (spec §7.4).
constexpr ChordMove kMoves[] = {
    {kI, kIV, 25},  // I–IV
    {kI, kVI, 25},  // I–vi
    {kIV, kI, 25},  // IV–I
    {kVI, kIV, 25}, // vi–IV
    {kI, kV, 10},   // I–V, rare
    // No ii–IV, the brief's: only Lydian reaches ii (its colour move I–II), where IV is #iv°, which the guard closes.
    {kII, kI, 25}, // ii–I: so Lydian's II has a way out besides the home pull
    {kVI, kI, 25}, // vi–I: Lydian's vi has no other way out once the guard closes vi–IV on #iv°; in Major it splits
                   // vi's exits between IV and I
    {kV, kI, 25},  // V–I: without it V leaves only by the home pull (10) against the repeat (40), and holds ~40 bars
};

/// 8 bars a chord, one a phrase, and a heavy repeat, so a chord often holds 16 or 24 bars. No cadence target.
constexpr ChordGraph kGraph{
    .homePull = 40, // owner, round 1's Q338 (#338): at 10, Dorian's IV held too long
    .moves = kMoves,
    .repeatWeight = 40,
    .barsPerChord = 8,
};

constexpr Scale kScales[] = {Scale::Major, Scale::Lydian, Scale::Dorian};

constexpr WeightedBars kBuildBars[] = {{16, 1}};

constexpr Genre kAmbient{
    .name = "ambient",
    .oledName = "AMBIENT",
    .version = 2, // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 60, .tempoHighBpm = 90, .scales = kScales, .swing = 0},
    .phraseBars = 8,
    .parts = kParts,
    .repickChance = 400,
    .dropOuts = kDropOuts,
    .dropOutChance = 150,
    .dropOutGapBars = 4,
    // No fills, so FILL does nothing.
    // Guard factor 0: Lydian's IV is #iv°, its Bass a tritone from the Drone, so I–IV and vi–IV never land
    // there; Dorian's vi is vi°, so I–vi and vi–IV never play in Dorian either (spec §7.4).
    // add9 and sus4 before the 7th: no dominant 7th over the Drone, V's leading tone gone at level 2 (rules review S8).
    .harmony = {.graph = &kGraph,
                .extensionFloor = 1,
                .extensionCeiling = 2,
                .extensionOrder = kNinthFirst,
                .guard = {.factor = 0}},
    .palette = &ambientPalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 2,
            .groovePhrases = {4, 8},
            .breakPhrases = {2, 4},
            .buildBars = kBuildBars,
            .grooveToBuild = 150,
            .dropDecayPhrases = 2,
            // The Build's top equals the Drop lift for Energy and Density (spec §9.5): neither falls on the Drop's
            // step, and no gate closes on its bar line.
            .dropLiftEnergy = macroPoints(3),
            .dropLiftDensity = macroPoints(3),
            .offsets =
                {
                    {macroPoints(-15), 0},                                                    // Intro
                    {},                                                                       // Groove
                    {macroPoints(-10), macroPoints(-10), macroPoints(-15), macroPoints(-15)}, // Break: a dip
                    {0, macroPoints(3), 0, macroPoints(3)},                                   // Build: a swell
                    {0, macroPoints(-15)},                                                    // Outro
                },
            // The Break recedes to the Pad, the Drone and the bells; the Build keeps everything, so its Density ramp
            // brings the parts back one at a time through their gates.
            .breakKeeps = {.parts = partBit(kPadPart) | partBit(kDronePart) | partBit(kLeadPart)},
            .buildKeeps = kEverything,
            .buildCutBars = 0,
            .gap = GapLength::None,
            .roll = {.lane = Lane::None},
            .accentLane = Lane::None,
            .transitionEntry = TransitionEntry::Intro,
        },
};
static_assert(genreValid(kAmbient), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre ambient = kAmbient;

} // namespace deluge::generative::genres
