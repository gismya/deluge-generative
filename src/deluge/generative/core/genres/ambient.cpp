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
/// Soft percussion: sparse one-bar patterns, more hits as Complexity rises. Every lane sits behind Density 35, so at
/// the default Density only a raised Density brings it in.
constexpr Pattern kKick[] = {
    {"X.......5.......", "................", 0, 2},
    {"X.........7.....", "................", 1},
    {"X......4..7.....", ".......g........", 2},
};

/// Brushes on 2 and 4, ghosted strokes between them with Complexity.
constexpr Pattern kBrush[] = {
    {"....7.......7...", "................", 0, 2},
    {"....7..3....7.7.", ".......g........", 1},
    {"..3.7..3..7.7.4.", "..g....g......g.", 2},
};

constexpr Pattern kRim[] = {
    {"......6.......5.", "................", 0},
    {"...5......7.....", "...g............", 1},
};

constexpr Pattern kClosedHat[] = {
    {"..6...6...6...6.", "................", 0, 2},
    {"5.7.5.7.5.7.5.7.", "g...g...g...g...", 1},
};

/// Mallets and pings: one hit on beat 3, a few around it with Complexity.
constexpr Pattern kMallets[] = {
    {"........7.......", "................", 0, 2},
    {"..7.....7.....4.", "..............g.", 1},
};

/// Wood hits, off the beat.
constexpr Pattern kWood[] = {
    {"...6.......5....", "................", 0, 2},
    {"...7..4....5..3.", "......g.......g.", 1},
};

/// The Bass: one note on the 1 of each two bars, held to the pattern's end (kSubLoop's gate). A Loop's gate is one
/// value for its part, so every pattern is two bars long.
constexpr Pattern kSub[] = {
    {"X...............................", "................................", 0},
};

/// The Lead's bells: tier 0 on the 1 and the & of 3, one and two bars; the busier tiers add a few 8ths (spec §4.5).
constexpr Pattern kBells[] = {
    {"X.........8.....", "................", 0, 2},
    {"X.........8.....X...............", "................................", 0},
    {"X.....7...8.....", "................", 1},
    {"X.........8.....X.....6.........", "................................", 1},
    {"X...6.....8...5.", "....g.........g.", 2},
    {"X.6...7.4.8...6.", "..g.....g.....g.", 3},
};
// clang-format on

constexpr int16_t kLaneGate = macroPoints(35);

/// Six soft lanes, every one gated at 35, with no Ornaments.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 2}, .densityGate = kLaneGate, .expression = {.spread = 8}},
    {.lane = Lane::Snare, .pool = kBrush, .band = {0, 2}, .densityGate = kLaneGate, .expression = {.spread = 10}},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}, .densityGate = kLaneGate, .expression = {.spread = 8}},
    {.lane = Lane::ClosedHat,
     .pool = kClosedHat,
     .band = {0, 1},
     .densityGate = kLaneGate,
     .expression = {.spread = 12}},
    {.lane = Lane::Perc1, .pool = kMallets, .band = {0, 1}, .densityGate = kLaneGate, .expression = {.spread = 10}},
    {.lane = Lane::Perc2, .pool = kWood, .band = {0, 1}, .densityGate = kLaneGate, .expression = {.spread = 10}},
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

/// The root alone, the octave below now and then as Complexity rises, never sliding; each note held two bars, to the
/// pattern's end, ending before the next pick may start (loopNotesValid()).
constexpr LoopSpec kSubLoop{
    .degrees = kRootOnly,
    .draws = {.leap = 0, .octaveUp = 0, .octaveDown = {0, 200}, .accent = 0},
    .slide = 0,
    .mutation = 60,
    .gateTicks = 2 * kTicksPerBar,
};

/// Chord positions: mostly the root, the 3rd and the 5th, then the Extensions (spec §4.5).
constexpr LineWeight kBellPositions[] = {{0, 4}, {1, 3}, {2, 3}, {3, 2}, {4, 1}};

/// Small leaps, an octave up now and then, each note sounding to the next onset or the bar line.
constexpr MotifSpec kBellsMotif{
    .positions = kBellPositions,
    .draws = {.leap = {2, 3}, .octaveUp = {0, 100}, .octaveDown = 0, .accent = 100},
    .reroll = 80,
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
        // The Bass Register's default: the root sounds between F#1 and F2, its octave jumps below.
        .loop = kSubLoop,
        .expression = {.spread = 0},
        .gestures = kNoGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "PAD",
        // The Pad Register's default, each chord with its Extensions, no doubled root, held with no retrigger.
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift, .sweepBars = 16},
        .volume = 100,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "DRONE",
        // An octave under the Pad Register's default (anchor C3), so the key's root and fifth sit under the Pad.
        .reg = {.anchor = 48, .spanLow = -12, .spanHigh = 12},
        .sustain = {.pedal = true},
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift},
        .volume = 100,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "BELLS",
        .pool = kBells,
        .band = {0, 3},
        // At the Break's Density at the default macros (25 − 15), so the Break keeps the bells, their anchored 1s; the
        // brief's "about 20" would close them there. They enter as the Intro's Density passes 10 (tuning).
        .densityGate = macroPoints(10),
        .motif = kBellsMotif,
        .expression = {.spread = 10, .spreadMode = SpreadMode::Fresh},
        .gestures = kNoGestures,
        .drift = {.weight = kSlowDrift},
        .volume = 100,
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kNoGestures},
};

constexpr int32_t kPadPart = 2;
constexpr int32_t kDronePart = 3;
constexpr int32_t kLeadPart = 4;

/// Home I, the brief's moves one way each; the home pull and the repeat bring the walk back (spec §4.5). Weights in
/// the colour moves' units (20). Lydian's and Dorian's colour moves come from the table (spec §7.4).
constexpr ChordMove kMoves[] = {
    {kI, kIV, 25},  // I–IV
    {kI, kVI, 25},  // I–vi
    {kIV, kI, 25},  // IV–I
    {kVI, kIV, 25}, // vi–IV
    {kI, kV, 10},   // I–V, rare
    {kII, kIV, 20}, // ii–IV
};

/// 8 bars a chord, one a phrase, and a heavy repeat, so a chord often holds 16 or 24 bars. No cadence target.
constexpr ChordGraph kGraph{
    .homePull = 10,
    .moves = kMoves,
    .repeatWeight = 40,
    .barsPerChord = 8,
};

constexpr Scale kScales[] = {Scale::Major, Scale::Lydian, Scale::Dorian};

constexpr WeightedBars kBuildBars[] = {{16, 1}};

constexpr Genre kAmbient{
    .name = "ambient",
    .oledName = "AMBIENT",
    .version = 1, // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 60, .tempoHighBpm = 90, .scales = kScales, .swing = 0},
    .phraseBars = 8,
    .parts = kParts,
    // No fills, so FILL does nothing; no drop-outs.
    .harmony = {.graph = &kGraph, .extensionFloor = 1, .extensionCeiling = 2},
    .palette = &ambientPalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 2,
            .groovePhrases = {6, 12},
            .breakPhrases = {1, 2},
            .buildBars = kBuildBars,
            .grooveToBuild = 300,
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
