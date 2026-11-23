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

// The Generator block's per-roster capacities, sized by construction from the largest roster (spec §2.7), and the
// checks each genre's data is held to: rosterFits() against those capacities, and genreValid(), every rule a built-in
// genre table `static_assert`s.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/sounding_notes.h"
#include "generative/core/types.h"
#include <cstdint>
#include <span>

namespace deluge::generative {

constexpr int32_t kMaxRatchetHits = 4;
constexpr int32_t kMaxFlamGraceHits = 1;
/// Hits one note can make in a step: a ratchet (the Build roll's 64ths too), or a flam's grace hits plus the main hit.
constexpr int32_t kMaxHitsPerNote = kMaxRatchetHits > 1 + kMaxFlamGraceHits ? kMaxRatchetHits : 1 + kMaxFlamGraceHits;

/// The events one part of `role` can buffer in a step. Each of its onset notes can make kMaxHitsPerNote note-ons, and
/// each of them can end inside the step: a ratchet's earlier hits end at the next hit, and its last can end before the
/// next decision (Ratchet4's 6-tick spacing at Lean −12). Every note sounding from earlier steps can end, and on a bar
/// line every note held back can be re-struck (late entry, the Section-line cut). A Kit lane's key holds one note, and
/// a pitched part's onset can overlap the next one (a slide): its sounding-note and late-entry tables hold no more.
constexpr int32_t stepEventsOf(Role role, bool barLine) {
	const int32_t hits = onsetNotes(role) * kMaxHitsPerNote;
	return 2 * hits + maxSounding(role) * (barLine ? 2 : 1);
}
/// The events one step of `roster` can buffer, part by part (spec §2.7): each part's, plus a Section, a KeyScale and a
/// Pump event (§8.9), and a Sound event per part.
constexpr int32_t stepEventsOf(std::span<const Role> roster, bool barLine) {
	int32_t events = 3;
	for (Role role : roster) {
		events += 1 + stepEventsOf(role, barLine);
	}
	return events;
}
/// The controls a set of `kind` can move: what its layers reach (setMoves()).
constexpr int32_t controlsMovedBy(ControlSetKind kind) {
	int32_t controls = 0;
	for (int32_t c = 0; c < kNumSoundControls; c++) {
		controls += setMoves(kind, static_cast<SoundControl>(c)) ? 1 : 0;
	}
	return controls;
}
/// The Control events a part of `role` with `lanes` lanes can send in one step (spec §11.7): each control each of its
/// sets can move.
constexpr int32_t controlEventsOf(Role role, int32_t lanes) {
	int32_t events = 0;
	for (int32_t set = 0; set < controlSetsOf(role, lanes); set++) {
		events += controlsMovedBy(controlSetKind(role, set));
	}
	return events;
}
/// A genre's Control events in one step at most; rosterFits() holds every genre to kMaxControlEventsPerStep.
constexpr int32_t controlEventsOf(const Genre& genre) {
	int32_t events = 0;
	for (int32_t p = 0; p < partCount(genre); p++) {
		events += controlEventsOf(genre.parts[p].role, laneCount(genre.parts[p]));
	}
	return events;
}
/// Control events one step can send: the largest roster's, each Kit with every lane.
constexpr int32_t kMaxControlEventsPerStep =
    overLargestRoster([](Role role, int32_t lanes) { return controlEventsOf(role, lanes); });
/// The step being decided plus the step still playing (spec §2.7), for the largest roster. Re-strikes happen only on
/// bar lines, so only one of the two has them. Control events play on their step's grid line, before the next step is
/// decided, so only the step being decided holds any.
constexpr int32_t kEventBufferSize =
    stepEventsOf(kLargestRoster, true) + stepEventsOf(kLargestRoster, false) + kMaxControlEventsPerStep;

/// Whether a genre's roster fits the Generator block, which is sized for kLargestRoster (spec §2.7): its parts (at
/// most kMaxParts: partCount() stops there, so a part past it would never play), its sounding notes, its step events
/// on and off a bar line, its control sets and its Control events a step. genreValid() holds every built-in genre to
/// it.
constexpr bool rosterFits(const Genre& genre) {
	Role roster[kMaxParts] = {};
	const int32_t parts = partCount(genre);
	for (int32_t p = 0; p < parts; p++) {
		roster[p] = genre.parts[p].role;
	}
	const std::span<const Role> roles(roster, parts);
	return genre.parts.size() <= kMaxParts && soundingOf(roles) <= SoundingNotes::kTotal
	       && stepEventsOf(roles, true) <= stepEventsOf(kLargestRoster, true)
	       && stepEventsOf(roles, false) <= stepEventsOf(kLargestRoster, false) && controlSets(genre) <= kMaxControlSets
	       && controlEventsOf(genre) <= kMaxControlEventsPerStep;
}

/// Whether a genre's swing range is ordered and inside the Recipe's swing bounds (spec §3.2).
constexpr bool swingRangeValid(const Genre& genre) {
	return -kMaxSwing <= genre.facts.swingLow && genre.facts.swingLow <= genre.facts.swingHigh
	       && genre.facts.swingHigh <= kMaxSwing;
}

/// Whether a genre's data holds every rule the Generator relies on (spec §2.7, §5, §7.4, §8, §9.1, §9.3, §10.1,
/// §11.5): its interplay rules, Avoid's sources and anchors, its Roll fills' lanes, its roster's size, each flavour's
/// data, its Chord graph, its Sweeps and Build lengths, a Build cut that leaves the roll lane, a cut and a Gap shorter
/// than a phrase, its drop-outs (§8.8), its swing range, and a palette (dereferenced unchecked). Each built-in genre
/// table `static_assert`s it; genre_data_spec checks each rule on its own for every genre, so a failure there names the
/// rule. The data checks that only the harness makes (pools, weights and lane names, for example) are in
/// genre_data_spec alone.
constexpr bool genreValid(const Genre& genre) {
	return genre.palette != nullptr && interplayRulesValid(genre) && avoidRulesValid(genre) && rollFillsValid(genre)
	       && rosterFits(genre) && sustainValid(genre) && loopNotesValid(genre) && stabNotesValid(genre)
	       && arpNotesValid(genre) && motifNotesValid(genre) && harmonyValid(genre) && sweepsValid(genre)
	       && buildBarsValid(genre) && rollUncut(genre) && cutAndGapUnderBuild(genre) && dropOutsValid(genre)
	       && alternateLanesValid(genre) && sirensValid(genre) && registersShiftValid(genre) && swingRangeValid(genre);
}

} // namespace deluge::generative
