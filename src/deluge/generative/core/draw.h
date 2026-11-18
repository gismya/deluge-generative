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

// Keyed draws (spec §6.1–§6.4). Every random choice the Generator makes is a pure hash of
// (seed, part, lane, take, purpose, position), so draws never affect each other: skipping work, computing a muted
// part or turning a macro never shifts any other draw. The core never calls the firmware's random sources.

#pragma once

#include "generative/core/types.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

/// What a draw is for. Each purpose keys on the Arc counters it chooses, so repetition is designed in.
/// Append new purposes at the end: the value is part of the key, so reordering changes every event stream.
enum class Purpose : uint8_t {
	DensityJitter,  ///< position: pick (Groove, phrase, pool entry), step within the pattern
	LineRoll,       ///< position: pick (Groove, phrase, pool entry), step within the line, field
	LineMutation,   ///< position: pick (Groove, phrase, pool entry), passes since the line was rolled, step, field
	GrooveLength,   ///< position: Groove occurrence
	BreakLength,    ///< position: Break occurrence
	BuildLength,    ///< position: Build occurrence
	GrooveToBuild,  ///< position: Groove occurrence
	ComplexityTier, ///< position: pick (Groove, phrase). Whether a lane or part takes the next tier up
	PatternPick,    ///< position: pick (Groove, phrase), tier. Which pool entry plays
	PatternRepick,  ///< position: Groove occurrence, phrase in the Groove. Never keyed on the take
	Interplay,      ///< position: pick (Groove, phrase, pool entry), rule index, step within the pattern
	VelocitySpread, ///< position: Repeat, pick (Groove, phrase, pool entry) and step within the pattern; Fresh, the
	                ///< step
	Ornament,       ///< position: pick (Groove, phrase, pool entry), field (whether, which), step within the pattern
	PhraseAccent,   ///< position: Groove occurrence, phrase in the Groove. Never keyed on the take
	Answer,         ///< position: pick (Groove, phrase, pool entry), bars since the pick, field (tier, which, way)
	Fill,           ///< position: Section occurrence, phrase in the Section, Section kind and field (whether, which)
	HeldFill,       ///< position: the step a fresh-pick FILL press lands on
	FillTier,       ///< position: pick (Groove, phrase). As ComplexityTier, for a Swap's fill pattern
	FillPattern,    ///< position: pick (Groove, phrase), tier. As PatternPick, for a Swap's fill pattern
	DriftPoint,     ///< position: the walk's point index, the control. Lane: the control set's. Never keyed on the take
	DriftPhase,     ///< position: none. Where a set's walk sits between its points. Never keyed on the take
	SweepPhase,     ///< position: none. A part's Sweep starting quarter. Never keyed on the take
	ChordWalk,      ///< position: Section kind and occurrence, phrase in the Section, bar in the phrase. Part kArcPart
	VoiceJitter,    ///< position: the Sustain voicing note's VoicePlace. As DensityJitter, with no pick
	ArpWalk,        ///< position: pick (Groove, phrase, pool entry), step within the pattern. A Random Arp's note
	DropOut, ///< position: Groove occurrence, phrase in the Groove, bar in the phrase and field (whether, which).
	         ///< Part kArcPart. Never keyed on the take
};

/// The part word of draws that belong to no part: the Arc's, the Phrase accent's, fills' and the chord walk's.
constexpr uint8_t kArcPart = 0xFF;

/// Everything a draw is keyed on. Position words are whichever Arc counters the purpose names.
struct DrawKey {
	uint32_t seed = 0;
	uint8_t part = 0;
	uint8_t lane = 0xFF; ///< Lane value, or 0xFF for a part-wide draw.
	uint8_t take = 0;
	Purpose purpose = Purpose::DensityJitter;
	uint32_t position[3] = {};
};

namespace detail {
/// A 32-bit integer finaliser with good avalanche ("lowbias32", Chris Wellons). Unsigned, so wrapping is defined.
constexpr uint32_t mix(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}
} // namespace detail

/// A uniform 32-bit value for `key`.
constexpr uint32_t draw(const DrawKey& key) {
	uint32_t h = detail::mix(key.seed ^ 0x9e3779b9U);
	h = detail::mix(h
	                ^ (static_cast<uint32_t>(key.part) | static_cast<uint32_t>(key.lane) << 8
	                   | static_cast<uint32_t>(key.take) << 16 | static_cast<uint32_t>(key.purpose) << 24));
	for (uint32_t word : key.position) {
		h = detail::mix(h ^ word);
	}
	return h;
}

