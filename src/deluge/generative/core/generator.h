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

// The Generator (spec §2.1, §2.2): one instance's whole state in one fixed-size block, and the step function that
// turns a Recipe into the event stream. No globals, no allocation, no firmware headers, integer math only.
//
// A driver runs it like this (the Perform driver does it from the tick, 12 ticks ahead of each step):
//
//   generator.start(recipe, seed, takes);
//   each time the clock reaches decisionTick(generator.nextStep()): generator.decideNextStep(queue, log);
//   each tick: while (generator.popDue(now, &event)) play(event);
//   on stop, a jump, exit or retiring an Output: generator.flush(now), then play what popDue() gives.

#pragma once

#include "generative/core/arc.h"
#include "generative/core/event.h"
#include "generative/core/event_buffer.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/sounding_notes.h"
#include "generative/core/steering.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

// ---- Buffer sizes, by construction (spec §2.7) ----

constexpr int32_t kMaxRatchetHits = 4;
constexpr int32_t kMaxFlamGraceHits = 1;
/// Hits one note can make in a step: a ratchet, or a flam's grace hits plus the main hit.
constexpr int32_t kMaxHitsPerNote = kMaxRatchetHits > 1 + kMaxFlamGraceHits ? kMaxRatchetHits : 1 + kMaxFlamGraceHits;
/// Note sources in the largest roster: one Drums part (every lane), one FX part (every lane) and the rest pitched,
/// each playing a full chord (spec §5.1, §5.3).
constexpr int32_t kMaxNoteSourcesPerStep = kNumDrumLanes + kNumFxLanes + (kMaxParts - 2) * kMaxChordNotes;
constexpr int32_t kMaxNoteOnsPerStep = kMaxNoteSourcesPerStep * kMaxHitsPerNote;
/// Every sounding note can be ended in one step, plus one Section and one KeyScale event.
constexpr int32_t kMaxEventsPerStep = kMaxNoteOnsPerStep + kMaxParts * kMaxSoundingPerPart + 2;
/// The step being decided plus the step still playing.
constexpr int32_t kEventBufferSize = 2 * kMaxEventsPerStep;

/// The worst case, worked out part by part from the largest roster rather than from the constants above: a Kit part
/// can hit every lane, a pitched part a full chord, every hit can be a ratchet or a flam, and every note sounding can
/// end in the same step as a Section starts on the piece's first step.
constexpr int32_t worstCaseStepEvents() {
	constexpr Role kLargestRoster[kMaxParts] = {Role::Drums, Role::FX, Role::Bass, Role::Chords, Role::Pad, Role::Lead};
	int32_t events = 2; // a Section and a KeyScale
	for (Role role : kLargestRoster) {
		int32_t sources = role == Role::Drums ? kNumDrumLanes : role == Role::FX ? kNumFxLanes : kMaxChordNotes;
		events += sources * kMaxHitsPerNote; // its note-ons
		events += kMaxSoundingPerPart;       // its note-offs
	}
	return events;
}
static_assert(kEventBufferSize >= 2 * worstCaseStepEvents(),
              "the event buffer must hold a worst-case step plus the step still playing (spec §2.7)");
static_assert(kMaxSoundingPerPart >= kNumDrumLanes && kMaxSoundingPerPart >= 2 * kMaxChordNotes,
              "a part's sounding-note table must hold a hit per lane, or a chord overlapping the next one");

/// A step of a Loop line (spec §5.2): pitch content over the pattern's rhythm.
struct LineStep {
	int8_t degree = 0; ///< Scale steps from the chord root.
	int8_t octave = 0;
	bool accent = false;
	bool slide = false;
};

/// Which pool entry a lane or patterned part plays, and how many picks it has made, which keys its draws.
struct PatternState {
	int8_t entry = -1; ///< Index into the pool, -1 before the first pick.
	uint16_t pick = 0;
};

struct PartState {
	PatternState lanes[kMaxLanesPerPart]; ///< Drums and FX: one per lane. A patterned pitched part uses lanes[0].
	LineStep line[kMaxPatternSteps];      ///< Loop parts.
	uint16_t passes = 0;                  ///< Passes since the line was rolled; keys mutation.
};

