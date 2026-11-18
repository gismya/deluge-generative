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
// Psytrance (#127, spec §4.5): a genre of its own, with no parent, so a Techno retune never moves it. A four-on-the-
// floor kick under a rolling 16th bass that Avoids it, a pedal Drone (an empty Harmony slot), a swept Lead that comes
// in on Drops and a 16th Arp, in 16-bar phrases with long Builds into hard Drops. Its fills are a Rest on the kick and
// bass and a Roll on the snare. Every value is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/capacity.h"
#include "generative/core/genre.h"

#include <iterator>

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
/// Four on the floor at every Complexity, with no pickups off the beat: no Bass anchor can meet a kick step
/// (avoidRulesValid()), and the Bass's beat steps are the kick's alone.
constexpr Pattern kKick[] = {
    {"X...X...X...X...", "................", 0},
};

/// The clap on 2 and 4 at every Complexity.
constexpr Pattern kClap[] = {
    {"....8.......8...", "................", 0},
};

/// 16ths around the & (the hats' Lock keeps them mostly there), busier with Complexity.
constexpr Pattern kClosedHat[] = {
    {"..8...8...8...8.", "................", 0, 2},
    {"5.8.5.8.5.8.5.8.", "g...g...g...g...", 0},
    {"3795379537953795", "gg.ggg.ggg.ggg.g", 1},
    {"..8.4.8.5.8.4.8.", "....g...g...g...", 1},
    {"4796479647964797", "gg.ggg.ggg.ggg.g", 2},
};

/// An open hat on the & now and then: not on every &, where it would replace every closed hat (spec §5.7).
constexpr Pattern kOpenHat[] = {
    {"..............7.", "................", 0, 2},
    {"......6.......7.", "................", 1},
};

/// The ride on the &s, behind its gate: a raised Density brings it in.
constexpr Pattern kRide[] = {
    {"..7...7...7...7.", "................", 0},
    {"..7.4.7...7.4.7.", "....g.......g...", 1},
};

/// Percussion off the beat, behind its gate.
constexpr Pattern kPerc[] = {
    {"...6.......5....", "................", 0, 2},
    {"...6..4....5..7.", "......g.........", 1},
};

/// The shaker on the e and the a, behind its gate.
constexpr Pattern kShaker[] = {
    {".5.7.5.7.5.7.5.7", ".g...g...g...g..", 0, 2},
    {"3575357535753575", "gg.ggg.ggg.ggg.g", 1},
};

/// The rolling bass: an onset on every 16th, the & anchored. The beat steps carry onsets too: Avoid Kick drops them,
/// since the kick plays every beat, so the bass rolls on the e, & and a with the kick alone on the beat. Density thins
/// the e and the a as it falls.
constexpr Pattern kRollingBass[] = {
    {"88X888X888X888X8", "................", 0, 2},
    {"78X878X878X878X8", "................", 0},
    {"89X889X889X889X9", "................", 1},
    {"79X979X979X979X9", ".g...g...g...g..", 1},
    {"99X999X999X999X9", "................", 2},
};

/// The Lead's riffs, each opening on an anchored 1: 8ths and dotted figures at tier 0, 16th runs as Complexity rises.
constexpr Pattern kLeadRiffs[] = {
    {"X..8..8...8..8..", "................", 0, 2},
    {"X.8.8.8.X.8.8.8.", "................", 0},
    {"X..8..8.X..8.88.X..8..8.X.8.8.8.", "................................", 0},
    {"X.88.88.X.88.8.8", "...g............", 1},
    {"X..8.88..8.88.8.", "......g.....g...", 1},
    {"X888.888X888.8.8", ".g.g...g.g.g....", 2},
    {"X8.8X8.8X8.8X889X8.8X8.8X8.89898", ".g...g...g...g...g...g...g.....g", 2},
};

/// The Arp's 16ths, the 1 anchored so the Break's dip never thins it to nothing; each entry with its own walk
/// (kArpWalks). At the default Density the 8s and 9s play; more Density fills in the rest.
constexpr Pattern kArpLine[] = {
    {"X787978797879787", "a.......a.......", 0, 2}, // 8ths, filling in to 16ths
    {"X878X878X878X878", "a...a...a...a...", 0},
    {"X898789878987898", "a.......a.......", 1},
    {"X8.8X8.8X8.8X8.8", "a...a...a...a...", 1}, // gallops
    {"X989898989898989", "a.......a.......", 2},
    {"X998999899989998", "a...a...a...a...", 2},
};
// clang-format on

/// Each kArpLine entry's walk (spec §4.5): Up or Up-down over one octave in the plain tier, up to two in the busier.
constexpr ArpWalk kArpWalks[] = {
    {.order = ArpOrder::Up, .octaves = 1},     // tier 0
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 0
    {.order = ArpOrder::Up, .octaves = 2},     // tier 1
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 1
    {.order = ArpOrder::UpDown, .octaves = 2}, // tier 2
    {.order = ArpOrder::Up, .octaves = 2},     // tier 2
};
static_assert(std::size(kArpWalks) == std::size(kArpLine), "a walk for each Arp pool entry");

