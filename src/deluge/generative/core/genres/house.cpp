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
// House (#132, spec §4.5): a genre of its own with no parent. Four on the floor with claps on 2 and 4, an offbeat bass
// on the &s between the kicks, chord stabs that always carry a 7th, Strings under them, swung hats leaning late on the
// e and the a, and a Lead that Responds to the bass, answering in each bar's second half, over a Chord graph that
// walks. Every value is starting data, tuned by ear in #133 (spec §21.5).

#include "generative/core/capacity.h"
#include "generative/core/chord_walk.h"
#include "generative/core/genre.h"
#include "generative/core/variants.h"

namespace deluge::generative::genres {

namespace {

// Patterns: one character per 16th. Importance '.' never, '1'-'9' as Density allows, 'X' anchor.
// Levels 'g' ghost, 'a' accent, else normal. Then the Complexity tier and, if not 1, the pick weight.

// clang-format off
/// Four on the floor at every Complexity: the bass owns the steps between the beats.
constexpr Pattern kKick[] = {
    {"X...X...X...X...", "................", 0},
};

/// Claps on 2 and 4, anchored. One tier: a tier 1+ pattern with only on-beat hits breaks the off-beat authoring rule.
constexpr Pattern kClap[] = {
    {"....X.......X...", "................", 0},
};

/// Ghost rims on the a's (the & of 4 is the open hat's), as Complexity rises on every a, the last at full
/// level from tier 1.
constexpr Pattern kRim[] = {
    {".......5.......6", ".......g.......g", 0},
    {"...5...6...5...7", "...g...g...g....", 1},
};

/// The & on every beat, the a ghosted behind it; from tier 1 an a after every & (#373), or the e and the a around it.
/// The e and the a lean late (kHatLean).
constexpr Pattern kClosedHat[] = {
    {"..85..85..85..85", "...g...g...g...g", 0, 2},
    {"..8...8...8...86", "...............g", 0},
    {"..87..87..87..87", "...g...g...g...g", 1, 2},
    {".687.687.687.687", ".g.g.g.g.g.g.g.g", 1},
    {"4596459645964597", "gg.ggg.ggg.ggg.g", 2},
};

/// An open hat on the & of 4, or the & of 2 and 4, then on all four &s from tier 1: each replaces the closed hat there
/// (spec §5.7).
constexpr Pattern kOpenHat[] = {
    {"..............7.", "................", 0},
    {"......7.......7.", "................", 0},
    {"..7...7...7...7.", "................", 1, 3},
    {"......7.......7.", "................", 1},
};

/// The ride on the &s, behind its gate.
constexpr Pattern kRide[] = {
    {"..8...8...8...8.", "................", 0},
    {"..8.5.8...8.5.8.", "....g.......g...", 1},
};

/// Shaker and tambourine on the e, & and a, behind their gate.
constexpr Pattern kShaker[] = {
    {"..6...6...6...6.", "................", 0},
    {".56.5.6..56.5.67", ".g..g....g..g...", 1},
};

/// Congas and cowbell off the beat, behind their gate.
constexpr Pattern kConga[] = {
    {"...6......6..5..", ".............g..", 0},
    {"...6..7...6..5.7", ".............g..", 1},
};

// Fill patterns: only the Swap plays them, over a phrase's last beats.
constexpr Pattern kSnareFill[] = {
    {"..........8.X8XX", ".............g..", 0},
    {"........7.8.X8XX", "........g....g..", 1},
};
constexpr Pattern kClapFill[] = {
    {"....X.......X.8X", "..............g.", 0},
    {"....X...7.8.X8XX", "........g....g..", 1},
};

/// The offbeat bass: the & of every beat, anchored, never a beat (the kick's). 16th pickups as Complexity rises, each
/// line's in one half: the first, so the Lead's Respond rule leaves the second free for answers, except one tier-1
/// line's in the second, which moves the answers to the first half (#368).
constexpr Pattern kOffbeatBass[] = {
    {"..X...X...X...X.", "................", 0, 3},
    {"..X..7X...X...X.", ".....g..........", 0},
    {"..X..7X7..X...X.", ".....g.g........", 1},
    {"..X7..X...X...X.", "...g............", 1},
    {"..X...X..7X7..X.", ".........g.g....", 1},
    {".7X7.7X...X...X.", ".g.g.g..........", 2},
};

/// S6's pickups (round 2's B, #369): the offbeat bass with five more rhythms, two of them 2 bars long. Its first five
/// rows and its last repeat kOffbeatBass's six, in order: change them together.
constexpr Pattern kPickupBass[] = {
    {"..X...X...X...X.", "................", 0, 3},
    {"..X..7X...X...X.", ".....g..........", 0},
    {"..X..7X7..X...X.", ".....g.g........", 1},
    {"..X7..X...X...X.", "...g............", 1},
    {"..X...X..7X7..X.", ".........g.g....", 1},
    {"..X...X7..X...X.", ".......g........", 1},
    {".7X...X...X...X.", ".g..............", 1},
    {"..X7..X7..X...X.", "...g...g........", 1},
    {"..X...X...X...X." "..X..7X7..X...X.", ".....................g.g........", 1, 2},
    {"..X7..X...X...X." ".7X...X7..X...X.", "...g.............g.....g........", 1},
    {".7X7.7X...X...X.", ".g.g.g..........", 2},
};

/// The stab: anchored on the a of 1, then &s and a's, more of them as Complexity rises, two 2-bar lines whose second bar
/// answers the first. Never on a beat (the kick's and the clap's) or an e.
constexpr Pattern kStab[] = {
    {"...X......8.....", "................", 0, 2},
    {"...X..8........8", "................", 0},
    {"...X..8...8....7", "...............g", 1, 2},
    {"...X...7..8....8", ".......g........", 1},
    {"...X..8...8...7.", "..............g.", 1},
    {"..7X..8.......8.", "..g.............", 1},
    {"...X..8...8....7" "...X......8...7.", "...............g..............g.", 1, 2},
    {"...X...7..8....." "...X..8........8", ".......g........................", 1},
    {"...X..87..87...8", ".......g...g....", 2},
};

/// The Lead's answers: one- and two-bar phrases anchored only in a bar's second half (beat 3 or its &), so the Respond
/// rule's against half, the first on an offbeat bass's tie, holds nothing the Lead must play. Each line has a note or
/// two in the first half too, a lead-in, which Respond thins (spec §5.5); from tier 1 two or three a bar, each on a step
/// of its own: a lead-in's draw holds while the pick does, so with one a bar a piece kept all or none.
constexpr Pattern kAnswers[] = {
    {"..6...7...X...8.", "..g.............", 0, 2},
    {"..6.7...X..8..7." "..6...6...X...8.", "..g.............................", 0},
    {"..6.5.7.X.8..7.8", ".............g..", 1},
    {"..7...6...X.8.7." "....6.5.X..7.8..", "..g...........g............g....", 1},
    {"..6...6.X.8.78.6", "..g...g......g.g", 2},
};
// clang-format on

/// The e and the a lean 3 ticks late, over the song swing (spec §4.5, §8.5).
constexpr int8_t kLateLean = 3;
constexpr Expression kHatExpression{.lean = {0, kLateLean, 0, kLateLean}, .spread = 12};
/// The closed hat's: the same feel, with ratchets.
constexpr Expression kClosedHatExpression = [] {
	Expression expression = kHatExpression;
	expression.ornaments = {.rateFloor = 0, .rateCeiling = 100, .ratchet = {3, 1, 0}};
	return expression;
}();

constexpr int16_t kPercGate = macroPoints(35);

/// In the fixed lane order (spec §5.4). The Snare plays only the Swap, the Roll fill and the Build roll; the Crash
/// only the Phrase accent.
constexpr LaneSpec kDrumLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {0, 0}, .expression = {.spread = 4}},
    {.lane = Lane::Snare, .fillPool = kSnareFill, .band = {0, 1}}, // the Swap's, the Roll fill's and the Build roll's
    {.lane = Lane::Clap, .pool = kClap, .fillPool = kClapFill, .band = {0, 1}, .expression = {.spread = 8}},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}, .expression = {.spread = 10}},
    {.lane = Lane::ClosedHat, .pool = kClosedHat, .band = {0, 2}, .expression = kClosedHatExpression},
    {.lane = Lane::OpenHat, .pool = kOpenHat, .band = {0, 1}, .expression = {.spread = 6}},
    {.lane = Lane::Ride, .pool = kRide, .band = {0, 1}, .densityGate = kPercGate, .expression = kHatExpression},
    {.lane = Lane::Crash}, // the Phrase accent's lane
    {.lane = Lane::Perc1, .pool = kShaker, .band = {0, 1}, .densityGate = kPercGate, .expression = kHatExpression},
    {.lane = Lane::Perc2, .pool = kConga, .band = {0, 1}, .densityGate = kPercGate, .expression = {.spread = 10}},
};

