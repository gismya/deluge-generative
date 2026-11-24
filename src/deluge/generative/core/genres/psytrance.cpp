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
// Psytrance (#127, spec §4.5): a parent genre of its own (#319), sharing no data with Techno, so a Techno retune never
// moves it. Its one subgenre, Classic, plays the parent's table as it is: a four-on-the-floor kick under a rolling 16th
// bass that Avoids it, a pedal Drone (an empty Harmony slot), a swept Lead that comes in on Drops and a 16th Arp, in
// 16-bar phrases with long Builds into hard Drops. Its fills are a Rest on the kick and bass and a Roll on the snare.
// Every value is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/capacity.h"
#include "generative/core/genre.h"

#include <initializer_list>
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

/// An open hat on the & now and then: every other bar, the hit in the second (#350), never on every &, where it would
/// replace every closed hat (spec §5.7).
constexpr Pattern kOpenHat[] = {
    {"..............................7.", "................................", 0, 2},
    {"......................6.......7.", "................................", 1},
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
/// since the kick plays every beat, so the bass rolls on the e, & and a with the kick alone on the beat. The e's sit
/// under the a's so Density thins the Intro stepwise: offbeats, then K.BB, then the full roll (round 2, S8). Even
/// levels, no ghosts: a psy bass rolls evenly (#353).
constexpr Pattern kRollingBass[] = {
    {"87X887X887X887X8", "................", 0, 2},
    {"86X886X886X886X8", "................", 0},
    {"87X987X987X987X9", "................", 1},
    {"86X986X986X986X9", "................", 1},
    {"97X997X997X997X9", "................", 2},
};

/// The Lead's riffs, each opening on an anchored 1: 8ths and dotted figures at tier 0, 16th runs as Complexity rises.
constexpr Pattern kLeadRiffs[] = {
    {"X..8..8...8..8..", "................", 0, 2},
    {"X.8.8.8.X.8.8.8.", "................", 0},
    {"X..8..8.X..8.88.X..8..8.X.8.8.8.", "................................", 0},
    {"X.88.88.X.88.8.8", "...g......g.....", 1},
    {"X..8.88..8.88.8.", "......g.....g...", 1},
    {"X..8..8.X..8.88.", "................", 1},
    {"X.8.8.88X.8.8.88", "................", 1},
    {"X..8.88..8.88.8.X.88.88.X..8.88.", "................................", 1},
    {"X888.888X888.8.8", ".g.g...g.g.g....", 2},
    {"X8.8X8.8X8.8X889X8.8X8.8X8.89898", ".g...g...g...g...g...g...g.....g", 2},
};

/// The Arp's 16ths in three tiers (#497), the 1 anchored so the Break's dip never thins it to nothing: the beats X or 9
/// (from Density about 6), the &s 7 (from about 17), the e's and a's 5 (from about 28). So the Intro's Density ramp
/// goes from the 1 to quarters to 8ths, a Groove at the default Density (25) plays 8ths, a Build's ramp fills in the
/// 16ths and a Drop's lift (33) plays them. The busier tiers put a 6 (from about 22) on some a's (tier 1: two or one of
/// four; tier 2: every a in one entry, two in the other), 16ths a Groove hears too: 8 to 10 notes a bar there. Every entry has an onset on every 16th: none skips the & (#354).
constexpr Pattern kArpLine[] = {
    {"X575957595759575", "a.......a.......", 0, 2}, // 8ths, filling in to 16ths
    {"X575X575X575X575", "a...a...a...a...", 0},
    {"X575957695759576", "a.......a.......", 1},
    {"X575X575X576X575", "a...a...a...a...", 1},
    {"X576957695769576", "a.......a.......", 2},
    {"X576X575X576X575", "a...a...a...a...", 2},
};
// clang-format on

/// The hats keep to the & steps between the kicks, less so as Complexity rises (spec §5.5).
constexpr InterplayRule kHatsOffKick[] = {
    {.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .offBeat = true, .strength = 90, .sensitivity = -40},
};

/// The bass never plays on, or sounds into, a kick step (spec §5.5): full strength at every Complexity.
constexpr InterplayRule kBassAvoidsKick[] = {
    {.kind = RuleKind::Avoid, .target = {0, Lane::Kick}, .strength = 100, .sensitivity = 0},
};

constexpr int16_t kRideShakerGate = macroPoints(35);
constexpr int16_t kPercGate = macroPoints(29);

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

/// The root, an octave up now and then as Complexity rises, never sliding, even and fixed per pick (round 2, S8), a
/// short gate so each 16th stands apart.
constexpr LoopSpec kBassLoop{
    .degrees = kRootOnly,
    .draws = {.leap = 0, .octaveUp = {60, 200}, .octaveDown = 0, .accent = 0},
    .slide = 0,
    .mutation = 0,
    .gateTicks = 20,
};

/// Scale steps from the root: mostly the root and the 5th, the 2nd (Phrygian's ♭2) and the 3rd, the 7th and 6th under
/// the root and the 6th and 7th over the 5th (round 2, S7).
constexpr LineWeight kLeadDegrees[] = {{0, 5}, {1, 2}, {2, 2}, {4, 3}, {-1, 2}, {-2, 1}, {5, 1}, {6, 1}};
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

/// The walks of the figure set's other variants (round 2, S1 A, C and E; #497), one per kArpLine entry: Up or
/// Up-down over one octave in the plain tier, up to two in the busier, or the same in random order.
constexpr ArpWalk kArpWalks[] = {
    {.order = ArpOrder::Up, .octaves = 1},     // tier 0
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 0
    {.order = ArpOrder::Up, .octaves = 2},     // tier 1
    {.order = ArpOrder::UpDown, .octaves = 1}, // tier 1
    {.order = ArpOrder::UpDown, .octaves = 2}, // tier 2
    {.order = ArpOrder::Up, .octaves = 2},     // tier 2
};
constexpr ArpWalk kShuffledWalks[] = {
    {.order = ArpOrder::Random, .octaves = 1}, // tier 0
    {.order = ArpOrder::Random, .octaves = 1}, // tier 0
    {.order = ArpOrder::Random, .octaves = 2}, // tier 1
    {.order = ArpOrder::Random, .octaves = 1}, // tier 1
    {.order = ArpOrder::Random, .octaves = 2}, // tier 2
    {.order = ArpOrder::Random, .octaves = 2}, // tier 2
};
static_assert(std::size(kArpWalks) == std::size(kArpLine) && std::size(kShuffledWalks) == std::size(kArpLine),
              "a walk for each Arp pool entry");

/// The sequence (round 2, S1 D): the Arp's four notes (root, 3rd, 5th, 7th), root-heavy, rolled per pick and held
/// until the next pick (mutation 0).
constexpr LineWeight kSeqDegrees[] = {{0, 5}, {2, 2}, {4, 3}, {6, 1}};
constexpr Answer kSeqAnswers[] = {{AnswerKind::Octave, 1}, {AnswerKind::Rotate, 2}};
constexpr LoopSpec kSeqLoop{
    .degrees = kSeqDegrees,
    .draws = {.leap = 6, .octaveUp = {150, 300}, .octaveDown = 0, .accent = 200},
    .slide = 0,
    .mutation = 0,
    .answers = kSeqAnswers,
    .answerBars = 8,
    .gateTicks = 18,
};

constexpr int32_t kBassPart = 1;
constexpr int32_t kDronePart = 2;
constexpr int32_t kLeadPart = 3;
constexpr int32_t kArpPart = 4;

/// Drums, Bass, Drone, Lead, Arp and FX (spec §4.5). The Lead and the Arp are both Lead role, sized within the largest
/// roster's capacities (rosterFits()).
constexpr PartSpec kParts[] = {
    // Each row at 85 % of its sound's volume (Classic sign-off 1, V2 C), under the Bass (tuning).
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kGestures, .volume = 85},
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
        // Lean 0 on every position, so "off every kick step" holds in ticks too (spec §5.5). A narrow spread, so the
        // roll's 16ths sound alike (#497, the psy idiom; tuning).
        .expression = {.spread = 2},
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
        .drift = {.sweepBars = 32},
        // 55 % of the preset's volume knob (round 2, H3, raised from round 1's 40 %): lifts 149 Cold 5th Pad from
        // about -27.1 to -22.3 dBFS without clipping the full mix (h3-level55-full: -14.5 dBFS RMS, -0.48 dBFS peak).
        .volume = 55,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Loop,
        .name = "LEAD",
        .pool = kLeadRiffs,
        .band = {0, 2},
        // Over the default Density (25), so it comes in on Drops, whose lift takes Density to 33 and decays over four
        // phrases: it plays a Drop's first two phrases and leaves on the phrase line (round 2, S2). Builds leave it out
        // (buildKeeps), though their Density ramp passes the gate.
        .densityGate = macroPoints(31),
        // The Arp's anchor plus two octaves (A5): its tonic two octaves over the Arp's; only notes under the Lead's
        // root reach the Arp's top. An octave over its tonic at most (#497): Song 3's A#6s, an octave and a fifth over
        // it, sounded shrill on 057's upper partials (tuning).
        .reg = {.anchor = 81, .spanLow = -5, .spanHigh = 12},
        .loop = kLeadLoop,
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 16},
        .volume = 100,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Loop,
        .name = "ARP",
        .pool = kArpLine,
        .band = {0, 2},
        // In from about the Intro's 16th bar (0:27 at 143 BPM, its Density ramping from 0 to 25), on quarters, then
        // 8ths: the Intro ramps up instead of opening on the Arp's anchor alone (#497, tuning).
        .densityGate = macroPoints(12),
        // 34 semitones (arpRange(2), for the figure set's walks): its tonic nearest A3, two octaves under the Lead's,
        // with room for the Loop's octave jumps.
        .reg = {.anchor = 57, .spanLow = -3, .spanHigh = 31},
        // Short notes, so each 16th speaks on its own (tuning). The walks are the figure set's (kFigureVariants).
        .loop = kSeqLoop,
        .arp = {.walks = kArpWalks, .gateTicks = 18},
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
        // 75 % of the preset's volume knob (Classic sign-off 1, V2 C): the Arp was the loudest part (tuning).
        .volume = 75,
        // Both play in a Drop; elsewhere the Arp keeps its 8ths under the Lead (the ARP+LEAD set's first variant,
        // Classic sign-off 1 V1: both is the most intense, rarer still outside Drops; tuning).
        .yield = {.part = kLeadPart, .inDrops = YieldMode::None, .outside = YieldMode::Eighths},
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};
// The part indices name the parts the arc's masks and the Build's cut mean.
static_assert(kParts[kBassPart].role == Role::Bass);
static_assert(kParts[kDronePart].role == Role::Pad);
static_assert(kParts[kLeadPart].role == Role::Lead && kParts[kLeadPart].flavour == Flavour::Loop);
static_assert(kParts[kArpPart].role == Role::Lead && kParts[kArpPart].flavour == Flavour::Loop);

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

