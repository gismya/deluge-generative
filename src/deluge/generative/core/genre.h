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

// What a genre is (spec §4): constant data read by one generic Generator, never code for one genre. Every field has a
// Generator-wide default here, and a genre lists only the fields it changes (with designated initialisers). Adding a
// field means adding it here with a default that keeps today's behaviour.
//
// Slots filled so far: identity, musical facts, macros, meter and phrase, a part roster with Drums lanes and Loop
// parts (with Registers and Degree sets), rhythm (pattern pools with tiers, fill entries), harmony (a Chord graph with
// its harmonic rhythm, and the Extension floor and ceiling), the palette and the Arc's offsets and lengths. Later
// slices extend these structs; they don't add per-genre code.

#pragma once

#include "generative/core/chord_walk.h"
#include "generative/core/palette.h"
#include "generative/core/pitch.h"
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace deluge::generative {

// ---- Rhythm (slot 6, spec §8.1) ----

/// How hard a pattern step plays.
enum class VelocityLevel : uint8_t { Ghost, Normal, Accent };

/// Step importance: 0 never plays, kAnchor always plays (an anchor), 1–9 play as Density allows.
constexpr uint8_t kAnchor = 10;
constexpr int32_t kMaxPatternSteps = 32;
/// Complexity tiers run 0 (plain) to kMaxTier (busiest).
constexpr uint8_t kMaxTier = 3;

/// One pool entry: 1 or 2 bars of steps, each with an importance and a velocity level, a Complexity tier (0 to
/// kMaxTier) and a weight above 0 for the pick within its tier. A pool lists its entries by tier, lowest first.
struct Pattern {
	uint8_t length = 0;
	uint8_t tier = 0;
	uint8_t weight = 1;
	uint8_t importance[kMaxPatternSteps] = {};
	VelocityLevel level[kMaxPatternSteps] = {};

	/// Written as two strings of 16 or 32 characters, one per step, so tables read like a step sequencer:
	///   importance: '.' never, '1'–'9' as Density allows, 'X' anchor
	///   levels:     'g' ghost, 'a' accent, anything else normal; a 'g' or 'a' over a '.' is a compile-time error
	template <int32_t N>
	consteval Pattern(const char (&importances)[N], const char (&levels)[N], uint8_t complexityTier, // NOLINT
	                  uint8_t pickWeight = 1)
	    : length(N - 1), tier(complexityTier), weight(pickWeight) {
		static_assert(N - 1 == 16 || N - 1 == 32, "a pattern is 1 or 2 bars of 16ths");
		if (complexityTier > kMaxTier || pickWeight == 0) {
			throw "a pattern's tier is 0 to kMaxTier and its weight at least 1";
		}
		for (int32_t i = 0; i < N - 1; i++) {
			char c = importances[i];
			if (c == '.') {
				importance[i] = 0;
				if (levels[i] == 'g' || levels[i] == 'a') {
					throw "a level mark ('g' or 'a') sits on a step that never plays ('.')";
				}
			}
			else if (c == 'X') {
				importance[i] = kAnchor;
			}
			else if (c >= '1' && c <= '9') {
				importance[i] = static_cast<uint8_t>(c - '0');
			}
			else {
				throw "pattern importance must be '.', '1'-'9' or 'X'";
			}
			level[i] = levels[i] == 'g'   ? VelocityLevel::Ghost
			           : levels[i] == 'a' ? VelocityLevel::Accent
			                              : VelocityLevel::Normal;
		}
	}
};

/// The lowest and highest Complexity tier a part or lane plays (spec §8.1).
struct TierBand {
	uint8_t low = 0;
	uint8_t high = 0;
};

/// Density thinning (spec §8.1): the importance a non-anchor step needs, plus its jitter, to play at each effective
/// Density, in importance hundredths. Given every 10 Density points and interpolated between.
struct DensityCurve {
	static constexpr int32_t kKnots = kMacroPoints / 10 + 1;
	int16_t knots[kKnots] = {};

	[[nodiscard]] constexpr int32_t threshold(int32_t density) const {
		return interpolateMacro(knots, kKnots, density);
	}
};

/// The Generator-wide curve (tuning). Importance 9 comes in near Density 5 and each step down about 5 points later,
/// so Density 40 still holds back the lowest two; full Density plays every step.
constexpr DensityCurve kDefaultDensityCurve{{1000, 820, 640, 460, 280, 0}};

// ---- Gates (spec §5.6) ----

/// An open gate closes only once Density falls this far below its threshold (tuning).
constexpr int32_t kGateHysteresis = macroPoints(2);

/// Whether a gate is open after a bar line at `density`, given whether it was open before it.
constexpr bool gateOpens(bool wasOpen, int32_t threshold, int32_t density) {
	return wasOpen ? density > threshold - kGateHysteresis : density >= threshold;
}

// ---- Interplay rules (slot 6, spec §5.5) ----

/// What a rule marks as against.
enum class RuleKind : uint8_t {
	/// Against every step where the target has no onset. With `offBeat` (the spec's `off` option), every step except
	/// the target's onsets one 8th
	/// later (wrapping inside its pattern), so a four-on-the-floor kick leaves the & steps free.
	Lock,
	/// Against every step where the target has a planned onset. A pitched source's note never sounds into one it
	/// doesn't play, and genreValid() keeps the source's anchors off every step the target can play
	/// (avoidRulesValid()).
	Avoid,
	/// Against the half of each bar where the target has more planned onsets, the first on a non-zero tie, neither
	/// when both are empty, counted at the half-bar's first step (Generator::countHalves()). Its target neither lists
	/// a Respond rule nor reads one through its own rules (interplayRulesValid()).
	Respond,
};

/// What a rule reads: a part, by its index in the roster, or one of a Drums part's lanes. A Drums part with no lane
/// named reads the union of its lanes.
struct RuleTarget {
	uint8_t part = 0;
	Lane lane = Lane::None;
};

/// A filter on the part or lane that lists it: on an against step, each onset that isn't an anchor is dropped when a
/// keyed draw falls below the rule's effective strength.
struct InterplayRule {
	RuleKind kind = RuleKind::Lock;
	RuleTarget target;
	bool offBeat = false;
	/// Percent, at the genre's default Complexity.
	int8_t strength = 100;
	/// Percentage points the strength moves over Complexity's whole 0–50 range, signed.
	int8_t sensitivity = 0;
};

/// A rule at full strength (100 %), in effectiveStrength()'s units.
constexpr int32_t kFullStrength = 100 * kMacroFull;

/// A rule's effective strength, 0 to kFullStrength, in 1/kMacroFull-percent units: its strength moved by its
/// sensitivity over the Recipe's Complexity away from the genre's default (spec §5.5).
constexpr int32_t effectiveStrength(const InterplayRule& rule, int32_t complexity, int32_t defaultComplexity) {
	const int32_t strength = rule.strength * kMacroFull + rule.sensitivity * (complexity - defaultComplexity);
	return strength < 0 ? 0 : strength > kFullStrength ? kFullStrength : strength;
}

// ---- Expression (spec §8.2, §8.4, §8.5) ----

/// What a Velocity spread nudge is keyed on (spec §8.4).
enum class SpreadMode : uint8_t {
	Repeat, ///< the step's place in the pattern: the nudge repeats with the pattern
	Fresh,  ///< the absolute Arc step: every pass differs
};

/// The Ornaments a part or lane may play, and how often (spec §8.2).
struct Ornaments {
	/// Chance per note, per-mille, at Complexity 0 (floor) and 50 (ceiling), linear between.
	int16_t rateFloor = 0;
	int16_t rateCeiling = 0;
	/// Draw weights of a ratchet of 2, 3 or 4 hits, and of a flam. All 0 plays none.
	uint8_t ratchet[3] = {};
	uint8_t flam = 0;
	/// Ticks a flam's grace hit plays before the main hit, 2 to 6 (tuning).
	uint8_t flamGap = 3;
};

/// A part's or lane's expression stage settings.
struct Expression {
	/// Ticks off the grid on the 1, e, & and a of each beat, -kMaxLean to kMaxLean (spec §8.5).
	int8_t lean[kStepsPerBeat] = {};
	/// The most a seeded nudge moves a note's velocity either way, after Energy's layer (spec §8.4).
	uint8_t spread = 0;
	SpreadMode spreadMode = SpreadMode::Repeat;
	Ornaments ornaments;
};

// ---- Parts and lanes (slot 5, spec §5) ----

/// A tempo-synced delay time's note (spec §10.6): a whole note down to a 32nd, the range the stock sync menu offers,
/// for a genre's tables to pick from (Dub, #131, the first).
enum class DelayNote : uint8_t { Whole, Half, Quarter, Eighth, Sixteenth, ThirtySecond };
/// A delay time's feel: even, dotted (×1.5) or triplet (×2/3).
enum class DelayFeel : uint8_t { Even, Dotted, Triplet };

/// A delay setup (spec §10.6): the four plain fields of a sound's Delay the Generator writes on every load of the
/// sound, a chosen one included. Feedback home stays as the preset saved it. A part, kit row or kit with one gets the
/// delay Throw (spec §11.4); one with none (`on` false) throws reverb only.
struct DelaySetup {
	bool on = false;
	DelayNote note = DelayNote::Eighth;
	DelayFeel feel = DelayFeel::Even;
	bool pingPong = false;
	bool analog = false;
	constexpr bool operator==(const DelaySetup&) const = default;
};

struct LaneSpec {
	Lane lane = Lane::Kick;
	std::span<const Pattern> pool;
	/// The lane's fill patterns, which only a Swap plays (spec §8.7). Tiered and weighted like `pool`, in `band`.
	std::span<const Pattern> fillPool;
	TierBand band;
	std::span<const InterplayRule> rules;
	/// Plays only while effective Density is at or above this, in macro units (spec §5.6). 0 always plays.
	int16_t densityGate = 0;
	/// Ticks from a hit's note-on to its note-off. Drum one-shots under 2 s ignore it and play out.
	uint8_t gateTicks = 6;
	Expression expression;
	/// The lane's row's delay setup (spec §10.6): a Drums or FX row is a Sound with its own delay.
	DelaySetup delay;
	/// A Siren lane's held note, in bars (spec §9.5, tuning): 1 up to the phrase. 0 on every other lane.
	uint8_t heldBars = 0;
	/// An optional alternate lane in the same Drums part that takes this lane's hits for whole Groove bars, drawn per
	/// bar with per-mille chance `alternateChance` (spec §5.7; Dub's Rim -> Snare). Lane::None: never swaps.
	Lane alternateLane = Lane::None;
	int16_t alternateChance = 0;
};

/// One value a line step may draw, with its draw weight (spec §5.2): a Loop step's scale step from the chord root, or
/// a Motif slot's chord position (0 the root, 1 the next chord tone up and so on, a position past the chord's last
/// note going on an octave up; a negative one counts down from the root).
struct LineWeight {
	int8_t value = 0;
	uint8_t weight = 1;
};

/// An Answer's transform (spec §5.2): a read-time change to the line, played in place of the call.
enum class AnswerKind : uint8_t {
	None,        ///< no Answer: the call plays. Never in an Answer list
	Sparse,      ///< silences the odd steps
	AccentsOnly, ///< silences the unaccented steps
	Octave,      ///< each step an octave up or down
	Fifth,       ///< each step 4 scale steps up or down
	Third,       ///< each step 2 scale steps up or down
	Reverse,     ///< the k-th of n onsets plays onset n - 1 - k's pitch content
	Mirror,      ///< each pitch reflected about the middle of the line's range
	Rotate,      ///< step i plays step i - 4's pitch content: one beat late
};

/// Each transform's fixed Generator-wide Answer tier, plain (0) to intricate (3).
constexpr uint8_t answerTier(AnswerKind kind) {
	switch (kind) {
	case AnswerKind::None:
	case AnswerKind::Sparse:
	case AnswerKind::AccentsOnly:
		return 0;
	case AnswerKind::Octave:
	case AnswerKind::Fifth:
		return 1;
	case AnswerKind::Third:
	case AnswerKind::Reverse:
		return 2;
	case AnswerKind::Mirror:
	case AnswerKind::Rotate:
		return 3;
	}
	return 0;
}

/// One transform in a Loop part's Answer list, with its weight within its tier.
struct Answer {
	AnswerKind kind;
	uint8_t weight;
	uint8_t tier; ///< answerTier(kind), never set apart from it.

	constexpr Answer(AnswerKind kind, uint8_t weight = 1) // NOLINT(google-explicit-constructor)
	    : kind(kind), weight(weight), tier(answerTier(kind)) {}
};

/// A value the live Complexity sets (spec §8.1): `atZero` at Complexity 0 to `atFull` at 50, linear between, in
/// integer math. One number holds at every Complexity.
struct ByComplexity {
	int16_t atZero = 0;
	int16_t atFull = 0;

	constexpr ByComplexity() = default;
	constexpr ByComplexity(int16_t both) : atZero(both), atFull(both) {} // NOLINT(google-explicit-constructor)
	constexpr ByComplexity(int16_t atZero, int16_t atFull) : atZero(atZero), atFull(atFull) {}
	[[nodiscard]] constexpr int32_t at(int32_t complexity) const {
		return atZero + (atFull - atZero) * complexity / kMacroFull;
	}
};

/// How each step of a line is rolled, a Loop's or a Motif's (spec §5.2, §8.1), at a pick or a pass end: from the
/// live Complexity for the leap range and the octave-jump chances. Chances are per-mille.
struct LineDraws {
	/// How far a step may leap: the largest value, either side of 0, it may draw (tuning). A Loop's values are scale
	/// steps from the chord root, a Motif's chord positions.
	ByComplexity leap = kDegreesPerScale;
	ByComplexity octaveUp = 100; ///< (tuning)
	ByComplexity octaveDown = 0; ///< (tuning)
	int16_t accent = 200;        ///< (tuning)
};

/// How a Loop part rolls and mutates its line (spec §5.2, §8.1). Chances are per-mille. The live Complexity sets
/// the leap range, the octave-jump chances and the slide chance of every step rolled, at a pick or by mutation.
struct LoopSpec {
	/// The scale steps from the chord root a step may draw.
	std::span<const LineWeight> degrees;
	LineDraws draws;
	/// When set, an anchor step always plays the chord root in the line's own octave (spec §5.2; Dub's Bass).
	/// Loop only (loopNotesValid()).
	bool rootOnAnchors = false;
	ByComplexity slide = 150;
	/// Chance a non-anchor step re-rolls at each pass end, at Motion 25 (tuning).
	int16_t mutation = 120;
	/// The Answer transforms the part plays, with weights (spec §5.2). Empty: it plays no Answers.
	std::span<const Answer> answers;
	/// N: an Answer every N bars at Motion 25 (tuning).
	uint8_t answerBars = 4;
	/// Ticks a plain note sounds.
	uint16_t gateTicks = 16;
	/// How far a slid note runs into the next step, so a LEGATO voice glides.
	uint8_t slideOverlapTicks = 6;
	/// Bit i: an octave jump may land on step i of each bar's 16 (line step modulo 16: a line's step is its pattern's
	/// step, line[patternStep]); a step off the mask
	/// keeps its drawn octave at 0, so the line's high and low notes mark a grouping (spec §5.2; Psytrance's 3-3-2).
	/// 0: any step (every genre before #497).
	uint16_t jumpSteps = 0;
	/// Per-mille chance a plain note on the pitch the part still sounds ties into it: no new strike, and the sounding
	/// note lasts to the new one's end (spec §5.2, §8.6). A rest, a Section line, a slide, an accent or a mute change
	/// ends a tie.
	uint16_t tieChance = 0;
};

/// How a Sustain part holds its voicing (spec §5.2, §8.1).
struct SustainSpec {
	/// A drone: the key's root and fifth only, ignoring chord changes and Extensions.
	bool pedal = false;
	/// The voicing also re-strikes on every Nth bar line counted from the phrase line, 0 for never. It must divide
	/// the phrase or be a whole multiple of it; a multiple counts whole phrases from the Section's start.
	uint8_t retriggerBars = 0;
	/// Adds the root an octave up, the lowest-importance note, when the voicing has room under 4 notes.
	bool doubleRoot = false;
	/// The first Extension plays wherever the part does, at any Density, as the root does: a genre whose every chord
	/// must sound its Extension (Detroit techno's Strings, spec §4.5). A pedal has none.
	bool anchorExtension = false;
	/// When set, a Section line the part plays on both sides of holds the notes whose pitch stays, at the velocity
	/// they started with, even on a chord change, and only a changed pitch ends and strikes there: no re-strike on
	/// the Drop's step (Ambient's Pad and Drone, spec §5.2). The one exception is the first Groove's first line, where
	/// the whole voicing strikes once, so a piece begun in an Intro plays at the Groove's level, not the Intro's.
	/// Every other cut still re-strikes it. Sustain only (sustainValid()).
	bool holdsAcrossSections = false;
	/// When set, a chord change holds the notes whose pitch the new voicing keeps, at the velocity they started with,
	/// and only the changed pitches end and strike (Ambient's Pad, spec §5.2). A Section line follows
	/// holdsAcrossSections. Sustain only (sustainValid()).
	bool holdsCommonTones = false;
	/// An open voicing (openVoicing()): the first chord puts the root and the 5th near the tonic and the 3rd and the
	/// Extensions an octave up, and each later one moves each voice the least, so the spread holds. Wants a Register
	/// about two octaves wide (Ambient's Pad, spec §5.2). A Sustain voicing chords only, with no doubled root
	/// (sustainValid()).
	bool openVoicing = false;
	/// With an open voicing: the per-mille chance, drawn on each bar line, that of the pairings that move the voices
	/// least the voicing takes the one holding the most of them in place, so a shared note stays put and doesn't strike
	/// (with holdsCommonTones). Otherwise it takes the first found, which may move a shared note an octave and strike
	/// it there (Ambient's Pad: owner, round 2's HOLD, spec §5.2). 0 to 1000; only with openVoicing (sustainValid()).
	uint16_t holdSharedChance = 0;
};

/// How a Stab part plays its chords (spec §5.2, §8.1): the voiced chord on each onset of its pattern.
struct StabSpec {
	/// Ticks a stab sounds (tuning). It ends sooner at the pattern's next onset or the next bar line, where the chord
	/// may change, so a stab overlaps the next one or the next chord by no more than its Lean or a flam's grace hit.
	uint16_t gateTicks = 2 * kTicksPerStep;
};

/// The order an Arp walks its chord's notes in: a fixed Generator-wide set (spec §8.1).
enum class ArpOrder : uint8_t {
	Up,     ///< lowest to highest, then from the lowest again
	Down,   ///< highest to lowest
	UpDown, ///< up, then down, the top and bottom notes played once each turn
	Random, ///< a keyed draw from the walk's notes on each onset
};

/// The most octaves an Arp walk climbs (spec §8.1).
constexpr uint8_t kMaxArpOctaves = 2;

/// An Arp pool entry's walk: its order and how many octaves of the chord's notes it climbs (spec §8.1).
struct ArpWalk {
	ArpOrder order = ArpOrder::Up;
	uint8_t octaves = 1;
};

/// Semitones a Register must span to hold a walk of `octaves` from any chord root without folding a note: the root
/// may sit up to 11 semitones above the span's bottom, and the last octave tops out at a 7th, 11 above its root
/// (spec §7.3). A 9th, one Extension level higher, may fold down an octave.
constexpr int32_t arpRange(int32_t octaves) {
	return 11 + 12 * (octaves - 1) + 11;
}

/// How an Arp part walks its chords (spec §5.2, §8.1).
struct ArpSpec {
	/// Each pool entry's walk, by its index in the part's pool: `walks[i]` goes with `pool[i]`.
	std::span<const ArpWalk> walks;
	/// Ticks a note sounds (tuning). It ends sooner at the pattern's next onset (before thinning, so the length never
	/// depends on Density) or the next bar line, where the chord may change: one note at a time (spec §2.7).
	uint16_t gateTicks = kTicksPerStep;
};

/// How a Motif part rolls and re-rolls its line of chord positions (spec §5.2, §8.1). Chances are per-mille. The
/// live Complexity sets the leap range and the octave-jump chances of every slot rolled, at a pick or a re-roll.
struct MotifSpec {
	/// The chord positions a slot may draw: 0 the root, then up through the chord's notes taken into one octave (a
	/// 9th counts as a 2nd), a position past the last going on an octave up; a negative one counts down (spec §5.2).
	std::span<const LineWeight> positions;
	LineDraws draws;
	/// Chance a non-anchor slot re-rolls its chord position and octave at each pass end, at Motion 25 (tuning).
	int16_t reroll = 120;
	/// Ticks a note sounds (tuning). It ends sooner at the pattern's next onset (before thinning) or the next bar
	/// line, where the chord may change: one note at a time (spec §2.7), long only where the onsets are sparse.
	uint16_t gateTicks = kTicksPerBeat;
};

/// A Sustain voicing note's place in the chord, which sets its importance (spec §8.1).
enum class VoicePlace : uint8_t { Root, Fifth, Third, Extension, SecondExtension, Doubling };
constexpr int32_t kVoicePlaces = static_cast<int32_t>(VoicePlace::Doubling) + 1;
/// Each place's importance (tuning): the root is an anchor, then the fifth, the third (or the sus4 in its place), the
/// Extensions and the doubled root. Read against the Density threshold like a pattern step's, with a jitter keyed on
/// the place.
constexpr uint8_t kVoiceImportance[kVoicePlaces] = {kAnchor, 8, 7, 6, 6, 4};

// ---- Sound controls: gestures and drift (spec §11.4, §11.5) ----

/// The gestures with switches and depths. Reset has neither: it only clears (spec §11.4).
/// `Count` stays last: it counts the gestures, wherever one is added.
enum class Gesture : uint8_t { Close, Wash, Rise, Throw, Count };
constexpr int32_t kNumGestures = static_cast<int32_t>(Gesture::Count);

/// Each "When" entry of a gesture, with its own switch per part (spec §11.4).
enum class GestureMoment : uint8_t {
	CloseBreak,      ///< Close, entering a Break
	CloseOutro,      ///< Close, across an Outro
	WashBreak,       ///< Wash, during a Break
	WashOutro,       ///< Wash, across an Outro
	RiseBuild,       ///< Rise, across a Build
	RiseIntro,       ///< Rise, across an Intro
	ThrowBreak,      ///< Throw, on the last hits before a Break
	ThrowGap,        ///< Throw, on the last hits before a Gap
	ThrowTransition, ///< Throw, on the outgoing parts' last hits in a transition (spec §11.4, §12.9)
	Count,           ///< Not a moment: stays last, and counts them.
};
constexpr int32_t kNumGestureMoments = static_cast<int32_t>(GestureMoment::Count);
static_assert(kNumGestureMoments <= 16, "GestureSpec::off and momentBit() hold a bit per moment in 16 bits");
constexpr uint16_t momentBit(GestureMoment moment) {
	return static_cast<uint16_t>(1U << static_cast<uint32_t>(moment));
}

/// `value` for each gesture: a GestureSpec default that covers every Gesture, however many there are.
template <typename T>
constexpr std::array<T, kNumGestures> eachGesture(T value) {
	std::array<T, kNumGestures> out{};
	out.fill(value);
	return out;
}

/// A part's gestures. Every switch defaults to on, so a genre lists only the moments it turns off (spec §11.4).
struct GestureSpec {
	uint16_t off = 0; ///< momentBit() per moment switched off.
	/// Each gesture's depth on the part, per-mille ×0..×2, by Gesture.
	std::array<int16_t, kNumGestures> depth = eachGesture<int16_t>(1000);
	/// On a Drums part, a lane a gesture acts on instead of the kit-wide controls (Thin on the snare in a Build roll,
	/// say), by Gesture. Lane::None: kit-wide. FX lanes always use their own controls, so an FX part ignores it.
	std::array<Lane, kNumGestures> aim = eachGesture(Lane::None);

	[[nodiscard]] constexpr bool on(GestureMoment moment) const { return (off & momentBit(moment)) == 0; }
};

/// How a part makes room for another while that one plays (spec §5.6, #497): it rests, or keeps only its 8ths.
enum class YieldMode : uint8_t { None, Rest, Eighths };

/// A part yielding to another (spec §5.6, #497; Psytrance's Arp under its Lead). It holds on each step where `part`
/// sounds by the Arc's rules (its gate open, kept by the Section, no drop-out; mutes never count), never in a Build:
/// `inDrops` in a Groove that started on a Drop, `outside` everywhere else. A patterned pitched part only, yielding
/// to another part (yieldValid()). part -1: it never yields (every genre before #497).
struct YieldSpec {
	int8_t part = -1;
	YieldMode inDrops = YieldMode::None;
	YieldMode outside = YieldMode::None;
};

/// A part's drift (spec §11.5).
struct DriftSpec {
	/// Depth at a given Motion, per-mille ×0..×2 of the Generator-wide depth.
	int16_t weight = 1000;
	/// Brightness drifts as a Sweep with this period in bars instead of the walk; 0 for the walk. It must divide the
	/// phrase or be a whole multiple of it.
	uint8_t sweepBars = 0;
};

struct PartSpec {
	Role role = Role::Drums;
	Flavour flavour = Flavour::None;
	const char* name = "";
	/// Drums and FX parts: their lanes, each with its own pool.
	std::span<const LaneSpec> lanes;
	/// Patterned pitched parts: the part's pool and tier band.
	std::span<const Pattern> pool;
	TierBand band;
	/// Patterned pitched parts: the part's interplay rules. A Kit part has none: its lanes carry their own.
	std::span<const InterplayRule> rules;
	/// The part plays only while effective Density is at or above this, in macro units (spec §5.6). 0 always plays.
	/// A lane plays only while its part's gate and its own are both open.
	int16_t densityGate = 0;
	Register reg;
	/// Bass and Lead parts playing Loop, Arp or Motif: the degrees their notes snap to (spec §7.5).
	DegreeSet degreeSet = DegreeSet::Full;
	LoopSpec loop;
	/// Sustain parts.
	SustainSpec sustain;
	/// Stab parts.
	StabSpec stab;
	/// Arp parts.
	ArpSpec arp;
	/// Motif parts.
	MotifSpec motif;
	Expression expression;
	GestureSpec gestures;
	DriftSpec drift;
	/// The post-FX volume knob at load, as a percent of the preset's knob position (spec §10.6, tuning): a synth
	/// part's, or each row's of a Kit part (Drums, FX). 100 keeps the preset's; never above it (1–100).
	uint8_t volume = 100;
	/// Whom the part makes room for, and how (spec §5.6). Off by default.
	YieldSpec yield;
	/// The part's own delay setup (spec §10.6): a synth part's sound's, or on a Kit part (Drums, FX) the kit's, at kit
	/// level. Each lane's row has its own on its LaneSpec. Only Drums has a kit-wide control set, so an FX kit's setup
	/// is written on load and never thrown: FX lanes throw their own rows (spec §11.4).
	DelaySetup delay;
	/// How far a Throw lifts delay feedback above home where the part, a row or its kit has a delay setup, in menu
	/// units, before the driver's cap at 23 (spec §11.4, tuning).
	uint8_t throwDelayDepth = 12;

	[[nodiscard]] constexpr Register registerOrDefault() const { return reg.anchor ? reg : defaultRegister(role); }
	/// A synth part plays one card preset; a Kit part (Drums, FX) plays its lanes' sounds instead (spec §10.1).
	[[nodiscard]] constexpr bool synth() const { return lanes.empty(); }
};

/// How many of a Kit part's lanes the Generator plays: its lanes, up to kMaxLanesPerPart.
constexpr int32_t laneCount(const PartSpec& part) {
	return std::min<int32_t>(static_cast<int32_t>(part.lanes.size()), kMaxLanesPerPart);
}
/// The index of `lane` in a Kit part's lanes, or -1.
constexpr int32_t laneIndexOf(const PartSpec& part, Lane lane) {
	for (int32_t l = 0; l < laneCount(part); l++) {
		if (part.lanes[l].lane == lane) {
			return l;
		}
	}
	return -1;
}

/// "`replacing` replaces `replaced`": lanes of one part play independently, except that when both land on the same
/// step, the replaced lane's hit is dropped before any voice starts (spec §5.7).
struct ReplacePair {
	Lane replacing = Lane::OpenHat;
	Lane replaced = Lane::ClosedHat;
};

/// The Generator-wide default, which every v1 genre uses. The Kit's CHOKE group still cuts the hat tails (§10.6).
constexpr ReplacePair kDefaultReplacePairs[] = {{Lane::OpenHat, Lane::ClosedHat}};

// ---- Harmony (slot 7, spec §7.4) ----

/// The genre's Harmony slot. Empty (no Chord graph) means the home chord only, which still gets its Extensions: no
/// walk, no colour moves, no cadences and no home bonuses.
struct HarmonySpec {
	/// The Chord graph with its harmonic rhythm, or nullptr for an empty slot. Constant data, never owned.
	const ChordGraph* graph = nullptr;
	uint8_t extensionFloor = 0;
	uint8_t extensionCeiling = 2;
	/// The order chords try their Extensions in (spec §7.4): the Generator-wide 7, 9, sus4, 6 unless the genre sets
	/// its own (Ambient's kNinthFirst).
	ExtensionOrder extensionOrder{};
	/// The chord guard's rule and factor (spec §7.4); the Generator-wide default unless the genre overrides it.
	ChordGuard guard{};
};

// ---- Section rules (slot 9, spec §9) ----

struct Range {
	uint8_t low = 1;
	uint8_t high = 1;
};

struct WeightedBars {
	uint8_t bars = 8;
	uint8_t weight = 1;
};

/// Value for a Density offset a genre leaves unset: it then uses its Energy offset (spec §9.4).
constexpr int16_t kSameAsEnergy = INT16_MIN;

/// One Section kind's offsets from the Recipe centre, in macro units, moving linearly from `from` on the Section's
/// first step to `to` on its last. A Build ignores `from`: it ramps from wherever the offset was when it started.
struct SectionOffsets {
	int16_t energyFrom = 0;
	int16_t energyTo = 0;
	int16_t densityFrom = kSameAsEnergy;
	int16_t densityTo = kSameAsEnergy;
};

constexpr WeightedBars kDefaultBuildBars[] = {{8, 1}};

constexpr uint16_t laneBit(Lane lane) {
	return static_cast<uint16_t>(1U << static_cast<uint32_t>(lane));
}

/// The bit of the roster's part `part` in PartsAndLanes::parts.
constexpr uint8_t partBit(int32_t part) {
	return static_cast<uint8_t>(1U << static_cast<uint32_t>(part));
}

/// Parts and lanes as a section rule names them: whole parts by their roster index, and single lanes.
struct PartsAndLanes {
	uint8_t parts = 0;  ///< partBit() per part: every lane of it.
	uint16_t lanes = 0; ///< laneBit() per lane.

	/// Whether it holds a part's note on `lane` (Lane::None for a pitched part's).
	[[nodiscard]] constexpr bool has(int32_t part, Lane lane) const {
		return (parts & partBit(part)) != 0 || (lane != Lane::None && (lanes & laneBit(lane)) != 0);
	}
	/// Whether it holds no part and no lane.
	[[nodiscard]] constexpr bool empty() const { return parts == 0 && lanes == 0; }
	/// Whether it shares a part or a lane with `other`.
	[[nodiscard]] constexpr bool intersects(const PartsAndLanes& other) const {
		return (parts & other.parts) != 0 || (lanes & other.lanes) != 0;
	}
	/// It with `other`'s parts and lanes too.
	[[nodiscard]] constexpr PartsAndLanes with(const PartsAndLanes& other) const {
		return {static_cast<uint8_t>(parts | other.parts), static_cast<uint16_t>(lanes | other.lanes)};
	}
	/// It without `other`'s parts and lanes.
	[[nodiscard]] constexpr PartsAndLanes without(const PartsAndLanes& other) const {
		return {static_cast<uint8_t>(parts & ~other.parts), static_cast<uint16_t>(lanes & ~other.lanes)};
	}
	constexpr bool operator==(const PartsAndLanes&) const = default;
};
constexpr PartsAndLanes kEverything{0xFF, 0xFFFF};
/// Build keeps left unset: the Break keeps (spec §9.5). Bit 15 names no lane.
constexpr PartsAndLanes kSameAsBreakKeeps{0, 0x8000};

/// An optional Build ending where the mix cuts out (spec §9.1).
enum class GapLength : uint8_t { None, Beat, Bar };
constexpr int32_t gapSteps(GapLength gap) {
	return gap == GapLength::Bar ? kStepsPerBar : gap == GapLength::Beat ? kStepsPerBeat : 0;
}

/// The Build roll's stages, slowest first (spec §8.3): hits every beat, 8th, 16th, then ratchets of 2 and 4 in each.
/// None is no roll: never a genre's top stage.
enum class RollStage : uint8_t { Beat, Eighth, Sixteenth, ThirtySecond, SixtyFourth, None };

struct BuildRoll {
	Lane lane = Lane::None; ///< None: no roll.
	/// The stage the roll stops climbing at: 16ths, 32nds or 64ths.
	RollStage top = RollStage::ThirtySecond;
	/// Each stage after the first starts with 1/fraction of the time before the Drop left, so it lands on the grid
	/// whatever the Gap (tuning).
	uint8_t stageFraction[4] = {2, 4, 8, 16};
};

/// What still plays in a Section by its keeps (spec §9.5), from ArcSpec::keepsIn().
struct Keeps {
	const PartsAndLanes* parts = nullptr; ///< The Break or Build keeps, or nullptr: everything plays.
	Lane rollLane = Lane::None;           ///< In a Build, the roll lane, which always plays; otherwise None.
	bool riser = true;                    ///< Whether the Riser lane plays: false only in a Build without one.

	/// Whether a part's notes on `lane` (Lane::None for a pitched part) play. FX lanes are the section rules' own.
	[[nodiscard]] bool lets(int32_t part, Lane lane) const;
};

/// How a genre wants to be entered in a transition (spec §9.5, §9.7): the style the confirm highlights.
enum class TransitionEntry : uint8_t {
	Drop,  ///< BUILD INTO A DROP: the default.
	Intro, ///< FADE THROUGH: for a genre that never drops (Ambient).
};

struct ArcSpec {
	/// Intro and Outro lengths. Density's Intro and Outro curves run between the Intro floor and the centre, not from
	/// the offsets below (spec §9.4).
	uint8_t introPhrases = 1;
	uint8_t outroPhrases = 1;
	/// The Groove's base length `b`, drawn per Groove, in phrases at Motion 25. Motion paces it (spec §9.3).
	Range groovePhrases{4, 8};
	Range breakPhrases{1, 1};
	/// Build lengths in bars (1, 2, 4, 8, 16 or 32), drawn by weight. Each divides the phrase or is a whole number of
	/// phrases, so the Drop lands on a phrase line: a shorter one takes the end of the phrase before it (spec §9.3).
	std::span<const WeightedBars> buildBars = kDefaultBuildBars;
	/// Chance per Groove end of going straight into a Build instead of a Break (spec §9.2), per-mille.
	int16_t grooveToBuild = 0;
	uint8_t dropDecayPhrases = 1;
	int16_t dropLiftEnergy = macroPoints(8);
	int16_t dropLiftDensity = kSameAsEnergy;
	/// Indexed by SectionKind. The Intro's and Outro's Density ends are unused: Density follows the Intro floor there.
	SectionOffsets offsets[kNumSectionKinds] = {
	    {macroPoints(-15), 0},                                                    // Intro: Energy rising from low
	    {},                                                                       // Groove: home
	    {macroPoints(-10), macroPoints(-10), macroPoints(-20), macroPoints(-20)}, // Break: a dip
	    {0, macroPoints(12), 0, macroPoints(10)},                                 // Build: a ramp above the centre
	    {0, macroPoints(-15)},                                                    // Outro: Energy falling
	};
	/// Parts and lanes still playing in a Break (spec §9.5). FX lanes are section rules' own and always play.
	PartsAndLanes breakKeeps = kEverything;
	/// The same for every Build. The roll lane always plays, and the Riser unless buildRiser is false.
	PartsAndLanes buildKeeps = kSameAsBreakKeeps;
	/// Parts and lanes that stop for the Build's last buildCutBars bars (0: no cut).
	PartsAndLanes buildCut = {.lanes = laneBit(Lane::Kick)};
	uint8_t buildCutBars = 0;
	/// The cut the other way round: buildCut holds from the Build's start until its last buildCutBars bars, so what it
	/// holds enters there and leads into the Drop (House's filter Build brings the Bass in a bar early).
	bool buildCutLeads = false;
	/// Whether the Build's Riser plays (spec §9.5): false for a Build that rises by its filters alone.
	bool buildRiser = true;
	GapLength gap = GapLength::None;
	BuildRoll roll;
	/// The lane a Phrase accent hits (spec §9.5), or None: always on a Drop, and on other Groove phrase starts with
	/// this chance, per-mille at Motion 25, scaled by the Phrase accent curve.
	Lane accentLane = Lane::None;
	int16_t accentChance = 0;
	/// The style the transition confirm highlights when this genre is the incoming one (spec §9.5). Only the
	/// highlight: the player may pick any style.
	TransitionEntry transitionEntry = TransitionEntry::Drop;

	/// What still plays in a Section of `kind` (spec §9.5): a Break's or Build's keeps; elsewhere everything.
	[[nodiscard]] constexpr Keeps keepsIn(SectionKind kind) const {
		if (kind == SectionKind::Break) {
			return {&breakKeeps};
		}
		if (kind == SectionKind::Build) {
			return {buildKeeps == kSameAsBreakKeeps ? &breakKeeps : &buildKeeps, roll.lane, buildRiser};
		}
		return {};
	}
};

// ---- Fills (slot 6, spec §8.7) ----

/// What a fill does.
enum class FillKind : uint8_t {
	Rest, ///< the masked lanes and parts play nothing for its length
	Swap, ///< the masked lanes play a fill pattern from their fill pool instead of their normal one
	Roll, ///< the masked lane plays a short Build-style ramp, as the Build roll does (spec §8.7)
};

/// One of a genre's fill entries.
struct FillEntry {
	FillKind kind = FillKind::Rest;
	/// A Rest: Drums lanes and whole Bass, Chords, Pad or Lead parts. A Swap: Drums lanes with a fill pool. A Roll: one
	/// Drums lane. Never an FX lane: section rules own them.
	PartsAndLanes mask;
	/// Its length, 1, 2 or 4 beats, ending on the phrase line. A held fill ignores it.
	uint8_t beats = 1;
	uint8_t weight = 1;
};

/// A section-end chance a genre leaves unset: it then uses its fill chance (spec §8.7).
constexpr int16_t kSameAsFillChance = -1;

// ---- Drop-outs (slot 6, spec §8.8) ----

/// One of a genre's drop-out entries: whole parts, Drums lanes or FX lanes falling silent for a few bars in a Groove.
struct DropOutEntry {
	/// Whole Bass, Chords, Pad or Lead parts, and Drums or FX lanes listed in the roster.
	PartsAndLanes mask;
	/// Its length, 1, 2, 4 or 8 bars, from a bar line.
	uint8_t bars = 1;
	uint8_t weight = 1;
	/// The pitched part or Drums lane in the mask whose last hits before it spike Space, held while it lasts (spec
	/// §11.4), or none. Never an FX lane.
	PartsAndLanes throwTarget;
};

// ---- The Build roll (spec §8.3) ----

/// A roll's base velocity climbs from the first to the second over the Build, to the Gap (tuning).
constexpr uint8_t kRollVelocity[2] = {56, 120};
/// How early the roll starts: with this share of the time before the Drop left at Complexity 0, per-mille, and from the
/// Build's first step at Complexity 50, linear between (tuning).
constexpr int32_t kRollStartAtZero = 250;

// ---- Velocity (spec §8.4) ----

/// Base velocity per level, Generator-wide (tuning).
constexpr uint8_t kLevelVelocity[3] = {48, 88, 112};
/// How far Energy's velocity layer moves a note at weight 1000, from Energy 0 to 50 (tuning).
constexpr int32_t kEnergyVelocityRange = 40;

// ---- Ornaments (spec §8.2) ----

/// A ratchet's spacing in ticks, by hit count 2, 3, 4: 2, 3 or 4 evenly spaced hits inside the 16th.
constexpr uint8_t kRatchetSpacing[3] = {12, 8, 6};
/// How much softer each later ratchet hit plays than the one before (tuning).
constexpr int32_t kRatchetSoftening = 8;
/// A flam's grace hit plays at this share of its main hit's velocity, per-mille (tuning).
constexpr int32_t kFlamGraceVelocity = 600;

// ---- Sound controls: the macro layer (spec §11.3) ----

/// The macros with a row in the macro layer's sign table: Energy, Density and Complexity, in Macro order. Motion has
/// no static offset: it paces drift (spec §11.5).
constexpr int32_t kMacroLayerRows = 3;
/// The Generator-wide sign table: how each macro moves each control, +1, -1 or 0 (spec §11.3). Thin is left free for
/// Builds.
constexpr int8_t kMacroLayerSigns[kMacroLayerRows][kNumSoundControls] = {
    // Brightness, Decay, Drive, Space (drier at the peak), Wobble, Thin
    {+1, -1, +1, -1, 0, 0}, // Energy
    {0, 0, 0, 0, 0, 0},     // Density
    {0, 0, 0, 0, 0, 0},     // Complexity
};
/// How far the macro layer moves a control at weight 1000, in control units, when its macro sits kMacroLayerSpan from
/// the genre's default (tuning): the whole range, so a Break's dip is clearly heard.
constexpr int32_t kMacroLayerDepth = kControlFull;
constexpr int32_t kMacroLayerSpan = macroPoints(25);
/// Whether any macro moves `control` (a column of the sign table with an entry). A genre's weights can't add one.
constexpr bool macroMoves(SoundControl control) {
	for (const auto& row : kMacroLayerSigns) {
		if (row[static_cast<int32_t>(control)] != 0) {
			return true;
		}
	}
	return false;
}

// ---- Sound controls: gestures and drift (spec §11.4, §11.5) ----

/// How far each gesture moves its controls at its peak at depth ×1, in control units (tuning).
constexpr int32_t kCloseBrightness = -50;
constexpr int32_t kWashSpace = 50;
constexpr int32_t kRiseThin = 100;
constexpr int32_t kRiseBrightness = 40;
constexpr int32_t kRiseRiserSpace = 50; ///< A Riser lane's Space, on top (spec §11.4).
constexpr int32_t kThrowSpace = 100;
/// Close and Wash ease in over 1 bar, and a Throw decays over 1 beat (spec §11.4).
constexpr int32_t kGestureEaseSteps = kStepsPerBar;
constexpr int32_t kThrowDecaySteps = kStepsPerBeat;
/// A Throw spikes on a part's hits this many steps before its line at most (tuning). The Generator decides one step
/// at a time, so it can't know which hit will be the last (spec §11.4).
constexpr int32_t kThrowWindowSteps = kStepsPerBeat;
/// Whether a step `stepsBefore` steps before a Throw's line is in its window (kThrowWindowSteps).
constexpr bool inThrowWindow(int32_t stepsBefore) {
	return stepsBefore >= 1 && stepsBefore <= kThrowWindowSteps;
}
/// Whether a gesture ever moves `control`: Close and Rise move Brightness, Wash, Rise on a Riser and Throw Space, and
/// Rise Thin.
constexpr bool gestureMoves(SoundControl control) {
	return control == SoundControl::Brightness || control == SoundControl::Space || control == SoundControl::Thin;
}

/// Drift's depth at weight 1000 and Motion 50, in control units: about half each control's range (spec §11.5,
/// tuning). It scales with Motion, so Motion 0 stops it.
constexpr int32_t kDriftDepth = 50;
/// Steps between the walk's seeded points at Motion 25: a fixed 8 bars, one phrase in every built-in genre but
/// Psytrance, whose 16-bar phrase passes two (spec §11.5's "about one phrase"). Twice as often at Motion 50, never at
/// 0. A constant, so the step's walk divides by a constant.
constexpr int32_t kDriftPointSteps = 8 * kStepsPerBar;
/// The walk's points apart in Motion units: each step moves it on by Motion, so Motion 25 passes one a phrase.
constexpr int32_t kDriftApart = kDriftPointSteps * macroPoints(25);
/// Brightness, Decay, Space and Wobble drift; Drive and Thin never do (spec §11.5).
constexpr bool drifts(SoundControl control) {
	return control != SoundControl::Drive && control != SoundControl::Thin;
}

// ---- The genre ----

struct Facts {
	int16_t tempoLowBpm = 120; ///< Rolls come from here; not a clamp.
	int16_t tempoHighBpm = 130;
	std::span<const Scale> scales;
	/// The song swing amount a new Recipe rolls from, swingLow..swingHigh inclusive, in -kMaxSwing..kMaxSwing (spec
	/// §3.2, ADR 0008). Like tempo, a range rolled from fresh entropy; straight (0..0) by default.
	int8_t swingLow = 0;
	int8_t swingHigh = 0;
	/// What a Recipe file that leaves out swing plays (spec §16.2): the range's middle.
	[[nodiscard]] constexpr int8_t swingMiddle() const { return static_cast<int8_t>((swingLow + swingHigh) / 2); }
};

// ---- Variant sets (slot 11, spec §3.6, §4.2, ADR 0008) ----

struct ResolvedGenre;

/// When a playing variant set may change its variant on its own (spec §3.6). Only Never is built: Drop is #495's.
enum class ChangeAt : uint8_t { Never, Drop };

/// One way a variant set's rule can go: a patch to a copy of the genre (core/variants.h).
struct Variant {
	const char* name = "";     ///< Stable, lowercase, for Recipe files: "plain". Never reused within its set.
	const char* oledName = ""; ///< At most kMaxVariantOledName characters: "PLAIN".
	uint16_t weight = 1;       ///< Its share of a draw, against its set's other variants. More than 0.
	/// Writes the variant's fields into a copy of the genre. nullptr: the genre as written.
	void (*apply)(ResolvedGenre&) = nullptr;
};

/// A rule a piece draws one variant of, as it rolls key and tempo (spec §3.6): Dub's 7th or plain skank.
struct VariantSet {
	const char* name = "";             ///< Stable, for Recipe files: "skank". Never reused within its genre.
	const char* oledName = "";         ///< At most kMaxVariantSetOledName characters: "SKANK".
	std::span<const Variant> variants; ///< 2 to kMaxVariants. The first is the genre as it played before the set.
	/// Perform's VARIANTS menu may switch it, at the next Drop (spec §13.6). Its patches keep to what a Drop reads
	/// afresh (spec §3.6).
	bool live = false;
	/// Drop: it may change at each Drop, by a keyed draw (spec §3.6), within its allowed variants. Its patches keep to
	/// what a Drop reads afresh, as a live set's do.
	ChangeAt changeAt = ChangeAt::Never;
	int16_t changeChance = 0; ///< Per mille at each Drop, 1 to 1000, for ChangeAt::Drop; 0 for Never.
};

/// The OLED names fit a menu row (ADR 0008): a set's name, and a variant's, in characters.
constexpr int32_t kMaxVariantSetOledName = 8;
constexpr int32_t kMaxVariantOledName = 9;
/// A variant's stable name, in characters: a Recipe file's `allow` list holds every variant's (recipe_file.cpp).
constexpr int32_t kMaxVariantNameChars = 23;

struct Genre {
	// Slot 1: identity (spec §4.4)
	const char* name = ""; ///< Stable, lowercase, used in Recipe files. Never reused. A subgenre's: `<parent>-<own>`.
	const char* oledName = "";  ///< The full name. A subgenre's names its parent too (`ACID TECHNO`).
	const char* shortName = ""; ///< A subgenre's row under its parent in the GENRE menu (`ACID`, spec §4.4).
	uint16_t version = 1;       ///< Goes up whenever a retune changes the event stream. A parent's is 0: it has none.
	/// The parent genre a subgenre is written as overrides of (spec §4.1): a table that is never played and has no
	/// parent of its own. nullptr for a genre that has none, and for a parent.
	const Genre* parent = nullptr;
	// Slot 2: musical facts
	Facts facts;
	// Slot 3: macros. Weights run 0..2000 per-mille and never flip a sign.
	Macros defaults;
	int16_t energyVelocityWeight = 1000;
	/// The macro layer's weight on each sign table entry, per-mille 0..2000 (spec §11.3). Rows as kMacroLayerSigns.
	int16_t soundWeight[kMacroLayerRows][kNumSoundControls] = {
	    {1000, 1000, 1000, 1000, 1000, 1000},
	    {1000, 1000, 1000, 1000, 1000, 1000},
	    {1000, 1000, 1000, 1000, 1000, 1000},
	};
	/// How long a Throw holds while its part is silent, from the hit that spiked it, before it decays (spec §11.4).
	int16_t throwHoldSteps = kStepsPerBar;
	// Slot 4: meter and phrase
	uint8_t phraseBars = 8;
	// Slots 5, 6: parts, rhythm
	std::span<const PartSpec> parts;
	std::span<const ReplacePair> replacePairs = kDefaultReplacePairs;
	DensityCurve densityCurve = kDefaultDensityCurve;
	/// The jitter added to non-anchor step importances, in importance hundredths (spec §8.1, tuning). Below 100, so
	/// Density 0 plays only anchors.
	int16_t densityJitter = 60;
	/// Chance, per-mille at Motion 25, that a Drums lane or patterned part re-picks at a phrase line in a Groove. The
	/// pattern re-pick Motion curve scales it (spec §8.1, §3.5, tuning).
	int16_t repickChance = 150;
	/// Fill entries (spec §8.7). Empty: no automatic fills, and FILL does nothing.
	std::span<const FillEntry> fills;
	/// The entry a plain FILL press plays, by its index in `fills`.
	uint8_t heldFill = 0;
	/// Chance, per-mille at Motion 25, of an automatic fill at the end of a Groove phrase or the Intro's last phrase.
	/// The fill Motion curve scales it (spec §3.5, tuning).
	int16_t fillChance = 0;
	/// The chance instead for a Groove's last phrase before a Break or Build, scaled the same way.
	int16_t sectionEndChance = kSameAsFillChance;
	/// Drop-out entries (spec §8.8). Empty: none plays.
	std::span<const DropOutEntry> dropOuts;
	/// Chance, per-mille at Motion 25, that a drop-out starts on the next bar line, drawn at each bar line of a Groove
	/// where one may. The drop-out Motion curve scales it (spec §3.5, tuning).
	int16_t dropOutChance = 0;
	/// Bars after a drop-out ends before the next may start, at least 1.
	uint8_t dropOutGapBars = 1;
	/// Bar grid drop-outs start on within a phrase (spec §8.8): a power of two dividing `phraseBars`, default 1 (any
	/// bar line). Drawn a bar ahead, so only on bar lines where `(barInPhrase + 1) % dropOutGridBars == 0` (8 in a
	/// 16-bar phrase draws on bars 7 and 15, landing on half-phrase and phrase lines).
	uint8_t dropOutGridBars = 1;
	// Slot 7: harmony
	HarmonySpec harmony;
	// Slot 8: the built-in palette (spec §10.1)
	const Palette* palette = nullptr;
	// Slot 9: section rules
	ArcSpec arc;
	// Slot 11: variant sets (spec §3.6), at most kMaxVariantSets. None: every piece plays the table as written.
	std::span<const VariantSet> variantSets;

	[[nodiscard]] constexpr int32_t phraseSteps() const { return phraseBars * kStepsPerBar; }
};

/// Whether a period of `bars` bars keeps step with phrases of `phraseBars`: it divides the phrase or is a whole
/// multiple of it, so it never straddles a phrase line. A harmonic rhythm, a Sustain's retrigger, a Sweep and a Build
/// length follow it (spec §5.2, §7.4, §9.3, §11.5).
constexpr bool fitsPhrase(int32_t bars, int32_t phraseBars) {
	return bars > 0 && phraseBars > 0 && (phraseBars % bars == 0 || bars % phraseBars == 0);
}

/// Whether the genre's Chord graph is one the walk can play (spec §7.4): a harmonic rhythm in whole bars that fits the
/// phrase (fitsPhrase()), so a chord never straddles a phrase line; at most kMaxChordMoves moves, each between two
/// different degrees of the 7; and a cadence target among them, or none. A loop (`loopChance` 0 or more) needs 2 whole
/// chords in half a phrase. An empty slot is valid. Its Extension order tries each Extension once, the 6 after the 7
/// (extensionOrderValid()).
constexpr bool harmonyValid(const Genre& genre) {
	if (!extensionOrderValid(genre.harmony.extensionOrder)) {
		return false;
	}
	const ChordGraph* graph = genre.harmony.graph;
	if (graph == nullptr) {
		return true;
	}
	if (!fitsPhrase(graph->barsPerChord, genre.phraseBars)) {
		return false;
	}
	// A loop half a phrase long holds 2 whole chords or more, and every chance is per-mille.
	if (graph->loopChance >= 0
	    && (graph->loopChance > 1000 || graph->halfLoopChance < 0 || graph->halfLoopChance > 1000
	        || graph->returnChance < 0 || graph->returnChance > 1000 || graph->barsPerChord == 0
	        || genre.phraseBars % (4 * graph->barsPerChord) != 0)) {
		return false;
	}
	if (static_cast<int32_t>(graph->moves.size()) > kMaxChordMoves) {
		return false;
	}
	for (const ChordMove& move : graph->moves) {
		if (move.from >= kDegreesPerScale || move.to >= kDegreesPerScale || move.from == move.to) {
			return false;
		}
	}
	return graph->cadenceTarget == kNoCadenceTarget || graph->cadenceTarget < kDegreesPerScale;
}

namespace genres {
/// Acid techno (spec §4.5), the default genre: a mutating acid Loop over Techno's drums, and a pedal Drone under it.
extern const Genre technoAcid;
/// Melodic techno (spec §4.5): the Chord graph walked under a rolling bass, a Sustain Pad and an Arp lead.
extern const Genre technoMelodic;
/// Dub techno (spec §4.5): one m7 or m9 chord, stabbed on the & over a sub bass and a pedal Pad.
extern const Genre technoDub;
/// Detroit techno (spec §4.5): a Chord graph walked under a root-rhythm bass, Strings, a Stab and a Motif lead.
extern const Genre technoDetroit;
/// Ambient (spec §4.5): pads and a drone over a slow Chord graph, a held sub, sparse bells and soft percussion, an Arc
/// that swells and recedes and never drops.
extern const Genre ambient;
/// Classic psytrance (spec §4.5), the Psytrance parent's only subgenre: a four-on-the-floor kick under a rolling 16th
/// bass that Avoids it, a pedal Drone, a swept Lead and a 16th Arp, in 16-bar phrases with long Builds into hard Drops.
extern const Genre psytranceClassic;
/// Dub (spec §4.5): roots dub, not Dub techno. One-drop drums, an offbeat skank, a bass that carries the tune and a
/// melodica, with drop-outs thrown into dotted-8th echoes and a Siren on Drops.
extern const Genre dub;
/// House (spec §4.5): four on the floor with claps on 2 and 4, an offbeat bass, 7th stabs and Strings over a walking
/// Chord graph, swung hats leaning late, and a Lead that Responds to the bass in each bar's second half.
extern const Genre house;
/// The Techno parent's: its drum and FX lanes only, which every subgenre's palette shares.
extern const Palette technoPalette;
extern const Palette technoAcidPalette;
extern const Palette technoMelodicPalette;
extern const Palette technoDubPalette;
extern const Palette technoDetroitPalette;
extern const Palette ambientPalette;
extern const Palette psytrancePalette;
extern const Palette dubPalette;
extern const Palette housePalette;
} // namespace genres

/// Whether a Techno subgenre's roster has what the Techno parent's tables assume (spec §4.5, #218): Drums first and FX
/// last (the parent's), with the bass second, the part the parent's Rest fill drops.
constexpr bool technoRosterValid(const Genre& genre) {
	return genre.parts.size() >= 3 && genre.parts.front().role == Role::Drums && genre.parts[1].role == Role::Bass
	       && genre.parts.back().role == Role::FX;
}

/// The most built-in genres: v1's eight, Techno's four subgenres among them (spec §4.5, ADR 0006). Parents don't count:
/// they are never played.
constexpr int32_t kMaxGenres = 8;

/// A subgenre of `parent` before its overrides (spec §4.1): every field the parent's, but its identity, which the
/// subgenre names itself. A genre file writes one as
///
///     constexpr Genre kAcid = [] {
///         Genre genre = subgenreOf(kTechnoParent);
///         genre.name = "techno-acid";
///         ...  // its names, then only what it changes
///         return genre;
///     }();
///
/// `parent` must be a constexpr table with static storage, in the same file, and not a subgenre itself: a subgenre of
/// a subgenre fails to compile.
consteval Genre subgenreOf(const Genre& parent) {
	if (parent.parent != nullptr) {
		throw "a subgenre's parent is a genre with no parent of its own (spec §4.1)";
	}
	Genre genre = parent;
	genre.name = "";
	genre.oledName = "";
	genre.shortName = "";
	genre.version = 1;
	genre.parent = &parent;
	return genre;
}

namespace detail {

/// Whether `rule`, listed on part `part` (and on `lane` of it, or Lane::None for the part itself), reads something
/// generated before it.
constexpr bool targetsEarlier(const Genre& genre, int32_t part, Lane lane, const InterplayRule& rule) {
	const RuleTarget& target = rule.target;
	if (target.part >= genre.parts.size() || target.part > part) {
		return false;
	}
	const PartSpec& targetSpec = genre.parts[target.part];
	if (target.lane != Lane::None && laneIndexOf(targetSpec, target.lane) < 0) {
		return false;
	}
	if (target.part < part) {
		return true;
	}
	// Within one part, only a lane may read an earlier lane, in the fixed lane order (spec §5.4).
	return lane != Lane::None && target.lane != Lane::None
	       && static_cast<int32_t>(target.lane) < static_cast<int32_t>(lane);
}

/// Whether `rules` holds a rule of `kind`.
constexpr bool hasRule(std::span<const InterplayRule> rules, RuleKind kind) {
	for (const InterplayRule& rule : rules) {
		if (rule.kind == kind) {
			return true;
		}
	}
	return false;
}

constexpr bool targetResponds(const Genre& genre, const RuleTarget& target);

/// Whether any of `rules`, listed on part `part` (and on `lane` of it), reads, at any depth, something that lists a
/// Respond rule. A rule that doesn't read something earlier is skipped (interplayRulesValid() rejects it on its own),
/// so the walk always goes back through the roster and ends. It keeps no memo, so a target reached by several paths is
/// walked again on each: rosters are at most kMaxParts parts and their rules a handful, and it runs at compile time
/// (genreValid()) or once at load.
constexpr bool readsRespond(const Genre& genre, int32_t part, Lane lane, std::span<const InterplayRule> rules) {
	for (const InterplayRule& rule : rules) {
		if (targetsEarlier(genre, part, lane, rule) && targetResponds(genre, rule.target)) {
			return true;
		}
	}
	return false;
}

/// Whether what `target` names (the part, the lane, or for a Kit part read whole any lane) lists a Respond rule, or
/// reads through its own rules, at any depth, something that does: its plan in a half-bar would then depend on a
/// Respond count that isn't taken yet.
constexpr bool targetResponds(const Genre& genre, const RuleTarget& target) {
	const PartSpec& part = genre.parts[target.part];
	if (target.lane == Lane::None
	    && (hasRule(part.rules, RuleKind::Respond) || readsRespond(genre, target.part, Lane::None, part.rules))) {
		return true;
	}
	for (const LaneSpec& lane : part.lanes) {
		if ((target.lane == Lane::None || lane.lane == target.lane)
		    && (hasRule(lane.rules, RuleKind::Respond) || readsRespond(genre, target.part, lane.lane, lane.rules))) {
			return true;
		}
	}
	return false;
}

/// Whether `rule`, listed on part `part` (and on `lane` of it), reads something generated before it, and, for
/// Respond, something that neither responds nor reads a Respond source at any depth.
constexpr bool ruleValid(const Genre& genre, int32_t part, Lane lane, const InterplayRule& rule) {
	return targetsEarlier(genre, part, lane, rule)
	       && (rule.kind != RuleKind::Respond || !targetResponds(genre, rule.target));
}

} // namespace detail

/// Whether every interplay rule names a part or lane that exists and is generated earlier: an earlier part in the
/// roster, or an earlier lane of the same part in the fixed lane order (spec §5.5). A rule that doesn't would make the
/// Generator's planned() recurse forever, so every built-in genre `static_assert`s it where it is defined. And no
/// Respond rule reads a part or lane that lists one or reads one through its own rules, at any depth: counting the
/// target's halves at a half-bar's first step reads its plan in the other half too, whose own Respond count isn't
/// taken yet.
constexpr bool interplayRulesValid(const Genre& genre) {
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()); p++) {
		const PartSpec& part = genre.parts[p];
		if (!part.lanes.empty() && !part.rules.empty()) {
			return false; // a Kit part plays only its lanes' rules
		}
		for (const InterplayRule& rule : part.rules) {
			if (!detail::ruleValid(genre, p, Lane::None, rule)) {
				return false;
			}
		}
		for (const LaneSpec& lane : part.lanes) {
			for (const InterplayRule& rule : lane.rules) {
				if (!detail::ruleValid(genre, p, lane.lane, rule)) {
					return false;
				}
			}
		}
	}
	return true;
}