/// FX lanes have no pools: only section rules play them (spec §5.4).
constexpr LaneSpec kFxLanes[] = {{.lane = Lane::Riser}, {.lane = Lane::Fall}, {.lane = Lane::Impact}};

/// The Intro stays unfiltered: no part Rises across it, while every Build still does (spec §11.4).
constexpr GestureSpec kGestures{.off = momentBit(GestureMoment::RiseIntro)};

/// The chord root, its 5th as Complexity rises. The Register's span (−5 to +7) holds the line under the Strings, so
/// no octave jumps.
constexpr LineWeight kBassDegrees[] = {{0, 8}, {4, 2}};
/// S6's pickups: the 5th most, the root and the 7th below about as often; the anchors stay on the root (tuning).
constexpr LineWeight kPickupDegrees[] = {{0, 3}, {4, 4}, {6, 3}};

/// Short offbeat notes, rare slides, a line that changes slowly.
constexpr LoopSpec kBassLoop{
    .degrees = kBassDegrees,
    .draws = {.leap = {3, 6}, .octaveUp = 0, .octaveDown = 0, .accent = 120},
    .slide = {0, 30},
    .mutation = 60,
    .gateTicks = 20,
};

/// Chord positions: mostly the root, the 3rd and the 5th, the 7th now and then, the note under the root rarely.
constexpr LineWeight kAnswerPositions[] = {{-1, 1}, {0, 4}, {1, 3}, {2, 3}, {3, 2}};

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kStabPart = 2;
constexpr int32_t kStringsPart = 3;
constexpr int32_t kLeadPart = 4;