/// The hats keep to the & steps between the kicks, less so as Complexity rises (spec §5.5).
constexpr InterplayRule kHatsOffKick[] = {
    {.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .offBeat = true, .strength = 90, .sensitivity = -40},
};

/// The bass never plays on, or sounds into, a kick step (spec §5.5): full strength at every Complexity.
constexpr InterplayRule kBassAvoidsKick[] = {
    {.kind = RuleKind::Avoid, .target = {0, Lane::Kick}, .strength = 100, .sensitivity = 0},
};

constexpr int16_t kRideShakerGate = macroPoints(35);
constexpr int16_t kPercGate = macroPoints(30);

/// In the fixed lane order (spec §5.4). Straight time and no Lean: psytrance sits on the grid. The snare plays only
/// the Build roll and the Roll fill.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 0}, .expression = {.spread = 4}},
    {.lane = Lane::Snare}, // the Build roll's lane, and the Roll fill's
    {.lane = Lane::Clap, .pool = kClap, .band = {0, 0}, .expression = {.spread = 8}},
    {
        .lane = Lane::ClosedHat,
        .pool = kClosedHat,
        .band = {0, 2},
        .rules = kHatsOffKick,
        .expression = {.spread = 12, .ornaments = {.rateFloor = 0, .rateCeiling = 120, .ratchet = {3, 1, 0}}},
    },
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .rules = kHatsOffKick, .expression = {.spread = 6}},
    {.lane = Lane::Ride, .pool = kRide, .band = {0, 1}, .densityGate = kRideShakerGate, .expression = {.spread = 8}},
    {.lane = Lane::Crash}, // the Phrase accent's lane
    {.lane = Lane::Perc1, .pool = kPerc, .band = {0, 1}, .densityGate = kPercGate, .expression = {.spread = 10}},
    {.lane = Lane::Perc2,
     .pool = kShaker,
     .band = {0, 1},
     .densityGate = kRideShakerGate,
     .expression = {.spread = 12}},
};

/// FX lanes have no pools: only section rules play them (spec §5.4).
constexpr LaneSpec kFxLanes[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};

/// The Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};

constexpr LineWeight kRootOnly[] = {{0, 1}};

/// The root, an octave up now and then as Complexity rises, never sliding, a short gate so each 16th stands apart.
constexpr LoopSpec kBassLoop{
    .degrees = kRootOnly,
    .draws = {.leap = 0, .octaveUp = {60, 200}, .octaveDown = 0, .accent = 120},
    .slide = 0,
    .mutation = 60,
    .gateTicks = 16,
};

/// Scale steps from the root: mostly the root and the 5th, the 2nd (Phrygian's ♭2) and the 3rd, the 7th now and then.
constexpr LineWeight kLeadDegrees[] = {{0, 5}, {1, 2}, {2, 2}, {4, 3}, {6, 1}};
constexpr Answer kLeadAnswers[] = {{AnswerKind::Octave, 2}, {AnswerKind::Reverse}, {AnswerKind::Rotate}};

/// The Lead's line: short notes with a few slides, leaping wider and jumping octaves as Complexity rises.
constexpr LoopSpec kLeadLoop{
    .degrees = kLeadDegrees,
    .draws = {.leap = {2, 6}, .octaveUp = {60, 180}, .octaveDown = 0, .accent = 150},
    .slide = {40, 120},
    .mutation = 100,
    .answers = kLeadAnswers,
    .answerBars = 4,
    .gateTicks = 18,
};

constexpr int32_t kBassPart = 1;
constexpr int32_t kDronePart = 2;
constexpr int32_t kLeadPart = 3;
constexpr int32_t kArpPart = 4;