namespace detail {

/// Bit s: one of `pool`'s patterns has an onset (with `anchors`, an anchor) on step s of a bar, at any tier. A pattern
/// that divides the bar (1, 2, 4 or 8 steps) repeats across it; one of whole bars folds onto it. Any other length
/// drifts across the bar, so its hits count as every step.
constexpr uint32_t barSteps(std::span<const Pattern> pool, bool anchors) {
	constexpr uint32_t kWholeBar = (1U << kStepsPerBar) - 1;
	uint32_t bits = 0;
	for (const Pattern& pattern : pool) {
		const int32_t length = pattern.length;
		const bool fits = length > 0 && (kStepsPerBar % length == 0 || length % kStepsPerBar == 0);
		for (int32_t s = 0; s < length; s++) {
			const bool hit = anchors ? pattern.importance[s] == kAnchor : pattern.importance[s] > 0;
			if (!hit) {
				continue;
			}
			if (!fits) {
				bits |= kWholeBar;
				continue;
			}
			for (int32_t b = s % kStepsPerBar; b < kStepsPerBar; b += length) {
				bits |= 1U << b;
			}
		}
	}
	return bits;
}

/// Bit s: `target` can have an onset on step s of a bar: a lane's pool, a pitched part's, or every lane of a Kit.
constexpr uint32_t targetBarSteps(const Genre& genre, const RuleTarget& target) {
	if (target.part >= genre.parts.size()) {
		return 0; // interplayRulesValid() rejects it
	}
	const PartSpec& part = genre.parts[target.part];
	if (part.lanes.empty()) {
		return barSteps(part.pool, false);
	}
	uint32_t bits = 0;
	for (const LaneSpec& lane : part.lanes) {
		bits |= target.lane == Lane::None || lane.lane == target.lane ? barSteps(lane.pool, false) : 0;
	}
	return bits;
}

/// Whether a source's Avoid rules can do what they say (avoidRulesValid()): `sustain` for a Sustain part, `flam` its
/// Ornaments' flam weight.
constexpr bool avoidSourceValid(const Genre& genre, std::span<const Pattern> pool, std::span<const InterplayRule> rules,
                                bool sustain, int32_t flam) {
	const uint32_t anchors = barSteps(pool, true);
	for (const InterplayRule& rule : rules) {
		if (rule.kind == RuleKind::Avoid
		    && (sustain || flam > 0 || (anchors & targetBarSteps(genre, rule.target)) != 0)) {
			return false;
		}
	}
	return true;
}
constexpr bool avoidSourceValid(const Genre& genre, const PartSpec& part) {
	return avoidSourceValid(genre, part.pool, part.rules, part.flavour == Flavour::Sustain,
	                        part.expression.ornaments.flam);
}
constexpr bool avoidSourceValid(const Genre& genre, const LaneSpec& lane) {
	return avoidSourceValid(genre, lane.pool, lane.rules, false, lane.expression.ornaments.flam);
}

} // namespace detail