/// `value`, a uniform 32-bit value, scaled to a uniform value in [0, n), for 0 < n. Multiply-high, so no modulo bias
/// worth hearing.
constexpr int32_t scaleBelow(uint32_t value, int32_t n) {
	return static_cast<int32_t>((static_cast<uint64_t>(value) * static_cast<uint32_t>(n)) >> 32);
}

/// A uniform value in [0, n), for 0 < n.
constexpr int32_t drawBelow(const DrawKey& key, int32_t n) {
	return scaleBelow(draw(key), n);
}

/// The index of one of `count` entries rolled by weight (`weight(i)`, never negative), or -1 when they weigh nothing.
/// `below(total)` gives the roll in [0, total), and is called only when the entries weigh something. Keyed draws
/// (drawWeightedIf()) and fresh-entropy rolls (a palette's, a Recipe's key) share it.
template <typename Weight, typename Below>
constexpr int32_t rollWeighted(int32_t count, Weight weight, Below below) {
	int32_t total = 0;
	for (int32_t i = 0; i < count; i++) {
		total += weight(i);
	}
	if (total <= 0) {
		return -1;
	}
	int32_t roll = below(total);
	for (int32_t i = 0; i < count; i++) {
		roll -= weight(i);
		if (roll < 0) {
			return i;
		}
	}
	return -1;
}

/// The index of an entry drawn by its `weight` among those `in(entry)` holds for, in their order, or -1 if it holds
/// for none. When their weights are all 0, the first of them.
template <typename Entry, typename In>
constexpr int32_t drawWeightedIf(const DrawKey& key, std::span<const Entry> entries, In in) {
	// The weight pass notes the first entry `in()` holds for, so weights of all 0 need no third pass.
	int32_t first = -1;
	const int32_t picked = rollWeighted(
	    static_cast<int32_t>(entries.size()),
	    [&](int32_t i) {
		    if (!in(entries[i])) {
			    return int32_t{0};
		    }
		    first = first < 0 ? i : first;
		    return static_cast<int32_t>(entries[i].weight);
	    },
	    [&](int32_t total) { return drawBelow(key, total); });
	return picked >= 0 ? picked : first;
}

/// The index of an entry drawn by its `weight`, or -1 for an empty list. A list whose weights are all 0 gives its first
/// entry.
template <typename Entry>
constexpr int32_t drawWeighted(const DrawKey& key, std::span<const Entry> entries) {
	return drawWeightedIf(key, entries, [](const Entry&) { return true; });
}

/// A stream of uniform values from fresh entropy, for what is rolled once and never replayed: a Recipe's facts (spec
/// §3.2) and sound picks (spec §10.1). Not keyed: each value depends on the ones before it.
class EntropyStream {
public:
	constexpr explicit EntropyStream(uint32_t state) : state_(state) {}

	/// The next uniform 32-bit value.
	constexpr uint32_t next() {
		state_ = detail::mix(state_ + 0x9e3779b9U);
		return state_;
	}
	/// The next value, scaled to [0, n), for 0 < n.
	constexpr int32_t below(int32_t n) { return scaleBelow(next(), n); }

private:
	uint32_t state_;
};

/// True with a chance of `perMille` / 1000.
constexpr bool drawChance(const DrawKey& key, int32_t perMille) {
	return drawBelow(key, 1000) < perMille;
}

// ---- Draw keys (spec §6.1) ----

/// A draw key for a part, or one of its lanes (Lane::None for a part-wide draw), before its position words.
DrawKey partKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose);

/// `key` positioned on a Groove and a phrase within it: word 0 and word 1. Word 2 is the purpose's.
DrawKey onPhrase(DrawKey key, uint16_t groove, uint16_t phrase, uint32_t word2 = 0);

/// A position word holding `value` with a draw's field in its low byte, for purposes that draw several fields.
constexpr uint32_t fieldWord(uint32_t value, int32_t field) {
	return value << 8 | static_cast<uint32_t>(field);
}

/// `key` for another field of the same draw: the field is position[2]'s low byte.
DrawKey withField(DrawKey key, int32_t field);

} // namespace deluge::generative