class Generator {
public:
	/// Begins a piece: every counter and the Arc reset, the Intro starts on step 0.
	void start(const Recipe& recipe, uint32_t seed, const Takes& takes);

	/// The step function. Decides nextStep(): drains the steering queue onto it (logging each command where it
	/// lands), moves the Arc, and buffers the step's events and the note-offs due before the next decision.
	void decideNextStep(SteeringQueue& queue, SteeringLog* log);

	/// The next step to decide. Its decision is due at decisionTick(nextStep()).
	[[nodiscard]] int32_t nextStep() const { return nextStep_; }

	/// Takes the next buffered event due at or before `now`, in play order.
	bool popDue(int32_t now, Event* out);
	/// The tick of the earliest buffered event, or INT32_MAX.
	[[nodiscard]] int32_t nextEventTick() const { return events_.nextTick(); }

	/// Ends everything at `now` (stop, a jump, exit, retiring an Output): buffered events that haven't played are
	/// dropped, and every note that started gets a note-off at `now`. popDue() then gives those note-offs.
	void flush(int32_t now);

	// ---- Queries, safe from task context between ticks (spec §2.8) ----

	[[nodiscard]] const Recipe& recipe() const { return recipe_; }
	[[nodiscard]] const Arc& arc() const { return arc_; }
	/// A macro as the last decided step played it: the Recipe centre plus the Arc's offset.
	[[nodiscard]] int32_t effectiveMacro(Macro macro) const;
	/// Events a full event buffer has dropped. A harness spec fails on any.
	[[nodiscard]] uint32_t droppedEvents() const { return dropped_; }
	/// Notes a full sounding-note table has ended early. A harness spec fails on any.
	[[nodiscard]] uint32_t evictedNotes() const { return evicted_; }

private:
	struct Step {
		int32_t index;
		int32_t tick;
		int32_t energy;
		int32_t density;
		int32_t complexity;
		int32_t motion;
	};

	void apply(const Command& command);
	void decidePart(int32_t partIndex, const Step& step);
	void decideLane(int32_t partIndex, int32_t laneIndex, const Step& step);
	void decideLoop(int32_t partIndex, const Step& step);
	/// Re-checks a lane's or part's Complexity tier at its pattern's wrap. @return true on a new pick.
	bool updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band, int32_t complexity) const;
	[[nodiscard]] bool plays(const Pattern& pattern, int32_t patternStep, uint8_t part, Lane lane,
	                         const PatternState& state, int32_t density) const;
	[[nodiscard]] uint8_t velocity(VelocityLevel level, int32_t energy) const;
	void rollLine(int32_t partIndex);
	void mutateLine(int32_t partIndex, int32_t motion);
	[[nodiscard]] uint8_t resolvePitch(const PartSpec& spec, const LineStep& lineStep) const;

	void noteOn(int32_t partIndex, Lane lane, uint8_t pitch, uint8_t velocity, int32_t onTick, int32_t offTick,
	            bool ornament);
	/// Buffers anything but a note-on. @return Whether it was stored (always, by the buffer's size).
	bool push(const Event& event);
	/// A buffered note-on was dropped before it played: its note leaves the table, or its queued note-off goes.
	void forgetUnplayed(const Event& noteOn);
	void queueNoteOffsBefore(int32_t tick);

	// The Recipe, seed and takes this instance plays, with steering applied.
	Recipe recipe_;
	uint32_t seed_ = 0;
	Takes takes_;
	const Genre* genre_ = nullptr; ///< Constant data, never owned.

	int32_t nextStep_ = 0;
	Arc arc_;
	PartState parts_[kMaxParts];
	SoundingNotes sounding_;
	EventBuffer<kEventBufferSize> events_;
	uint32_t dropped_ = 0;
	uint32_t evicted_ = 0;
};

/// H18 (spec §20): the state block stays small. Perform allocates two of them on entry.
static_assert(sizeof(Generator) <= 16 * 1024, "the Generator state block outgrew its budget");

} // namespace deluge::generative