/// Whether every Avoid rule can do what it says (spec §5.5, §8.1). No anchor in any of the source's pool patterns falls
/// on a step where any of the target's pool patterns has an onset, at any tier, comparing patterns of different
/// lengths by their step in the bar: an anchor always plays, so it would play where the rule must leave a hole. No
/// Sustain part lists one: it has no onsets to thin, and Generator::avoided() would cut its voicing on every target
/// step. And no source flams: a flam's grace hit on the step after an avoided one starts inside the avoided 16th.
constexpr bool avoidRulesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (!detail::avoidSourceValid(genre, part)) {
			return false;
		}
		for (const LaneSpec& lane : part.lanes) {
			if (!detail::avoidSourceValid(genre, lane)) {
				return false;
			}
		}
	}
	return true;
}

/// Whether each Roll fill entry names exactly one lane and no whole part, and that lane is a non-FX lane of a Drums
/// part in the roster (spec §8.7): a Roll plays on one Drums lane.
constexpr bool rollFillsValid(const Genre& genre) {
	for (const FillEntry& fill : genre.fills) {
		if (fill.kind != FillKind::Roll) {
			continue;
		}
		if (fill.mask.parts != 0 || std::popcount(fill.mask.lanes) != 1) {
			return false;
		}
		bool found = false;
		for (const PartSpec& part : genre.parts) {
			for (const LaneSpec& lane : part.lanes) {
				found = found
				        || (part.role == Role::Drums && !isFxLane(lane.lane) && fill.mask.lanes == laneBit(lane.lane));
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

namespace detail {

constexpr bool mayRatchet(const Ornaments& ornaments) {
	return ornaments.ratchet[0] > 0 || ornaments.ratchet[1] > 0 || ornaments.ratchet[2] > 0;
}

/// The Lean on pattern step `step` (any sign), clamped to ±12 as the Generator clamps it.
constexpr int32_t leanOn(const Expression& expression, int32_t step) {
	return std::clamp<int32_t>(expression.lean[((step % kStepsPerBeat) + kStepsPerBeat) % kStepsPerBeat], -kMaxLean,
	                           kMaxLean);
}

/// The latest a note on `step` starts, in ticks from its grid line: its Lean, moved later to the line where a cut may
/// fall on it (`onCutLine`: a Section's first step, or the phrase line a new sound or key change lands on) and on every
/// 16th when the part may ratchet (the last hit of a ratchet just before may start a tick before the line), and to a
/// tick after the 16th before when that one leans past it (hits on one key never cross) (spec §8.5).
constexpr int32_t latestStart(const Expression& expression, int32_t step, bool onCutLine) {
	int32_t start = std::max<int32_t>(leanOn(expression, step), leanOn(expression, step - 1) - kTicksPerStep + 1);
	return onCutLine || mayRatchet(expression.ornaments) ? std::max<int32_t>(start, 0) : start;
}

/// A ratchet's last hit starts by the line after its 16th, however it leans, and lasts its spacing (Ratchet2 leaned +11
/// ends at +35): it ends before the onset two 16ths on starts at its earliest, so the Loop rule's (k, k+2) pairs hold
/// a ratcheting part's notes too (spec §8.2, §8.5).
static_assert(kTicksPerStep - 1 + std::ranges::max(kRatchetSpacing) <= 2 * kTicksPerStep - kMaxLean,
              "a ratchet's last hit ends before the onset two 16ths on starts");

/// The earliest a note on `step` starts, in ticks from its grid line: its Lean, its flam gap earlier when the part may
/// flam, never earlier than −12 (spec §8.2, §8.5).
constexpr int32_t earliestStart(const Expression& expression, int32_t step) {
	const Ornaments& ornaments = expression.ornaments;
	return std::max<int32_t>(leanOn(expression, step) - (ornaments.flam > 0 ? ornaments.flamGap : 0), -kMaxLean);
}

/// Whether `holds(pattern, step)` for every onset (before thinning) of every pattern in `part`'s pool: the walk each
/// note-bound check makes.
template <typename Holds>
constexpr bool everyOnset(const PartSpec& part, Holds holds) {
	for (const Pattern& pattern : part.pool) {
		for (int32_t i = 0; i < pattern.length; i++) {
			if (pattern.importance[i] != 0 && !holds(pattern, i)) {
				return false;
			}
		}
	}
	return true;
}

/// The first step after `step` that `pattern` plays at any Density (before thinning), or its length if none does.
constexpr int32_t nextOnset(const Pattern& pattern, int32_t step) {
	int32_t next = step + 1;
	while (next < pattern.length && pattern.importance[next] == 0) {
		next++;
	}
	return next;
}

/// Whether a Loop part's plain notes, lasting its gate, each end by the time the pattern's next onset starts, before
/// thinning: from the note's latest start to that onset's earliest, the pattern's end counting as an onset, since
/// the next pick starts there (#221). So notes longer than a step (a sub bass's) play one at a time and end by the
/// pattern's end; only a pick thaw() adopts mid-pattern can start under one. A part that may ratchet fails: its hits
/// fall within a step.
constexpr bool longLoopNotesFit(const PartSpec& part) {
	if (mayRatchet(part.expression.ornaments)) {
		return false;
	}
	return everyOnset(part, [&](const Pattern& pattern, int32_t i) {
		const int32_t next = nextOnset(pattern, i);
		const int32_t end = latestStart(part.expression, i, i % kStepsPerBar == 0) + part.loop.gateTicks;
		return end <= (next - i) * kTicksPerStep + earliestStart(part.expression, next);
	});
}

} // namespace detail

/// How long a Stab, Arp or Motif note on step `patternStep` of `pattern` lasts, in ticks (spec §5.2): `gate`, but no
/// longer than to the pattern's next onset (before thinning, so the length never depends on Density) or the next bar
/// line, where the chord may change. `stepInBar` is the step's place in its bar. A note leaning `lean` ticks late
/// still ends by the bar line, so no note lasts past one.
constexpr int32_t onsetTicks(const Pattern& pattern, int32_t patternStep, int32_t stepInBar, int32_t gate,
                             int32_t lean) {
	int32_t untilNext = 1;
	const int32_t toBarLine = kStepsPerBar - stepInBar;
	while (untilNext < toBarLine && pattern.importance[(patternStep + untilNext) % pattern.length] == 0) {
		untilNext++;
	}
	const int32_t cap = untilNext * kTicksPerStep - (untilNext == toBarLine ? std::max<int32_t>(lean, 0) : 0);
	return std::min<int32_t>(gate, cap);
}

namespace detail {

/// Whether a part whose notes last onsetTicks() with `gate` keeps the note bound over each pattern's own onset gaps:
/// every note, from its latest start, ends by the time the onset after its next one starts, at its earliest. The onset
/// after the next is the pattern's, or past the bar line at the earliest a pattern could put one there: on the line
/// when the next onset is before it, a step after when it isn't (another pick may start on the line).
///
/// The length rule keeps the bound by construction: a note ends by its next grid onset or the bar line, its Lean
/// counted, and Lean moves a start 12 ticks at most. This walks the same rule, so a change to it that breaks the bound
/// (an overlap, a cap past the next onset) fails here first.
constexpr bool onsetNotesKeepBound(const PartSpec& part, int32_t gate) {
	return everyOnset(part, [&](const Pattern& pattern, int32_t i) {
		// No onset after it in the pattern reads as its length, never before the bar line.
		const int32_t line = (i / kStepsPerBar + 1) * kStepsPerBar;
		const int32_t next = nextOnset(pattern, i);
		const int32_t afterNext = next < line ? std::min<int32_t>(nextOnset(pattern, next), line) : line + 1;
		const int32_t length = onsetTicks(pattern, i, i % kStepsPerBar, gate, leanOn(part.expression, i));
		const int32_t end = latestStart(part.expression, i, i % kStepsPerBar == 0) + length;
		return end <= (afterNext - i) * kTicksPerStep + earliestStart(part.expression, afterNext);
	});
}

} // namespace detail

/// The note bound every Bass and Lead part keeps, so its table of two notes holds it (spec §2.7, §5.2): every note ends
/// by the time the onset after its next one starts, counting Lean, the clamps and a flam's grace hit.
///
/// A Loop part's plain note lasts its gate: a step at most, or longer when every note ends before the pattern's next
/// onset (detail::longLoopNotesFit(), #221: a sub bass's long notes). A slid note lasts a step plus the slide overlap
/// from its latest start (detail::latestStart(), the clamp counted on every beat's 1), and the onset two 16ths on
/// starts at its earliest (detail::earliestStart()) (spec §8.5, §8.6). A part that ties (LoopSpec::tieChance, a
/// per-mille) plays no Ornaments.
constexpr bool loopNotesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.flavour != Flavour::Loop) {
			if (part.loop.rootOnAnchors) {
				return false;
			}
			continue;
		}
		if (part.loop.gateTicks > kTicksPerStep && !detail::longLoopNotesFit(part)) {
			return false;
		}
		// A tie strikes nothing, so it can't carry an Ornament (spec §8.6).
		const Ornaments& ornaments = part.expression.ornaments;
		if (part.loop.tieChance > 1000
		    || (part.loop.tieChance != 0 && (detail::mayRatchet(ornaments) || ornaments.flam > 0))) {
			return false;
		}
		if (part.loop.slide.atZero == 0 && part.loop.slide.atFull == 0) {
			continue; // it never slides
		}
		for (int32_t k = 0; k < kStepsPerBeat; k++) {
			const int32_t start = detail::latestStart(part.expression, k, k == 0);
			const int32_t laterStart = detail::earliestStart(part.expression, k + 2);
			if (start + kTicksPerStep + part.loop.slideOverlapTicks > 2 * kTicksPerStep + laterStart) {
				return false;
			}
		}
	}
	return true;
}

/// Whether every Arp part has something to walk and keeps the note bound (spec §5.2, §7.3, §8.1): a pool, a walk of 1
/// to kMaxArpOctaves octaves for each entry, a gate of at least a tick, a Register that holds its widest walk
/// (arpRange()), and the bound over each pattern's own onset gaps (detail::onsetNotesKeepBound()).
constexpr bool arpNotesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.flavour != Flavour::Arp) {
			continue;
		}
		if (part.pool.empty() || part.arp.walks.size() != part.pool.size() || part.arp.gateTicks == 0) {
			return false;
		}
		int32_t octaves = 0;
		for (const ArpWalk& walk : part.arp.walks) {
			if (walk.octaves < 1 || walk.octaves > kMaxArpOctaves) {
				return false;
			}
			octaves = std::max<int32_t>(octaves, walk.octaves);
		}
		const Register reg = part.registerOrDefault();
		if (reg.spanHigh - reg.spanLow < arpRange(octaves) || !detail::onsetNotesKeepBound(part, part.arp.gateTicks)) {
			return false;
		}
	}
	return true;
}