/// The Lead answers the bass: the half of each bar where the Bass plans more onsets (the first on a tie) is against,
/// and the Lead's non-anchor onsets there thin, more so as Complexity rises (spec §5.5).
constexpr InterplayRule kLeadRespondsBass[] = {
    {.kind = RuleKind::Respond, .target = {kBassPart}, .strength = 60, .sensitivity = 40},
};

// Registers, stacked so no two pitched parts meet in any key: each anchor is an octave over the one below, so the
// tonics are octaves apart in every key, and the spans don't overlap (tonic T, the Bass's): Bass T−5..T+7, Strings
// T+8..T+22, Stab T+23..T+36, Lead T+37..T+53. Anchored on D♯, so the Bass's tonic runs A1 to G♯2: F♯, G and G♯ keys
// sit an octave over where a C anchor put them (#370), and C to F and A to B where they were.
constexpr Register kBassRegister{39, -5, 7};
constexpr Register kStringsRegister{51, -4, 10};
constexpr Register kStabRegister{63, -1, 12};
constexpr Register kLeadRegister{75, 1, 17};

/// Drums, an offbeat bass, the Stab, Strings, the Lead and FX (spec §4.5): Detroit's shape.
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes, .gestures = kGestures},
    {
        .role = Role::Bass,
        .flavour = Flavour::Loop,
        .name = "BASS",
        .pool = kOffbeatBass,
        .band = {0, 2},
        .densityGate = macroPoints(9),
        .reg = kBassRegister,
        .loop = kBassLoop,
        .expression = {.spread = 6},
        .gestures = kGestures,
    },
    {
        .role = Role::Chords,
        .flavour = Flavour::Stab,
        .name = "STAB",
        .pool = kStab,
        .band = {0, 2},
        .densityGate = macroPoints(12),
        .reg = kStabRegister,
        // Short, each stab on its own (tuning).
        .stab = {.gateTicks = 30},
        .expression = {.spread = 8},
        .gestures = kGestures,
        .drift = {.sweepBars = 32},
        // 80 % of the preset's volume knob (tuning): about 6.5 dB under the rest of the mix (delugemu captures of each
        // part alone, seed 1, a card with the pack).
        .volume = 80,
    },
    {
        .role = Role::Pad,
        .flavour = Flavour::Sustain,
        .name = "STRINGS",
        .densityGate = macroPoints(6),
        .reg = kStringsRegister,
        // Every chord with its Extension: the 7th, or the 9th, plays with the root at any Density.
        .sustain = {.anchorExtension = true},
        .gestures = kGestures,
        .drift = {.sweepBars = 8},
        // 40 % of the preset's volume knob (tuning): at the preset's level the Strings sat 4 dB over the rest of the
        // mix together and the mix peaked at full scale; at 40 about 10.5 dB under (delugemu captures of each part
        // alone, seed 1, a card with the pack).
        .volume = 40,
    },
    {
        .role = Role::Lead,
        .flavour = Flavour::Motif,
        .name = "LEAD",
        .pool = kAnswers,
        .band = {0, 2},
        .rules = kLeadRespondsBass,
        .densityGate = macroPoints(30),
        .reg = kLeadRegister,
        .motif = {.positions = kAnswerPositions,
                  .draws = {.leap = {2, 3}, .octaveUp = 0, .accent = 150},
                  .reroll = 20,
                  .gateTicks = 30},
        .expression = {.spread = 8},
        .gestures = kGestures,
        // The preset's volume knob (tuning): about 10.7 dB under the rest of the mix (delugemu captures of each part
        // alone, seed 1, a card with the pack), and no part plays above its preset's level.
        .volume = 100,
    },
    {.role = Role::FX, .name = "FX", .lanes = kFxLanes, .gestures = kGestures},
};
static_assert(kParts[kDrumsPart].role == Role::Drums);
static_assert(kParts[kBassPart].role == Role::Bass);
static_assert(kParts[kStabPart].role == Role::Chords && kParts[kStabPart].flavour == Flavour::Stab);
static_assert(kParts[kStringsPart].role == Role::Pad && kParts[kStringsPart].flavour == Flavour::Sustain);
static_assert(kParts[kLeadPart].role == Role::Lead && kParts[kLeadPart].flavour == Flavour::Motif);

