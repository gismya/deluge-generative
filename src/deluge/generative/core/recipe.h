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

// The Recipe (spec §3): what the Generator should make. It holds no notes. The seed and the takes sit beside it.

#pragma once

#include "generative/core/pitch.h"
#include "generative/core/types.h"
#include <algorithm>
#include <cstdint>
#include <optional>

namespace deluge::generative {

struct Genre;

// ---- Macros (spec §3.3) ----

enum class Macro : uint8_t { Energy, Density, Complexity, Motion };
constexpr int32_t kNumMacros = 4;

/// Macros are 0–50 on the OLED and stored at full resolution: 64 units per point, so a gold-knob detent
/// (128 across the range) is exactly 25 units and every point on the OLED is a whole number of units.
constexpr int32_t kMacroUnitsPerPoint = 64;
constexpr int32_t kMacroPoints = 50;
constexpr int32_t kMacroFull = kMacroPoints * kMacroUnitsPerPoint; // 3200
constexpr int32_t kMacroUnitsPerKnobStep = kMacroFull / 128;       // 25
static_assert(kMacroUnitsPerKnobStep * 128 == kMacroFull);

/// Macro units for a value written in OLED points, as genre data and the spec write them.
constexpr int32_t macroPoints(int32_t points) {
	return points * kMacroUnitsPerPoint;
}
/// The OLED value (0–50) of a stored macro, rounded to nearest.
constexpr int32_t macroToPoints(int32_t units) {
	return (units + kMacroUnitsPerPoint / 2) / kMacroUnitsPerPoint;
}

/// A value read off a table of `count` knots spread evenly over a macro's range, 0 to kMacroFull, interpolated
/// linearly between knots (Motion curves, spec §3.5; the Density curve, spec §8.1). `count - 1` divides kMacroFull.
template <typename Value>
constexpr int32_t interpolateMacro(const Value* knots, int32_t count, int32_t macro) {
	if (macro <= 0) {
		return knots[0];
	}
	if (macro >= kMacroFull) {
		return knots[count - 1];
	}
	int32_t span = kMacroFull / (count - 1);
	int32_t knot = macro / span;
	int32_t low = knots[knot];
	int32_t high = knots[knot + 1];
	return low + (high - low) * (macro % span) / span;
}

struct Macros {
	int16_t value[kNumMacros] = {macroPoints(25), macroPoints(25), macroPoints(25), macroPoints(25)};

	constexpr int32_t operator[](Macro macro) const { return value[static_cast<int32_t>(macro)]; }
	constexpr void set(Macro macro, int32_t units) {
		value[static_cast<int32_t>(macro)] = static_cast<int16_t>(units < 0            ? 0
		                                                          : units > kMacroFull ? kMacroFull
		                                                                               : units);
	}
	constexpr bool operator==(const Macros&) const = default;
};

/// Each part's Part bias (spec §3.3): a signed offset per Macro that the part plays at around the Recipe value, after
/// the Arc, through partBiased() (core/part_bias.h). In macro units, -kMacroFull..kMacroFull, by roster index. 0
/// plays the Recipe value.
struct PartBiases {
	int16_t value[kMaxParts][kNumMacros] = {};

	[[nodiscard]] constexpr int32_t of(int32_t part, Macro macro) const {
		return value[part][static_cast<int32_t>(macro)];
	}
	constexpr void set(int32_t part, Macro macro, int32_t units) {
		value[part][static_cast<int32_t>(macro)] =
		    static_cast<int16_t>(std::clamp<int32_t>(units, -kMacroFull, kMacroFull));
	}
	/// Whether `part` has a bias on any macro.
	[[nodiscard]] constexpr bool any(int32_t part) const {
		const int16_t* bias = value[part];
		return (bias[0] | bias[1] | bias[2] | bias[3]) != 0;
	}
	constexpr bool operator==(const PartBiases&) const = default;
};
static_assert(kNumMacros == 4, "PartBiases::any() reads four macros");

// ---- Sound picks (spec §3.1, §10.1) ----

/// The longest card path a sound pick holds, with its terminator. Longer paths are never picked.
constexpr int32_t kMaxSoundPath = 128;

/// A card path, held in place so a Recipe owns its picks whatever the card lists.
struct SoundPath {
	char text[kMaxSoundPath] = {};