/// Whether every Motif part has something to play and keeps the note bound (spec §5.2, §8.1): a pool, chord positions
/// to draw from, a gate of at least a tick, and the bound over each pattern's own uneven onset gaps
/// (detail::onsetNotesKeepBound()).
constexpr bool motifNotesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.flavour != Flavour::Motif) {
			continue;
		}
		if (part.pool.empty() || part.motif.positions.empty() || part.motif.gateTicks == 0
		    || !detail::onsetNotesKeepBound(part, part.motif.gateTicks)) {
			return false;
		}
	}
	return true;
}

/// Whether every Stab part has something to play (spec §5.2, §8.1): a pool, and a gate of at least a tick. onsetTicks()
/// ends a stab by its pattern's next onset or the bar line; the bound over each pattern's own onset gaps
/// (detail::onsetNotesKeepBound()) guards that rule, as for the Arp and the Motif.
constexpr bool stabNotesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.flavour == Flavour::Stab
		    && (part.pool.empty() || part.stab.gateTicks == 0
		        || !detail::onsetNotesKeepBound(part, part.stab.gateTicks))) {
			return false;
		}
	}
	return true;
}

/// Whether every part that yields (spec §5.6, #497) is a patterned pitched part yielding to another of the genre's
/// parts, which isn't itself yielding: the Generator works out the yields in one pass, after every part's own silence.
/// A part that never yields names no part and sets no mode; a mode of None everywhere is never set.
constexpr bool yieldValid(const Genre& genre) {
	const auto parts = static_cast<int32_t>(genre.parts.size());
	for (const PartSpec& part : genre.parts) {
		const YieldSpec& yield = part.yield;
		const bool modes = yield.inDrops != YieldMode::None || yield.outside != YieldMode::None;
		if (yield.part < 0) {
			if (modes) {
				return false;
			}
			continue;
		}
		if (!modes || yield.part >= parts || &genre.parts[yield.part] == &part || !part.lanes.empty()
		    || part.pool.empty() || genre.parts[yield.part].yield.part >= 0) {
			return false;
		}
	}
	return true;
}