/// Drums, Bass, Drone, Lead, Arp and FX (spec §4.5). The Lead and the Arp are both Lead role, sized within the largest
/// roster's capacities (rosterFits()).
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kRollingBass,
        .band = {0, 2},
        .rules = kBassAvoidsKick,
        // The key's root between D#1 and D2 (39–73 Hz): low, with room for the octave over it.
        .reg = {.anchor = 33, .spanLow = -5, .spanHigh = 12},
        .loop = kBassLoop,
        // Lean 0 on every position, so "off every kick step" holds in ticks too (spec §5.5).
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "DRONE",
        // The Arp's anchor less an octave (A2), so the Drone's tonic is always exactly an octave under the Arp's and
        // the root and fifth sit under the Arp and the Lead.
        .reg = {.anchor = 45, .spanLow = -12, .spanHigh = 12},
        .sustain = {.pedal = true},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
        // 40 % of the preset's volume knob (tuning): at the preset's level the held root and fifth sat about 10 dB over
        // the rest of the mix and clipped it (delugemu capture, seed 1, 149 Cold 5th Pad).
        .volume = 40,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Loop,
        .name = "LEAD",
        .pool = kLeadRiffs,
        .band = {0, 2},
        // Over the default Density (25), so it comes in on Drops, whose lift takes Density to 33. Builds leave it out
        // (buildKeeps), though their Density ramp passes the gate.
        .densityGate = macroPoints(30),
        // The Arp's anchor plus two octaves (A5): its tonic two octaves over the Arp's; only notes under the Lead's
        // root reach the Arp's top.
        .reg = {.anchor = 81, .spanLow = -5, .spanHigh = 19},
        .loop = kLeadLoop,
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
        .volume = 100,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Arp,
        .name = "ARP",
        .pool = kArpLine,
        .band = {0, 2},
        // 34 semitones (arpRange(2)): a two-octave walk from any chord root, starting between 3 below the tonic and 31
        // over it; the tonic nearest A3, two octaves under the Lead's.
        .reg = {.anchor = 57, .spanLow = -3, .spanHigh = 31},
        // Short notes, so each 16th speaks on its own (tuning).
        .arp = {.walks = kArpWalks, .gateTicks = 18},
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
        .volume = 100,
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};
// The part indices name the parts the arc's masks and the Build's cut mean.
static_assert(kParts[kBassPart].role == Role::Bass);
static_assert(kParts[kDronePart].role == Role::Pad);
static_assert(kParts[kLeadPart].role == Role::Lead && kParts[kLeadPart].flavour == Flavour::Loop);
static_assert(kParts[kArpPart].role == Role::Lead && kParts[kArpPart].flavour == Flavour::Arp);

/// The kick drop with the bass, the held-fill entry, 1, 2 or 4 beats (weight 3 in all), and a snare Roll of 1 or 2
/// beats (weight 2 in all) (spec §8.7).
constexpr PartsAndLanes kKickAndBass{.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)};
constexpr PartsAndLanes kSnare{.lanes = laneBit(Lane::Snare)};
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Rest, .mask = kKickAndBass, .beats = 2, .weight = 1},
    {.kind = FillKind::Rest, .mask = kKickAndBass, .beats = 1, .weight = 1},
    {.kind = FillKind::Rest, .mask = kKickAndBass, .beats = 4, .weight = 1},
    {.kind = FillKind::Roll, .mask = kSnare, .beats = 1, .weight = 1},
    {.kind = FillKind::Roll, .mask = kSnare, .beats = 2, .weight = 1},
};

constexpr Scale kScales[] = {Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor};

constexpr WeightedBars kBuildBars[] = {{16, 2}, {32, 1}};

constexpr Genre kPsytrance{
    .name = "psytrance",
    .oledName = "PSYTRANCE",
    .version = 1, // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 138, .tempoHighBpm = 146, .scales = kScales, .swing = 0},
    .phraseBars = 16,
    .parts = kParts,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 400,
    // An empty slot: the home chord throughout, under the Drone's pedal (spec §7.4). The Arp walks its triad, a 7th
    // joining as Complexity rises.
    .harmony = {.extensionFloor = 0, .extensionCeiling = 1},
    .palette = &psytrancePalette,
    .arc =
        {
            .introPhrases = 1,
            .outroPhrases = 1,
            .groovePhrases = {3, 6},
            .breakPhrases = {1, 1},
            .buildBars = kBuildBars,
            .grooveToBuild = 0,
            .dropDecayPhrases = 1,
            .dropLiftEnergy = macroPoints(8),
            .offsets =
                {
                    {macroPoints(-15), 0},                                                  // Intro
                    {},                                                                     // Groove
                    {macroPoints(-10), macroPoints(-10), macroPoints(-5), macroPoints(-5)}, // Break: a soft dip
                    {0, macroPoints(12), 0, macroPoints(10)},                               // Build
                    {0, macroPoints(-15)},                                                  // Outro
                },
            // The breakdown is the Drone, the Arp and the Lead over the closed hat. Its Density dips by 5, not 20:
            // the closed hat has no anchors and the parent-less table has no softer default. The Lead stays behind its
            // gate there at the default Density.
            .breakKeeps = {.parts = partBit(kDronePart) | partBit(kArpPart) | partBit(kLeadPart),
                           .lanes = laneBit(Lane::ClosedHat)},
            // Builds play everything but the Lead, so the kick and bass roll on until the cut and the Lead comes in
            // on the Drop.
            .buildKeeps = kEverything.without({.parts = partBit(kLeadPart)}),
            .buildCut = kKickAndBass,
            .buildCutBars = 2,
            .gap = GapLength::Beat,
            .roll = {.lane = Lane::Snare, .top = RollStage::ThirtySecond},
            .accentLane = Lane::Crash,
            .accentChance = 0, // a Crash on Drops and after a fill that ends a phrase
        },
};
static_assert(genreValid(kPsytrance), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre psytrance = kPsytrance;

} // namespace deluge::generative::genres