	[[nodiscard]] constexpr bool empty() const { return text[0] == 0; }
	[[nodiscard]] constexpr const char* c_str() const { return text; }
	/// Sets the path to `folder` + '/' + `name`, or to `folder` alone when `name` is null. False, and empty, if it
	/// doesn't fit.
	bool set(const char* folder, const char* name = nullptr);
	void clear() { text[0] = 0; }
};

/// The Recipe's sound picks (spec §3.1): a card preset per synth part and a sample per Drums or FX lane. An empty
/// lane pick is a lane with nothing to load. They are stored beside the Recipe, not in it: the core never reads them
/// (a sound changes what the music sounds like, never how it plays), so the Generator's copy of the Recipe leaves them
/// out.
struct SoundPicks {
	SoundPath part[kMaxParts];
	SoundPath lane[kNumLanes];
};

/// A sound the player chose (spec §10.1, §13.5): a card preset for a synth part (`lane` None), or a file for one of a
/// Drums or FX part's lanes, a sample, or a preset for an FX lane that takes one (spec §10.5).
struct SoundChoice {
	SoundPath path;
	Lane lane = Lane::None;
};

// ---- Variant sets (spec §3.6, ADR 0008) ----

/// A genre lists at most this many variant sets (spec §4.4), each with 2 to kMaxVariants variants.
constexpr int32_t kMaxVariantSets = 8;
constexpr int32_t kMaxVariants = 8;
static_assert(kMaxVariants <= 8, "VariantPicks::allowed is a uint8_t bitmask, a bit a variant");
/// A pick a Recipe file named but this firmware doesn't know, until drawUnsetVariants() draws it (core/variants.h).
constexpr uint8_t kUnsetVariant = 0xFF;

/// The Recipe's variant picks (spec §3.6): per set of its genre, by the set's index in the genre's list, the variant
/// playing (`pick`, an index into the set's variants) and the variants a draw may land on (`allowed`, a bit per
/// variant). Every bit set is RANDOM, one bit a fixed variant, several a subset. Stored, not derived from the seed, so
/// NEW SEED keeps them and a Recipe file, a Capture header and AS IT BEGAN carry them. Sets past the genre's are 0.
struct VariantPicks {
	uint8_t pick[kMaxVariantSets] = {};
	uint8_t allowed[kMaxVariantSets] = {};

	constexpr bool operator==(const VariantPicks&) const = default;
};

// ---- The Recipe (spec §3.1, §3.2) ----

/// The longest swing interval a Recipe holds: 5 doublings of a 16th (2 bars), the coarsest sync level at the
/// Generator's magnitude (spec §12.3).
constexpr int32_t kMaxSwingInterval = 5;
/// A Recipe's swing amount runs from -kMaxSwing to kMaxSwing, as the song's does.
constexpr int32_t kMaxSwing = 49;
/// A Recipe's tempo, in hundredths of a BPM, runs from kMinTempo to kMaxTempo.
constexpr int32_t kMinTempo = 10 * 100;
constexpr int32_t kMaxTempo = 999 * 100;

struct Recipe {
	const Genre* genre = nullptr; ///< Constant genre data. Files store its stable name.
	int32_t tempo = 12800;        ///< Hundredths of a BPM, as UNPATCHED_TEMPO holds it.
	uint8_t keyRoot = 9;          ///< Pitch class, 0 = C.
	Scale scale = Scale::Minor;
	int8_t swing = 0;          ///< Song swing amount, -kMaxSwing..kMaxSwing.
	uint8_t swingInterval = 0; ///< A note value: 0 the 16th, 1 the 8th, each step doubling. Drivers map it.
	Macros macros;
	/// Each pitched part's octave shift, -kMaxOctaveShift..kMaxOctaveShift, by roster index: its Register moves by
	/// 12 semitones an octave (spec §3.1, §7.3). 0 for Drums and FX, which have no Register.
	int8_t octave[kMaxParts] = {};
	/// Each part's Part bias (spec §3.3). Regenerate keeps it, as it keeps the macros; a genre change sets it to 0.
	PartBiases bias;
	/// Each variant set's pick and allowed variants (spec §3.6). A new Recipe and Regenerate draw them; NEW SEED keeps
	/// them.
	VariantPicks variants;

	constexpr bool operator==(const Recipe&) const = default;
};

/// Takes per part: 0–15, shown as 1–16 (spec §6.6).
constexpr int32_t kMaxTakes = 16;

/// Each part's take (spec §6.6), beside the Recipe.
struct Takes {
	uint8_t take[kMaxParts] = {};

	constexpr bool operator==(const Takes&) const = default;
};

/// A new Recipe from `genre`: its default macros, with facts rolled from `entropy` (fresh entropy, never the seed,
/// spec §3.2). Its sound picks are rolled separately, from the card, by rollSounds() (palette.h).
///
/// A genre change or Regenerate passes the key that plays as `leanFrom` (spec §9.7): the scale is rolled first, then
/// the roots whose key shares 6 or 7 notes with it split kKeyLeanPerMille of the root roll evenly, and the rest is
/// spread evenly over all 12 roots.
///
/// Each variant set is drawn by weight within `narrowed`'s allowed variants (every variant when it's null, or for a set
/// it allows none of), after the facts, from the same stream (spec §3.6): a set never moves a fact.
Recipe newRecipe(const Genre& genre, uint32_t entropy, std::optional<Key> leanFrom = std::nullopt,
                 const VariantPicks* narrowed = nullptr);

/// The related roots' share of a leaning root roll, per mille (spec §9.7, tuning).
constexpr int32_t kKeyLeanPerMille = 650;

} // namespace deluge::generative