/// Whether every Sweep's period fits the phrase (fitsPhrase()), so it keeps to phrase lines (spec §11.5).
constexpr bool sweepsValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.drift.sweepBars != 0 && !fitsPhrase(part.drift.sweepBars, genre.phraseBars)) {
			return false;
		}
	}
	return true;
}

/// Whether every Build length is 1, 2, 4, 8, 16 or 32 bars and lands its Drop on a phrase line (fitsPhrase(), spec
/// §9.3). Arc::buildSteps() plays a phrase in place of one that doesn't land.
constexpr bool buildBarsValid(const Genre& genre) {
	for (const WeightedBars& build : genre.arc.buildBars) {
		const int32_t bars = build.bars;
		if (bars > 32 || !std::has_single_bit(static_cast<unsigned>(bars)) || !fitsPhrase(bars, genre.phraseBars)) {
			return false;
		}
	}
	return true;
}

/// Whether the Build cut, if any, leaves the roll lane playing (spec §8.3, §9.1): the Arc plan counts a rolling lane as
/// sounding and never reads the cut, so a cut that stopped the roll would plan a part the dim shows silenced.
constexpr bool rollUncut(const Genre& genre) {
	const Lane roll = genre.arc.roll.lane;
	if (genre.arc.buildCutBars == 0) {
		return true;
	}
	for (size_t p = 0; p < genre.parts.size(); p++) {
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			if (lane.lane == roll && genre.arc.buildCut.has(static_cast<int32_t>(p), roll)) {
				return false;
			}
		}
	}
	return true;
}