/// Groove rests (spec §4.5, §8.8; round 2 S4, #457): the Arp rests for 8 or 16 bars, everything but Kick and Bass
/// rests for 8 bars, or the Lead for 8 or 16 (#497, so a Lead in the Grooves comes and goes), drawn on the bar before
/// a half-phrase line (dropOutGridBars = 8). No Throw.
constexpr PartsAndLanes kArpOnly{.parts = partBit(kArpPart)};
constexpr PartsAndLanes kLeadOnly{.parts = partBit(kLeadPart)};
constexpr PartsAndLanes kAllButKickAndBass{.parts = partBit(kArpPart) | partBit(kLeadPart) | partBit(kDronePart),
                                           .lanes = laneBit(Lane::Clap) | laneBit(Lane::ClosedHat)
                                                    | laneBit(Lane::OpenHat) | laneBit(Lane::Ride)
                                                    | laneBit(Lane::Perc1) | laneBit(Lane::Perc2)};
constexpr DropOutEntry kDropOuts[] = {
    {.mask = kArpOnly, .bars = 8, .weight = 3},   {.mask = kAllButKickAndBass, .bars = 8, .weight = 2},
    {.mask = kArpOnly, .bars = 16, .weight = 1},  {.mask = kLeadOnly, .bars = 8, .weight = 2},
    {.mask = kLeadOnly, .bars = 16, .weight = 1}, // the Lead's two: tuning
};