/// A Swap on Clap and Snare, the held-fill entry, and a snare Roll of 1 or 2 beats (spec §8.7).
constexpr PartsAndLanes kSnareMask{.lanes = laneBit(Lane::Snare)};
constexpr FillEntry kFills[] = {
    {.kind = FillKind::Swap, .mask = {.lanes = laneBit(Lane::Clap) | laneBit(Lane::Snare)}, .beats = 2, .weight = 2},
    {.kind = FillKind::Roll, .mask = kSnareMask, .beats = 1, .weight = 1},
    {.kind = FillKind::Roll, .mask = kSnareMask, .beats = 2, .weight = 1},
    {.kind = FillKind::Rest,
     .mask = {.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)},
     .beats = 2,
     .weight = 2},
    {.kind = FillKind::Rest, .mask = {.lanes = laneBit(Lane::Kick)}, .beats = 4, .weight = 1},
};

/// Home i, with the brief's moves each played both ways, the way back to i by the home pull (kMaxChordMoves was 8), so
/// every chord the walk reaches has a way on that isn't home. Dorian adds i–IV through its colour move; in Major the
/// same degrees read I–IV and so on in Mixolydian. Weights in the colour moves' units (20): i–♭VII and ♭VI–♭VII at half
/// the others', so ♭VII doesn't take a third of minor bars (#371).
constexpr ChordMove kHouseMoves[] = {
    {kI, kIV, 30},   // i–iv
    {kI, kVII, 12},  // i–♭VII
    {kIV, kVII, 25}, // iv–♭VII
    {kVII, kIV, 15}, // ♭VII–iv
    {kI, kVI, 20},   // i–♭VI
    {kVI, kVII, 15}, // ♭VI–♭VII
    {kVII, kVI, 15}, // ♭VII–♭VI
};