/// The genre's shortest Build in bars, no longer than its phrase (spec §9.1).
constexpr int32_t shortestBuildBars(const Genre& genre) {
	int32_t shortest = genre.phraseBars;
	for (const WeightedBars& build : genre.arc.buildBars) {
		shortest = std::min<int32_t>(shortest, build.bars);
	}
	return shortest;
}

/// Whether the Build cut and the Gap are each shorter than the genre's shortest Build and its phrase (spec §9.1, §9.5,
/// §13.5): the Arc plan counts a Build's keeps as sounding and leaves the cut and the Gap out, so one that covered a
/// whole Build (Dub's is a bar) or a whole phrase would plan a part as playing where it plays nothing. A leading cut
/// (`buildCutLeads`) covers each Build but its last `buildCutBars` bars: it leaves at least one bar, and its span stays
/// under the phrase in every Build length.
constexpr bool cutAndGapUnderBuild(const Genre& genre) {
	const ArcSpec& arc = genre.arc;
	const int32_t shortest = shortestBuildBars(genre);
	if (gapSteps(arc.gap) >= shortest * kStepsPerBar) {
		return false;
	}
	if (!arc.buildCutLeads) {
		return arc.buildCutBars < shortest;
	}
	if (arc.buildCutBars < 1) {
		return false;
	}
	for (const WeightedBars& build : arc.buildBars) {
		if (build.bars - arc.buildCutBars >= genre.phraseBars) {
			return false;
		}
	}
	return true;
}

