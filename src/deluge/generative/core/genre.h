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

/// One pool entry: 1 or 2 bars of steps, each with an importance and a velocity level, a Complexity tier (0 plain to
/// 3 busiest) and a weight above 0 for the pick within its tier. A pool lists its entries by tier, lowest first.
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
			throw "a pattern's tier is 0-3 and its weight at least 1";
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

// ---- Parts and lanes (slot 5, spec §5) ----

struct LaneSpec {
	Lane lane = Lane::Kick;
	std::span<const Pattern> pool;
	TierBand band;
	/// Ticks from a hit's note-on to its note-off. Drum one-shots under 2 s ignore it and play out.
	uint8_t gateTicks = 6;
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
	Register reg;
	/// Bass and Lead parts playing Loop, Arp or Motif: the degrees their notes snap to (spec §7.5).
	DegreeSet degreeSet = DegreeSet::Full;
	LoopSpec loop;

	[[nodiscard]] constexpr Register registerOrDefault() const { return reg.anchor ? reg : defaultRegister(role); }
};

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

struct ArcSpec {
	uint8_t introPhrases = 1;
	uint8_t outroPhrases = 1;
	Range groovePhrases{4, 8};
	Range breakPhrases{1, 1};
	/// Build lengths in bars, drawn by weight. Each must be a whole number of phrases for now (Build placement inside
	/// a phrase is the Section rules slice).
	std::span<const WeightedBars> buildBars = kDefaultBuildBars;
	/// Chance per Groove end of going straight into a Build instead of a Break (spec §9.2), per-mille.
	int16_t grooveToBuild = 0;
	uint8_t dropDecayPhrases = 1;
	int16_t dropLiftEnergy = macroPoints(8);
	int16_t dropLiftDensity = kSameAsEnergy;
	/// Indexed by SectionKind.
	SectionOffsets offsets[kNumSectionKinds] = {
	    {macroPoints(-15), 0, macroPoints(-20), 0},                               // Intro: rising from low
	    {},                                                                       // Groove: home
	    {macroPoints(-10), macroPoints(-10), macroPoints(-20), macroPoints(-20)}, // Break: a dip
	    {0, macroPoints(12), 0, macroPoints(10)},                                 // Build: a ramp above the centre
	    {0, macroPoints(-15), 0, macroPoints(-20)},                               // Outro: falling
	};
};

// ---- Velocity (spec §8.4) ----

/// Base velocity per level, Generator-wide (tuning).
constexpr uint8_t kLevelVelocity[3] = {48, 88, 112};
/// How far Energy's velocity layer moves a note at weight 1000, from Energy 0 to 50 (tuning).
constexpr int32_t kEnergyVelocityRange = 40;

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
	// Slot 4: meter and phrase
	uint8_t phraseBars = 8;
	// Slots 5, 6: parts, rhythm
	std::span<const PartSpec> parts;
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

/// Every built-in genre, in menu order. At most kMaxGenres.
std::span<const Genre* const> allGenres();
/// The genre with this stable name, or nullptr.
const Genre* findGenre(const char* name);

} // namespace deluge::generative