/// 2 bars a chord, a repeat now and then so some last 4 and a few longer, the cadence on ♭VII every 4 chords (spec
/// §4.5). The repeat weight has no cap and weighs most where the guard thins the other ways on: at 15 one bar in 7
/// sat in a hold of 6 bars or more, at 8 one in 16. The chords loop (spec §7.4, round 2's S1): a 2- or 4-chord loop
/// held through the Break and the Build, a new one on 1 Drop in 3 (S1's other variants change that), and one Drop's
/// move in 3 goes back to an earlier loop (the owner, round 2: "might also go back to a previous loop", tuning).
constexpr ChordGraph kHouseGraph{
    .homePull = 20,
    .moves = kHouseMoves,
    .repeatWeight = 8,
    .cadenceTarget = kVII,
    .cadencePeriod = 4,
    .barsPerChord = 2,
    .loopChance = 333,
    .halfLoopChance = 333,
    .returnChance = 333,
};

/// House's graph moving to another loop at `chance` per-mille on each Drop (S1).
constexpr ChordGraph loopsAt(int16_t chance) {
	ChordGraph graph = kHouseGraph;
	graph.loopChance = chance;
	return graph;
}
constexpr ChordGraph kHalfDropsGraph = loopsAt(500);
constexpr ChordGraph kEveryDropGraph = loopsAt(1000);
constexpr ChordGraph kOneLoopGraph = loopsAt(0);

constexpr Scale kScales[] = {Scale::Minor, Scale::Dorian, Scale::Mixolydian};

/// 4 bars once in three, 8 bars twice (spec §4.5).
constexpr WeightedBars kBuildBars[] = {{4, 1}, {8, 2}};

// Variant sets (#461, #496, ADR 0008): the owner asked in round 2 for S1, S4a and S6 to vary per piece. Today's first
// in each; weights are tuning.

/// S1, how often a Drop brings another chord loop: 1 Drop in 3 (round 2's pick), half of them (the owner's note: about
/// two thirds of pieces draw it, #472 round 3), every Drop or never (one loop for the whole piece). Weights tuning.
/// Live: only the graph's loop chance moves, read on the Drop's own step after dropVariants() lands it.
constexpr Variant kLoopVariants[] = {
    {.name = "third", .oledName = "1 IN 3", .weight = 1},
    {.name = "half",
     .oledName = "1 IN 2",
     .weight = 6,
     .apply = [](ResolvedGenre& resolved) { resolved.genre.harmony.graph = &kHalfDropsGraph; }},
    {.name = "every",
     .oledName = "EVERY",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.genre.harmony.graph = &kEveryDropGraph; }},
    {.name = "never",
     .oledName = "NEVER",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.genre.harmony.graph = &kOneLoopGraph; }},
};

/// S4a, the Build's shape (round 2's QBK options): everything (today), no Lead (it returns on the Drop), the kick and
/// Bass cut for the last 2 bars, or no kick or Bass in the whole Build. Not live: the Arc reads the Build ahead.
constexpr Variant kBuildVariants[] = {
    {.name = "full", .oledName = "FULL", .weight = 2},
    {.name = "nolead",
     .oledName = "NO LEAD",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.genre.arc.buildKeeps = kEverything.without({.parts = partBit(kLeadPart)});
         }},
    {.name = "cut2",
     .oledName = "CUT 2",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         resolved.genre.arc.buildCut = {.parts = partBit(kBassPart), .lanes = laneBit(Lane::Kick)};
	         resolved.genre.arc.buildCutBars = 2;
         }},
    {.name = "nokick",
     .oledName = "NO KICK",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         // The Drums by lane, so the kick alone leaves.
	         resolved.genre.arc.buildKeeps =
	             kEverything.without({.parts = static_cast<uint8_t>(partBit(kDrumsPart) | partBit(kBassPart)),
	                                  .lanes = laneBit(Lane::Kick)});
         }},
};