/// Whether every Siren lane holds its note 1 bar up to a phrase, on an FX part, and no other lane sets a held length
/// (spec §9.5): a Siren that outlasted its phrase would ring on into the next Section's plan.
constexpr bool sirensValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		for (const LaneSpec& lane : part.lanes) {
			const bool siren = lane.lane == Lane::Siren && part.role == Role::FX;
			if (siren ? lane.heldBars < 1 || lane.heldBars > genre.phraseBars : lane.heldBars != 0) {
				return false;
			}
		}
	}
	return true;
}

/// The lowest and highest notes a pitched part can play unshifted, in any key (spec §7.3). Every flavour plays inside
/// the Register's span around the part's tonic: a Loop, a Motif and a pedal through noteFor()'s fold, a voicing and its
/// doubled root at the nearest place inside it, an Arp from the root's lowest place up, folded down. The tonic is the
/// key root nearest the anchor, the lower on a tie, so it lies from 6 below the anchor to 5 above whatever the key.
struct PitchReach {
	int32_t lowest = 0;
	int32_t highest = 0;
};
constexpr PitchReach pitchReach(const PartSpec& part) {
	const Register reg = part.registerOrDefault();
	return {reg.anchor - 6 + reg.spanLow, reg.anchor + 5 + reg.spanHigh};
}

/// The octave shifts a part takes (spec §3.1, §7.3): those of -kMaxOctaveShift..kMaxOctaveShift that keep its whole
/// reach inside MIDI's 0–127, so no note of it is ever clamped. Only 0 for Drums and FX.
struct OctaveRange {
	int32_t lowest = 0;
	int32_t highest = 0;
	[[nodiscard]] constexpr bool contains(int32_t shift) const { return shift >= lowest && shift <= highest; }
	[[nodiscard]] constexpr int32_t clamp(int32_t shift) const {
		return shift < lowest ? lowest : shift > highest ? highest : shift;
	}
};
constexpr OctaveRange octaveRange(const PartSpec& part) {
	if (!hasRegister(part.role)) {
		return {};
	}
	const PitchReach reach = pitchReach(part);
	OctaveRange range{-kMaxOctaveShift, kMaxOctaveShift};
	while (range.lowest < 0 && reach.lowest + 12 * range.lowest < 0) {
		range.lowest++;
	}
	while (range.highest > 0 && reach.highest + 12 * range.highest > 127) {
		range.highest--;
	}
	return range;
}

/// Whether every pitched part plays inside MIDI's range unshifted, so its octave range holds 0, and its Register anchor
/// ± 12 × kMaxOctaveShift stays a MIDI note, so shiftedRegister()'s byte never wraps (spec §7.3).
constexpr bool registersShiftValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (!hasRegister(part.role)) {
			continue;
		}
		const int32_t anchor = part.registerOrDefault().anchor;
		const PitchReach reach = pitchReach(part);
		// pitchReach() bounds the notes only for a span of at least 11 semitones: a narrower one lets noteFor()'s
		// upward fold land past spanHigh.
		const Register reg = part.registerOrDefault();
		if (reg.spanHigh - reg.spanLow < 11 || anchor - 12 * kMaxOctaveShift < 0 || anchor + 12 * kMaxOctaveShift > 127
		    || reach.lowest < 0 || reach.highest > 127) {
			return false;
		}
	}
	return true;
}