constexpr Scale kScales[] = {Scale::Minor, Scale::Phrygian, Scale::HarmonicMinor};

constexpr WeightedBars kBuildBars[] = {{8, 2}, {16, 2}, {32, 1}};

// Variant sets (#461, #497, ADR 0008): round 2's S1 and S3, and Classic sign-off 1's V1 (the Arp and the Lead
// together), the Lead's amount and the Arp's accents. The genre as written first in each; weights are tuning.

/// S1, the Arp's figure (round 2: "I like all, but D instead of B"): the seeded sequence (D, as written, heaviest),
/// the walks up or up and down (A), the same notes in random order (C), or the walks on the triad (E: no 7th). Every
/// variant plays kArpLine, so the figure keeps its Density tiers. Live: a part's flavour and its line data are read
/// step by step, and the pool stays the same.
constexpr Variant kFigureVariants[] = {
    {.name = "sequence", .oledName = "SEQUENCE", .weight = 3},
    {.name = "walk",
     .oledName = "WALK",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kArpPart].flavour = Flavour::Arp; }},
    {.name = "shuffle",
     .oledName = "SHUFFLE",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         PartSpec& arp = resolved.parts[kArpPart];
	         arp.flavour = Flavour::Arp;
	         arp.arp.walks = kShuffledWalks;
         }},
    {.name = "triad",
     .oledName = "TRIAD",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.parts[kArpPart].flavour = Flavour::Arp;
	         // The home chord's only other reader is the Arp: the Drone is a pedal, the Lead and Bass Loops.
	         resolved.genre.harmony.extensionCeiling = 0;
         }},
};

