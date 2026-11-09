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
// parts (with Registers and Degree sets), rhythm (pattern pools with tiers), harmony (the home chord's Extension floor
// and ceiling), the palette and the Arc's offsets and lengths. Later slices
// extend these structs; they don't add per-genre code.

#pragma once

#include "generative/core/palette.h"
#include "generative/core/pitch.h"
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <algorithm>
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
	///   levels:     'g' ghost, 'a' accent, anything else normal
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

/// What a rule marks as against. Lock is built; Avoid lands with Psytrance and Respond with House.
enum class RuleKind : uint8_t {
	/// Against every step where the target has no onset. With `offBeat` (the spec's `off` option), every step except
	/// the target's onsets one 8th
	/// later (wrapping inside its pattern), so a four-on-the-floor kick leaves the & steps free.
	Lock,
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

struct LaneSpec {
	Lane lane = Lane::Kick;
	std::span<const Pattern> pool;
	TierBand band;
	std::span<const InterplayRule> rules;
	/// Plays only while effective Density is at or above this, in macro units (spec §5.6). 0 always plays.
	int16_t densityGate = 0;
	/// Ticks from a hit's note-on to its note-off. Drum one-shots under 2 s ignore it and play out.
	uint8_t gateTicks = 6;
	Expression expression;
};

/// One scale step a Loop line may land on, relative to the chord root, with its draw weight.
struct DegreeWeight {
	int8_t degree = 0;
	uint8_t weight = 1;
};

/// How a Loop part rolls and mutates its line (spec §5.2). Chances are per-mille.
struct LoopSpec {
	std::span<const DegreeWeight> degrees;
	int16_t octaveUp = 100;
	int16_t octaveDown = 0;
	int16_t accent = 200;
	int16_t slide = 150;
	/// Chance a non-anchor step re-rolls at each pass end, at Motion 25 (tuning).
	int16_t mutation = 120;
	/// Ticks a plain note sounds.
	uint16_t gateTicks = 16;
	/// How far a slid note runs into the next step, so a LEGATO voice glides.
	uint8_t slideOverlapTicks = 6;
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
	Expression expression;

	[[nodiscard]] constexpr Register registerOrDefault() const { return reg.anchor ? reg : defaultRegister(role); }
};

/// "`replacing` replaces `replaced`": lanes of one part play independently, except that when both land on the same
/// step, the replaced lane's hit is dropped before any voice starts (spec §5.7).
struct ReplacePair {
	Lane replacing = Lane::OpenHat;
	Lane replaced = Lane::ClosedHat;
};

/// The Generator-wide default, which every v1 genre uses. The Kit's CHOKE group still cuts the hat tails (§10.6).
constexpr ReplacePair kDefaultReplacePairs[] = {{Lane::OpenHat, Lane::ClosedHat}};

// ---- Harmony (slot 7, spec §7.4) ----

/// The genre's Harmony slot. Empty (no Chord graph) means the home chord only, which still gets its Extensions. The
/// Chord graph, harmonic rhythm and cadences come with the first genre that walks one.
struct HarmonySpec {
	uint8_t extensionFloor = 0;
	uint8_t extensionCeiling = 2;
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

/// Parts and lanes as a section rule names them: whole parts by their roster index, and single lanes.
struct PartsAndLanes {
	uint8_t parts = 0;  ///< Bit p: the roster's part p, every lane of it.
	uint16_t lanes = 0; ///< laneBit() per lane.