/// S6, the Bass's pickups (#369): the 5th from Complexity 25 (round 2's A, today), the 5th only from Complexity 34
/// (round 2's today), or B's pickups. The owner liked them all and kept A for now, so A is the heaviest. Not live: the
/// pickups' pool has five more entries, and a frozen Bass holds its entry across a Drop.
constexpr Variant kBassVariants[] = {
    {.name = "fifth", .oledName = "5TH", .weight = 2},
    {.name = "root",
     .oledName = "ROOT",
     .weight = 1,
     .apply = [](ResolvedGenre& resolved) { resolved.parts[kBassPart].loop.draws.leap = {0, 6}; }},
    {.name = "pickups",
     .oledName = "PICKUPS",
     .weight = 1,
     .apply =
         [](ResolvedGenre& resolved) {
	         PartSpec& bass = resolved.parts[kBassPart];
	         bass.pool = kPickupBass;
	         bass.loop.degrees = kPickupDegrees;
	         bass.loop.draws.leap = {4, 8}; // tuning
	         bass.loop.rootOnAnchors = true;
         }},
};

constexpr VariantSet kVariantSets[] = {
    {.name = "loops", .oledName = "LOOPS", .variants = kLoopVariants, .live = true},
    {.name = "build", .oledName = "BUILD", .variants = kBuildVariants},
    {.name = "bass", .oledName = "BASS", .variants = kBassVariants},
};

constexpr Genre kHouse{
    .name = "house",
    .oledName = "HOUSE",
    .version = 6, // a retune that changes its event stream raises it by one (spec 4.4, 21.2)
    .facts = {.tempoLowBpm = 120, .tempoHighBpm = 126, .scales = kScales, .swingLow = 10, .swingHigh = 20},
    .phraseBars = 8,
    .parts = kParts,
    .fills = kFills,
    .heldFill = 0,
    .fillChance = 350,
    .sectionEndChance = 1000,
    // Every chord with a 7th, usually, and a second Extension as Complexity rises (spec §7.4).
    .harmony = {.graph = &kHouseGraph, .extensionFloor = 1, .extensionCeiling = 2, .guard = {.factor = 0}},
    .palette = &housePalette,
    .arc =
        {
            .introPhrases = 2,
            .outroPhrases = 1,
            .groovePhrases = {4, 8},
            .breakPhrases = {1, 2},
            .buildBars = kBuildBars,
            .grooveToBuild = 0,
            .dropDecayPhrases = 1,
            .offsets =
                {
                    {macroPoints(-15), 0},                                                  // Intro
                    {},                                                                     // Groove
                    {macroPoints(-10), macroPoints(-10), macroPoints(-5), macroPoints(-5)}, // Break: a soft dip
                    {0, macroPoints(12), 0, macroPoints(10)},                               // Build
                    {0, macroPoints(-15)},                                                  // Outro
                },
            // The breakdown is the Strings and the Stab over the closed hat. Its Density dips by 5, not 20: the
            // closed hat has no anchors.
            .breakKeeps = {.parts = partBit(kStabPart) | partBit(kStringsPart), .lanes = laneBit(Lane::ClosedHat)},
            // Builds play everything: no cut, so the kick and bass run on into the Gap.
            .buildKeeps = kEverything,
            .buildCutBars = 0,
            .gap = GapLength::Beat,
            .roll = {.lane = Lane::Snare, .top = RollStage::ThirtySecond},
            .accentLane = Lane::Crash,
            // a Crash on Drops, after a fill that ends a phrase, and at 150 per-mille on other phrase starts
            .accentChance = 150,
        },
    .variantSets = kVariantSets,
};
static_assert(genreValid(kHouse), "the genre data holds every rule genreValid() lists (genre_data_spec names one)");

} // namespace

const Genre house = kHouse;

} // namespace deluge::generative::genres