/// Whether every lane's alternate lane (spec §5.7) is valid: chance in 0–1000, 0 when no alternate lane is set;
/// when set, on a Drums part, naming a different non-FX lane of the same part that has no pattern pool of its own,
/// from a lane that has one.
constexpr bool alternateLanesValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		for (const LaneSpec& lane : part.lanes) {
			if (lane.alternateChance < 0 || lane.alternateChance > 1000) {
				return false;
			}
			if (lane.alternateLane == Lane::None) {
				if (lane.alternateChance != 0) {
					return false;
				}
				continue;
			}
			if (part.role != Role::Drums || lane.pool.empty() || lane.alternateLane == lane.lane
			    || isFxLane(lane.alternateLane)) {
				return false;
			}
			const int32_t alt = laneIndexOf(part, lane.alternateLane);
			if (alt < 0 || !part.lanes[alt].pool.empty()) {
				return false;
			}
		}
	}
	return true;
}

/// Whether the genre's drop-out entries play as spec §8.8 says: each 1, 2, 4, 8 or 16 bars long and no longer than the
/// phrase, so it never outlasts the phrase it starts in; its mask some whole pitched parts and Drums or FX lanes listed
/// in the roster (no whole Kit), its throw target one pitched part or Drums lane of the mask or none (never an FX
/// lane), and never every pitched part and every Drums lane with a pool, which would be a Gap; a gap of at least a bar,
/// a bar grid that is a power of two dividing the phrase, and a chance a per-mille.
constexpr bool dropOutsValid(const Genre& genre) {
	if (genre.dropOutGapBars < 1 || genre.dropOutGridBars < 1 || genre.dropOutGridBars > genre.phraseBars
	    || !std::has_single_bit(static_cast<unsigned>(genre.dropOutGridBars))
	    || genre.phraseBars % genre.dropOutGridBars != 0 || genre.dropOutChance < 0 || genre.dropOutChance > 1000) {
		return false;
	}
	for (const DropOutEntry& entry : genre.dropOuts) {
		const PartsAndLanes mask = entry.mask;
		const PartsAndLanes target = entry.throwTarget;
		if (entry.bars > 16 || !std::has_single_bit(static_cast<unsigned>(entry.bars)) || entry.bars > genre.phraseBars
		    || mask.empty()) {
			return false;
		}
		if (!target.without(mask).empty() || std::popcount(target.parts) + std::popcount(target.lanes) > 1) {
			return false;
		}
		bool leavesSome = false;
		uint8_t pitched = 0;
		uint16_t drumLanes = 0;
		uint16_t validLanes = 0;
		for (size_t p = 0; p < genre.parts.size(); p++) {
			const PartSpec& part = genre.parts[p];
			const bool inMask = (mask.parts & partBit(static_cast<int32_t>(p))) != 0;
			if (part.lanes.empty()) {
				pitched |= partBit(static_cast<int32_t>(p));
				leavesSome = leavesSome || !inMask;
				continue;
			}
			for (const LaneSpec& lane : part.lanes) {
				validLanes |= laneBit(lane.lane);
				if (part.role == Role::Drums) {
					drumLanes |= laneBit(lane.lane);
					leavesSome = leavesSome || (!lane.pool.empty() && (mask.lanes & laneBit(lane.lane)) == 0);
				}
			}
		}
		if ((mask.parts & ~pitched) != 0 || (mask.lanes & ~validLanes) != 0 || (target.lanes & ~drumLanes) != 0
		    || !leavesSome) {
			return false;
		}
	}
	return true;
}

/// Whether every Sustain part holds data the flavour plays (spec §5.2): a retrigger period, if any, that fits the
/// phrase (fitsPhrase()), and no anchored Extension or open voicing on a pedal, which voices none (pedalVoicing()).
/// Only a Sustain holds across Section lines or holds common tones: cutSectionLine() leaves the flagged part's notes
/// uncut, and only decideSustain() ends or extends them. Only a Sustain voices open, and doubles no root there
/// (openVoicing()), and only an open voicing has a chance to hold its shared notes, at most 1000 per-mille.
constexpr bool sustainValid(const Genre& genre) {
	for (const PartSpec& part : genre.parts) {
		if (part.flavour != Flavour::Sustain) {
			if (part.sustain.holdsAcrossSections || part.sustain.holdsCommonTones || part.sustain.openVoicing
			    || part.sustain.holdSharedChance != 0) {
				return false;
			}
			continue;
		}
		const SustainSpec& sustain = part.sustain;
		if (sustain.retriggerBars != 0 && !fitsPhrase(sustain.retriggerBars, genre.phraseBars)) {
			return false;
		}
		if (sustain.pedal && sustain.anchorExtension) {
			return false;
		}
		if (sustain.openVoicing && (sustain.pedal || sustain.doubleRoot)) {
			return false;
		}
		if (sustain.holdSharedChance > 1000 || (sustain.holdSharedChance != 0 && !sustain.openVoicing)) {
			return false;
		}
	}
	return true;
}

/// Every built-in genre, in menu order. At most kMaxGenres.
std::span<const Genre* const> allGenres();
/// The genre with this stable name, or nullptr. Parents aren't found: no Recipe names one.
const Genre* findGenre(const char* name);
/// The default genre: the one the GENRE menu opens on before any Recipe exists (spec §4.5, §14.1).
const Genre& defaultGenre();

/// How many of the genre's parts the Generator plays: the roster, up to kMaxParts.
constexpr int32_t partCount(const Genre& genre) {
	return std::min<int32_t>(static_cast<int32_t>(genre.parts.size()), kMaxParts);
}

/// The genre's Drums part, the one the punch-ins act on and Pump leaves out (spec §8.9), or -1 with none.
constexpr int32_t drumsPart(const Genre& genre) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (genre.parts[p].role == Role::Drums) {
			return p;
		}
	}
	return -1;
}

/// What a control set is, which decides the layers that move it (spec §11.3–§11.5).
enum class ControlSetKind : uint8_t {
	Part,    ///< A pitched part's own: every layer.
	KitWide, ///< A Drums part's kit-level controls: the macro layer and gestures, no drift.
	KitLane, ///< A Drums lane's row controls: drift, and gestures aimed at the lane.
	FxLane,  ///< An FX lane's row controls: every layer.
};
constexpr int32_t kNumControlSetKinds = 4;
/// Whether the macro layer moves a set of `kind` (spec §11.3): a Drums lane's row takes none, since the macro layer
/// moves the kit-wide controls.
constexpr bool takesMacroLayer(ControlSetKind kind) {
	return kind != ControlSetKind::KitLane;
}
/// Whether a set of `kind` can ever move `control`: the layers that act on it, and what each layer moves.
constexpr bool setMoves(ControlSetKind kind, SoundControl control) {
	const bool drift = kind != ControlSetKind::KitWide && drifts(control);
	const bool macro = takesMacroLayer(kind) && macroMoves(control);
	return drift || macro || gestureMoves(control);
}
/// How a part of each role lays out its control sets (spec §11.1): a set of its own first, if it has one, then one per
/// lane if it has those. One table for controlSetsOf(), controlSetKind() and controlSetLane().
struct ControlSetLayout {
	bool own = true; ///< A set of its own: a pitched part's, or a kit's kit-wide one.
	ControlSetKind ownKind = ControlSetKind::Part;
	bool perLane = false; ///< A set per lane after it: on Drums, and on FX instead of its own.
	ControlSetKind laneKind = ControlSetKind::Part;
};
/// A pitched part has one set; Drums the kit-wide set, then one per lane; FX one per lane only, since FX lanes always
/// use their own row controls (spec §11.4).
constexpr ControlSetLayout controlSetLayout(Role role) {
	switch (role) {
	case Role::Drums:
		return {.ownKind = ControlSetKind::KitWide, .perLane = true, .laneKind = ControlSetKind::KitLane};
	case Role::FX:
		return {.own = false, .perLane = true, .laneKind = ControlSetKind::FxLane};
	default:
		return {};
	}
}
/// The control sets of a part of `role` with `lanes` lanes.
constexpr int32_t controlSetsOf(Role role, int32_t lanes) {
	const ControlSetLayout layout = controlSetLayout(role);
	return (layout.own ? 1 : 0) + (layout.perLane ? lanes : 0);
}
constexpr int32_t controlSetsOf(const PartSpec& part) {
	return controlSetsOf(part.role, laneCount(part));
}
/// The kind of a `role` part's control set `set`, in controlSetsOf()'s order.
constexpr ControlSetKind controlSetKind(Role role, int32_t set) {
	const ControlSetLayout layout = controlSetLayout(role);
	return layout.own && set == 0 ? layout.ownKind : layout.laneKind;
}
/// The kind of the `role` part's set that its events on `lane` move: its own for Lane::None, else its lane's.
constexpr ControlSetKind controlSetKindOf(Role role, Lane lane) {
	const ControlSetLayout layout = controlSetLayout(role);
	return lane == Lane::None ? layout.ownKind : layout.laneKind;
}
constexpr ControlSetKind controlSetKind(const PartSpec& part, int32_t set) {
	return controlSetKind(part.role, set);
}
/// The lane of `part`'s control set `set`: Lane::None for a part's own or the kit-wide set, and for any set of a part
/// with no per-lane sets.
constexpr Lane controlSetLane(const PartSpec& part, int32_t set) {
	const ControlSetLayout layout = controlSetLayout(part.role);
	if ((layout.own && set == 0) || !layout.perLane) {
		return Lane::None;
	}
	return part.lanes[set - (layout.own ? 1 : 0)].lane;
}
/// The delay setup of `part`'s control set on `lane` (spec §10.6): a lane's row's, or for Lane::None the part's own
/// (a synth's, or a Kit's at kit level). Off for a lane the part doesn't list.
constexpr DelaySetup delaySetupOf(const PartSpec& part, Lane lane) {
	if (lane == Lane::None) {
		return part.delay;
	}
	const int32_t l = laneIndexOf(part, lane);
	return l >= 0 ? part.lanes[l].delay : DelaySetup{};
}
/// Whether the genre's FX part lists a Siren lane (spec §10.5).
constexpr bool genreHasSiren(const Genre& genre) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (genre.parts[p].role == Role::FX && laneIndexOf(genre.parts[p], Lane::Siren) >= 0) {
			return true;
		}
	}
	return false;
}
/// Whether any part, row or kit of the genre has a delay setup (spec §10.6).
constexpr bool genreHasDelaySetup(const Genre& genre) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		if (part.delay.on) {
			return true;
		}
		for (int32_t l = 0; l < laneCount(part); l++) {
			if (part.lanes[l].delay.on) {
				return true;
			}
		}
	}
	return false;
}
/// Whether a Throw on `part`'s control set on `lane` lifts delay feedback (spec §11.4): it has a delay setup and the
/// part a Throw depth above 0. Otherwise it throws reverb only.
constexpr bool throwsDelay(const PartSpec& part, Lane lane) {
	return delaySetupOf(part, lane).on && part.throwDelayDepth > 0;
}
/// The control sets over the whole roster.
constexpr int32_t controlSets(const Genre& genre) {
	int32_t sets = 0;
	for (int32_t p = 0; p < partCount(genre); p++) {
		sets += controlSetsOf(genre.parts[p]);
	}
	return sets;
}
/// `perPart(role, lanes)` summed over the largest roster (kLargestRoster), each Kit with every lane: what the
/// Generator block's per-roster capacities are counted from.
template <typename PerPart>
constexpr int32_t overLargestRoster(PerPart perPart) {
	int32_t sum = 0;
	for (Role role : kLargestRoster) {
		sum += perPart(role, maxLanes(role));
	}
	return sum;
}
/// The most control sets a roster has: the largest roster's. A genre with more fails rosterFits().
constexpr int32_t kMaxControlSets =
    overLargestRoster([](Role role, int32_t lanes) { return controlSetsOf(role, lanes); });

} // namespace deluge::generative