	/// Whether it holds a part's note on `lane` (Lane::None for a pitched part's).
	[[nodiscard]] constexpr bool has(int32_t part, Lane lane) const {
		return (parts & (1U << static_cast<uint32_t>(part))) != 0
		       || (lane != Lane::None && (lanes & laneBit(lane)) != 0);
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
	/// The same for every Build. The roll lane and the Riser always play.
	PartsAndLanes buildKeeps = kSameAsBreakKeeps;
	/// Parts and lanes that stop for the Build's last buildCutBars bars (0: no cut).
	PartsAndLanes buildCut = {.lanes = laneBit(Lane::Kick)};
	uint8_t buildCutBars = 0;
	GapLength gap = GapLength::None;
	BuildRoll roll;
	/// The lane a Phrase accent hits (spec §9.5), or None: always on a Drop, and on other Groove phrase starts with
	/// this chance, per-mille at Motion 25, scaled by the Phrase accent curve.
	Lane accentLane = Lane::None;
	int16_t accentChance = 0;
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

// ---- The genre ----

struct Facts {
	int16_t tempoLowBpm = 120; ///< Rolls come from here; not a clamp.
	int16_t tempoHighBpm = 130;
	std::span<const Scale> scales;
	int8_t swing = 0;
};

struct Genre {
	// Slot 1: identity (spec §4.4)
	const char* name = ""; ///< Stable, lowercase, used in Recipe files. Never reused.
	const char* oledName = "";
	uint16_t version = 1; ///< Goes up whenever a retune changes the event stream.
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
	// Slot 7: harmony
	HarmonySpec harmony;
	// Slot 8: the built-in palette (spec §10.1)
	const Palette* palette = nullptr;
	// Slot 9: section rules
	ArcSpec arc;

	[[nodiscard]] constexpr int32_t phraseSteps() const { return phraseBars * kStepsPerBar; }
};

namespace genres {
extern const Genre techno;
extern const Palette technoPalette;
} // namespace genres

/// The most built-in genres: v1's five (spec §4.5).
constexpr int32_t kMaxGenres = 5;

namespace detail {

/// Whether `rule`, listed on part `part` (and on `lane` of it, or Lane::None for the part itself), reads something
/// generated before it.
constexpr bool targetsEarlier(const Genre& genre, int32_t part, Lane lane, const InterplayRule& rule) {
	const RuleTarget& target = rule.target;
	if (target.part >= genre.parts.size() || target.part > part) {
		return false;
	}
	const PartSpec& targetSpec = genre.parts[target.part];
	if (target.lane != Lane::None) {
		bool found = false;
		for (const LaneSpec& spec : targetSpec.lanes) {
			found = found || spec.lane == target.lane;
		}
		if (!found) {
			return false;
		}
	}
	if (target.part < part) {
		return true;
	}
	// Within one part, only a lane may read an earlier lane, in the fixed lane order (spec §5.4).
	return lane != Lane::None && target.lane != Lane::None
	       && static_cast<int32_t>(target.lane) < static_cast<int32_t>(lane);
}

} // namespace detail

/// Whether every interplay rule names a part or lane that exists and is generated earlier: an earlier part in the
/// roster, or an earlier lane of the same part in the fixed lane order (spec §5.5). A rule that doesn't would make the
/// Generator's planned() recurse forever, so every built-in genre `static_assert`s it where it is defined.
constexpr bool interplayRulesValid(const Genre& genre) {
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()); p++) {
		const PartSpec& part = genre.parts[p];
		if (!part.lanes.empty() && !part.rules.empty()) {
			return false; // a Kit part plays only its lanes' rules
		}
		for (const InterplayRule& rule : part.rules) {
			if (!detail::targetsEarlier(genre, p, Lane::None, rule)) {
				return false;
			}
		}
		for (const LaneSpec& lane : part.lanes) {
			for (const InterplayRule& rule : lane.rules) {
				if (!detail::targetsEarlier(genre, p, lane.lane, rule)) {
					return false;
				}
			}
		}
	}
	return true;
}

/// Every built-in genre, in menu order. At most kMaxGenres.
std::span<const Genre* const> allGenres();
/// The genre with this stable name, or nullptr.
const Genre* findGenre(const char* name);

/// How many of the genre's parts the Generator plays: the roster, up to kMaxParts.
constexpr int32_t partCount(const Genre& genre) {
	return std::min<int32_t>(static_cast<int32_t>(genre.parts.size()), kMaxParts);
}

/// The sets of sound controls the macro layer moves (spec §11.1, §11.4): one per part, kit-wide on Drums, except that
/// an FX part's lanes each use their own row controls.
constexpr int32_t controlChannels(const Genre& genre) {
	int32_t channels = 0;
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		channels += part.role == Role::FX ? static_cast<int32_t>(part.lanes.size()) : 1;
	}
	return channels;
}
/// The most control channels a roster has: five parts and one FX part with every FX lane. A genre with more fails
/// genre_data_spec.
constexpr int32_t kMaxControlChannels = kMaxParts - 1 + kNumFxLanes;

} // namespace deluge::generative
