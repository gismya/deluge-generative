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
/// Every sounding note can be ended in one step and re-struck on a Section line, plus one Section and one KeyScale
/// event.
constexpr int32_t kMaxEventsPerStep = kMaxNoteOnsPerStep + 2 * kMaxParts * kMaxSoundingPerPart + 2;
/// The step being decided plus the step still playing.
constexpr int32_t kEventBufferSize = 2 * kMaxEventsPerStep;

/// The worst case, worked out part by part from the largest roster rather than from the constants above: a Kit part
/// can hit every lane, a pitched part a full chord, every hit can be a ratchet or a flam, and every note sounding can
/// end and be re-struck in the same step, on a Section line.
constexpr int32_t worstCaseStepEvents() {
	constexpr Role kLargestRoster[kMaxParts] = {Role::Drums, Role::FX, Role::Bass, Role::Chords, Role::Pad, Role::Lead};
	int32_t events = 2; // a Section and a KeyScale
	for (Role role : kLargestRoster) {
		int32_t sources = role == Role::Drums ? kNumDrumLanes : role == Role::FX ? kNumFxLanes : kMaxChordNotes;
		events += sources * kMaxHitsPerNote; // its note-ons
		events += kMaxSoundingPerPart;       // its note-offs
		events += kMaxSoundingPerPart;       // its re-strikes on a Section line
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

/// Which pool entry a lane or patterned part plays (spec §8.1). Every draw for it is keyed on its pick: the Groove
/// and phrase where the pattern last changed by a Drop or a re-pick (the Intro shares the first Groove's), with the
/// take it was picked with. The entry is a pure function of those and the tier, never of how it got there, so
/// turning Complexity up and back down, or stepping back to a take, replays the same music.
struct PatternState {
	int8_t entry = -1;   ///< Index into the pool, -1 before the first pick.
	uint16_t groove = 0; ///< The Groove occurrence of the pick.
	uint16_t phrase = 0; ///< The phrase within that Groove.
};

struct PartState {
	PatternState lanes[kMaxLanesPerPart]; ///< Drums and FX: one per lane. A patterned pitched part uses lanes[0].
	LineStep line[kMaxPatternSteps];      ///< Loop parts.
	uint16_t passes = 0;                  ///< Passes since the line was rolled; keys mutation.
	bool gateOpen = false;                ///< The part's Density gate, as of the last bar line (spec §5.6).
	bool laneGateOpen[kMaxLanesPerPart] = {};
	int8_t pendingTake = -1; ///< A take change waiting for the next phrase line, or -1.
	bool newTake = false;    ///< A new take landed on this step: every pattern re-picks.
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
	/// The chord playing: the home chord (an empty Harmony slot, spec §7.4), with the Extensions the live Complexity
	/// gives within the genre's floor and ceiling, in steps from the tonic.
	[[nodiscard]] Chord chord() const;
	/// Whether a key or scale change is waiting for its line (the SCALE LED blinks, spec §13.5).
	[[nodiscard]] bool keyChangePending() const { return keyPending_; }
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
		/// Where patterns change (spec §8.1): a Drop opens a fresh pick, and a phrase line inside a Groove may re-pick.
		bool drop;
		bool repickLine;
		uint16_t groove; ///< The Groove occurrence playing, on a drop or a re-pick line.
		uint16_t phrase; ///< The phrase within it.
	};

	/// @return Whether to log the command on this step: true if it took effect here. False if it waits for a line
	/// (it is logged where it lands) or is invalid (never logged: an invalid SetKey never waits, see decideNextStep).
	bool apply(const Command& command);
	/// Lands a pending key change on a phrase line or an Arc Drop (spec §7.6, §9.7). @return Whether the key changed.
	bool landKeyChange(const Step& step, SteeringLog* log);
	/// Takes waiting for a phrase line land on this step's, each logged here.
	void landTakes(int32_t step, SteeringLog* log);
	void decidePart(int32_t partIndex, const Step& step);
	/// A Drums or FX part: each lane's pick and planned hit, then Replace pairs, then the hits that are left.
	void decideKit(int32_t partIndex, const Step& step);
	void playLane(int32_t partIndex, int32_t laneIndex, const Step& step);
	void decideLoop(int32_t partIndex, const Step& step);
	/// What updatePick() found on a step.
	enum class PickChange : uint8_t { None, Wrap, NewPick };
	/// Brings a lane's or patterned part's pick up to date on `step` (spec §8.1): a fresh pick at a Drop, a keyed
	/// re-pick chance at a phrase line in a Groove, the pick a new take would have now, and the Complexity tier at each
	/// wrap of its pattern.
	PickChange updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band, uint8_t part, Lane lane,
	                      const Step& step) const;
	/// The pool entry for a pick at `complexity`: the tier band, the remainder's keyed chance of the next tier up, the
	/// nearest non-empty tier at or below, and a weighted keyed pick within it.
	[[nodiscard]] int32_t pickEntry(const PatternState& state, std::span<const Pattern> pool, TierBand band,
	                                uint8_t part, Lane lane, int32_t complexity) const;
	[[nodiscard]] bool plays(const Pattern& pattern, int32_t patternStep, uint8_t part, Lane lane,
	                         const PatternState& state, int32_t density) const;

	// ---- Planned onsets and interplay rules (spec §5.5) ----

	/// What a part or one of its lanes plays from: a lane (laneIndex >= 0) or a patterned pitched part (-1).
	struct Source {
		std::span<const Pattern> pool;
		std::span<const InterplayRule> rules;
		Lane lane;
		const PatternState* state;
	};
	[[nodiscard]] Source source(int32_t part, int32_t laneIndex) const;
	/// Whether a source has a planned onset on `step` of its current pattern pass: Density thinning, then its own
	/// interplay rules. What rules read, before gates, Replace pairs and expression. Pure, so a rule can ask about any
	/// step of an earlier source's pattern.
	[[nodiscard]] bool planned(int32_t part, int32_t laneIndex, int32_t step, int32_t density) const;
	/// Whether a rule's target has a planned onset `back` steps before `step`, wrapping inside each pattern it reads.
	[[nodiscard]] bool targetPlanned(const RuleTarget& target, int32_t step, int32_t back, int32_t density) const;
	[[nodiscard]] bool against(const InterplayRule& rule, int32_t step, int32_t density) const;
	/// A rule's effective strength, in hundredths of a percent of kMacroFull: its strength moved by its sensitivity
	/// over the Recipe's Complexity (spec §5.5).
	[[nodiscard]] int32_t effectiveStrength(const InterplayRule& rule) const;
	/// Energy's velocity layer at `energy`, scaled by the genre's weight (spec §8.4).
	[[nodiscard]] int32_t energyLayer(int32_t energy) const;
	/// A note's velocity: its base (the flavour's level) plus Energy's layer, clamped to 1–127.
	[[nodiscard]] uint8_t velocity(int32_t baseVelocity, int32_t energy) const;

	// ---- Gates and lines (spec §5.6, §12.5) ----

	/// Whether a part's notes on `lane` (Lane::None for a pitched part) sound: its gate and the lane's are open.
	[[nodiscard]] bool sounds(int32_t part, Lane lane) const;
	/// At a bar line: moves every gate with its hysteresis. A closing gate ends its sounding notes on the line; an
	/// opening one brings in the notes it held back that last to the end of the bar (late entry).
	void updateGates(const Step& step);
	/// At a Section line: every sounding note ends on the line, and a note lasting to the end of the first bar is
	/// re-struck there (spec §12.5).
	void cutSectionLine(const Step& step);
	/// Starts `note`'s remainder on `step`'s line with the Energy then, if it lasts to the end of that bar.
	/// @return Whether it did.
	bool lateEntry(int32_t partIndex, const SoundingNote& note, const Step& step);
	void rollLine(int32_t partIndex);
	void mutateLine(int32_t partIndex, int32_t motion);
	[[nodiscard]] Key key() const { return {recipe_.keyRoot, recipe_.scale}; }
	[[nodiscard]] uint8_t resolvePitch(const PartSpec& spec, const LineStep& lineStep) const;

	/// One note the flavours decided, for play() to start.
	struct Note {
		Lane lane = Lane::None;
		uint8_t pitch = 0;
		int16_t baseVelocity = 0; ///< Before Energy's layer.
		uint8_t velocity = 0;
		int32_t onTick = 0;
		int32_t offTick = 0;
	};
	/// Starts a decided note, or holds it back for late entry while its gate is closed.
	void play(int32_t partIndex, const Note& note);
	/// Buffers a note-on and tracks its note in the sounding table. A note on the same key starting on the same tick
	/// (a re-strike on a line) gives way to it.
	void noteOn(int32_t partIndex, const Note& note);
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
	/// A key change waiting for the next phrase line or Arc Drop, as its command so it is logged where it lands.
	Command pendingKey_;
	bool keyPending_ = false;
	PartState parts_[kMaxParts];
	SoundingNotes sounding_;
	SoundingNotes held_; ///< Notes gated-out parts and lanes would be playing, for late entry (spec §5.6).
	EventBuffer<kEventBufferSize> events_;
	uint32_t dropped_ = 0;
	uint32_t evicted_ = 0;
};

/// H18 (spec §20): the state block stays small. Perform allocates two of them on entry.
static_assert(sizeof(Generator) <= 16 * 1024, "the Generator state block outgrew its budget");

} // namespace deluge::generative