/// The Break with the Lead in and the Arp out (S3 B), its Density at the Groove's plus 6 so the Lead's gate (31) opens
/// (round 2's diff; tuning).
constexpr PartsAndLanes kLeadBreakKeeps{.parts = partBit(kDronePart) | partBit(kLeadPart),
                                        .lanes = laneBit(Lane::ClosedHat)};
void leadBreak(ResolvedGenre& resolved) {
	resolved.genre.arc.offsets[static_cast<int32_t>(SectionKind::Break)] = {macroPoints(-10), macroPoints(-10),
	                                                                        macroPoints(6), macroPoints(6)};
	resolved.genre.arc.breakKeeps = kLeadBreakKeeps;
}

/// S3, the Break (round 2: today, "but weighing today"): the Drone, the Arp and the closed hat (as written), the Lead
/// in and the Arp out (B), or B without the hat (C). Not live: the Arc reads the Break ahead.
constexpr Variant kBreakVariants[] = {
    {.name = "arp", .oledName = "ARP", .weight = 2},
    {.name = "lead", .oledName = "LEAD", .weight = 1, .apply = leadBreak},
    {.name = "leadnohat",
     .oledName = "NO HAT",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         leadBreak(resolved);
	         resolved.genre.arc.breakKeeps = {.parts = partBit(kDronePart) | partBit(kLeadPart)};
         }},
};

/// V1, the Arp while the Lead plays (Classic sign-off 1: "all three randomized, lower chance for both, especially
/// outside the Drop"): dropboth (as written: both in a Drop; elsewhere the Arp keeps its 8ths), turns (the Arp rests),
/// or the Arp's 8ths everywhere. Never in a Build (yieldOn()). Live: the yield is read step by step.
constexpr Variant kArpLeadVariants[] = {
    {.name = "dropboth", .oledName = "DROP BOTH", .weight = 1},
    {.name = "turns",
     .oledName = "TURNS",
     .weight = 2,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.parts[kArpPart].yield = {
	             .part = kLeadPart, .inDrops = YieldMode::Rest, .outside = YieldMode::Rest};
         }},
    {.name = "eighths",
     .oledName = "8THS",
     .weight = 2,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.parts[kArpPart].yield = {
	             .part = kLeadPart, .inDrops = YieldMode::Eighths, .outside = YieldMode::Eighths};
         }},
};

/// How much Lead (Classic sign-off 1: "the Lead should come in earlier and more often"): a Drop's first two phrases
/// (as written, gate 31), every Groove too (gate 24, the default Density's 25 over it, the Break's 20 under it), or
/// from the Intro's last third (gate 20, the Break too). Not live: tuning kept it per piece (variant-ideas, LEAD).
constexpr Variant kLeadVariants[] = {
    {.name = "drops", .oledName = "DROPS", .weight = 1},
    {.name = "grooves",
     .oledName = "GROOVES",
     .weight = 3,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kLeadPart].densityGate = macroPoints(24); }}, // (tuning)
    {.name = "intro",
     .oledName = "INTRO",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kLeadPart].densityGate = macroPoints(20); }}, // (tuning)
};

/// Where the sequence's octave jumps land (Classic sign-off 1, F3: "the echo on the Arp's higher notes should be more
/// on the beat"): any step (as written), the 3-3-2 (steps 0, 3, 6, 8, 11 and 14, the Goa gallop's accents) or the
/// offbeat 8ths (2, 6, 10 and 14). The sequence's (Loop) only: a walk has no jumps. Live: the mask is read where the
/// line rolls, at a pick.
constexpr uint16_t steps(std::initializer_list<int32_t> list) {
	uint16_t mask = 0;
	for (int32_t step : list) {
		mask = static_cast<uint16_t>(mask | (1U << step));
	}
	return mask;
}
constexpr Variant kAccentVariants[] = {
    {.name = "anywhere", .oledName = "ANYWHERE", .weight = 1},
    {.name = "332",
     .oledName = "3-3-2",
     .weight = 3,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kArpPart].loop.jumpSteps = steps({0, 3, 6, 8, 11, 14}); }},
    {.name = "offbeat",
     .oledName = "OFFBEAT",
     .weight = 2,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kArpPart].loop.jumpSteps = steps({2, 6, 10, 14}); }},
};

constexpr VariantSet kVariantSets[] = {
    {.name = "figure", .oledName = "FIGURE", .variants = kFigureVariants, .live = true},
    {.name = "break", .oledName = "BREAK", .variants = kBreakVariants},
    {.name = "arplead", .oledName = "ARP+LEAD", .variants = kArpLeadVariants, .live = true},
    {.name = "lead", .oledName = "LEAD", .variants = kLeadVariants},
    {.name = "accents", .oledName = "ACCENTS", .variants = kAccentVariants, .live = true},
};

/// The parent: never played, so it carries no version (spec §4.4). Its name is the group's in the GENRE menu and the
/// stem of its subgenres' names; no Recipe names it (findGenre() lists only playable genres). It holds the whole of
/// Psytrance; a subgenre overrides what it plays differently (#410 adds more).
constexpr Genre kPsytranceParent{
    .name = "psytrance",
    .oledName = "PSYTRANCE",
    .version = 0,
    .facts = {.tempoLowBpm = 138, .tempoHighBpm = 146, .scales = kScales},
    .phraseBars = 16,
    .parts = kParts,
    .densityJitter = 15,
    .repickChance = 300,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 600,
    .sectionEndChance = 1000,
    .dropOuts = kDropOuts,
    .dropOutChance = 300,
    .dropOutGapBars = 8,
    .dropOutGridBars = 8,
    // An empty slot: the home chord throughout, under the Drone's pedal (spec §7.4).
    .harmony = {.extensionFloor = 0, .extensionCeiling = 1},
    .palette = &psytrancePalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 1,
            .groovePhrases = {2, 4},
            .breakPhrases = {1, 1},
            .buildBars = kBuildBars,
            .grooveToBuild = 500,
            // The Drop's lift decays over four phrases, so the Lead (gate 31) plays a Drop's first two phrases and
            // leaves on the phrase line (round 2, S2; spec §9.3's named exception, #349).
            .dropDecayPhrases = 4,
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
            .buildCutBars = 4,
            .gap = GapLength::Beat,
            .roll = {.lane = Lane::Snare, .top = RollStage::ThirtySecond},
            .accentLane = Lane::Crash,
            .accentChance = 0, // a Crash on Drops and after a fill that ends a phrase
        },
    .variantSets = kVariantSets,
};

/// Classic psytrance (#319): Psytrance as #127 built it, with no overrides of its parent's table.
constexpr Genre kPsytranceClassic = [] {
	Genre genre = subgenreOf(kPsytranceParent);
	genre.name = "psytrance-classic";
	genre.oledName = "CLASSIC PSY";
	genre.shortName = "CLASSIC";
	genre.version = 7; // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
	return genre;
}();
static_assert(genreValid(kPsytranceClassic),
              "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre psytranceClassic = kPsytranceClassic;

} // namespace deluge::generative::genres
