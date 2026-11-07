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
/// Hits one note can make in a step: a ratchet (the Build roll's 64ths too), or a flam's grace hits plus the main hit.
constexpr int32_t kMaxHitsPerNote = kMaxRatchetHits > 1 + kMaxFlamGraceHits ? kMaxRatchetHits : 1 + kMaxFlamGraceHits;

/// The events one step can buffer, worked out part by part from the largest roster: one Drums part (every lane), one
/// FX part (every lane) and the rest pitched, each playing a full chord (spec §5.1, §5.3). Every note can make
/// kMaxHitsPerNote note-ons, all but the last of which end inside the step; every note sounding from earlier steps
/// can end; and on a bar line every note held back can be re-struck (late entry, the Section-line cut). Plus a
/// Section and a KeyScale event.
constexpr int32_t worstCaseStepEvents(bool barLine) {
	constexpr Role kLargestRoster[kMaxParts] = {Role::Drums, Role::FX, Role::Bass, Role::Chords, Role::Pad, Role::Lead};
	int32_t events = 2;
	for (Role role : kLargestRoster) {
		const int32_t sources = role == Role::Drums ? kNumDrumLanes : role == Role::FX ? kNumFxLanes : kMaxChordNotes;
		// A Kit lane's key holds one note; a pitched part's chord can overlap the next one (a slide). The part's
		// sounding-note and late-entry tables hold no more.
		const int32_t sounding = maxSounding(role);
		events += sources * kMaxHitsPerNote;       // its note-ons
		events += sources * (kMaxHitsPerNote - 1); // its earlier hits' note-offs, inside the step
		events += sounding;                        // note-offs for its notes from earlier steps
		events += barLine ? sounding : 0;          // its re-strikes
	}
	return events;
}
/// The step being decided plus the step still playing (spec §2.7). Re-strikes happen only on bar lines, so only one
/// of the two has them.
constexpr int32_t kEventBufferSize = worstCaseStepEvents(true) + worstCaseStepEvents(false);
static_assert(maxSounding(Role::Drums) == kNumDrumLanes && maxSounding(Role::FX) == kNumFxLanes
                  && maxSounding(Role::Bass) == 2 * kMaxChordNotes,
              "the buffer counts what a part's tables hold: a hit per lane, or a chord overlapping the next one");

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
	bool newTake = false; ///< A new take landed on this step: every pattern re-picks.
};

class Generator {
public:
	/// Begins a piece: every counter and the Arc reset, and nothing pending (every start is fresh, spec §13.2). The
	/// Intro starts on step 0, or with `skipIntro` (SHIFT+PLAY) a Groove.
	void start(const Recipe& recipe, uint32_t seed, const Takes& takes, bool skipIntro = false);

	/// The step function. Decides nextStep(): drains the steering queue onto it (logging each command where it
	/// lands), moves the Arc, and buffers the step's events and the note-offs due before the next decision.
	void decideNextStep(SteeringQueue& queue, SteeringLog* log);

	/// Once ended(): moves the queued line-bound commands into the pending list, as the end line's decision would,
	/// without deciding it. A force that lands on the end line then cancels the ending (spec §13.2). Everything else
	/// stays queued, in order.
	void takeQueued(SteeringQueue& queue);

	/// After a jump's flush (spec §12.4): moves on to `step`, the next bar line at or after nextStep(), deciding
	/// nothing. The Arc carries on through the skipped steps, which hold no line, so nothing lands on them.
	void skipTo(int32_t step);

	/// The next step to decide. Its decision is due at decisionTick(nextStep()).
	[[nodiscard]] int32_t nextStep() const { return nextStep_; }

	/// Takes the next buffered event due at or before `now`, in play order.
	bool popDue(int32_t now, Event* out);
	/// The tick of the earliest buffered event, or INT32_MAX.
	[[nodiscard]] int32_t nextEventTick() const { return events_.nextTick(); }

	/// Why flush() ends everything.
	enum class FlushFor : uint8_t {
		Stop, ///< stop, exit, the piece's end, retiring an Output: nothing more of this piece is heard
		Jump, ///< the piece plays on from elsewhere (spec §12.4)
	};
	/// Ends everything at `now`: buffered events that haven't played are dropped, and every note that started gets a
	/// note-off at `now`. On a jump, a key change or a Section start that was decided but not yet heard still sends its
	/// KeyScale or Section event, at `now`, since the piece carries on in it. On a stop they're dropped: like the
	/// pending list, what nobody heard is cancelled (spec §2.6, §12.5). popDue() then gives those events.
	void flush(int32_t now, FlushFor why);

	// ---- Queries, safe from task context between ticks (spec §2.8) ----

	[[nodiscard]] const Recipe& recipe() const { return recipe_; }
	[[nodiscard]] const Arc& arc() const { return arc_; }
	/// A macro as the last decided step played it: the Recipe centre plus the Arc's offset.
	[[nodiscard]] int32_t effectiveMacro(Macro macro) const;
	/// The chord playing: the home chord (an empty Harmony slot, spec §7.4), with the Extensions the live Complexity
	/// gives within the genre's floor and ceiling, in steps from the tonic.
	[[nodiscard]] Chord chord() const;
	/// Whether a key or scale change is waiting for its line (the SCALE LED blinks, spec §13.5).
	[[nodiscard]] bool keyChangePending() const { return pending_.has(CommandKind::SetKey); }
	/// Whether a forced Section is waiting for its line (its pad blinks, spec §13.5).
	[[nodiscard]] bool sectionForced() const { return pending_.has(CommandKind::ForceSection); }
	/// Whether `kind` is the forced Section waiting for its line.
	[[nodiscard]] bool sectionForced(SectionKind kind) const {
		const Command* waiting = pending_.waiting({CommandKind::ForceSection});
		return waiting != nullptr && waiting->kind == CommandKind::ForceSection
		       && waiting->target == static_cast<uint8_t>(kind);
	}
	/// Whether End is waiting for its line (its pad blinks, spec §13.5).
	[[nodiscard]] bool endQueued() const { return pending_.has(CommandKind::End); }
	/// A part's mute and solo as landed, and whether a press of its pad waits for the bar line (it blinks).
	[[nodiscard]] bool partMuted(int32_t part) const { return (muted_ & (1U << part)) != 0; }
	[[nodiscard]] bool partSoloed(int32_t part) const { return (soloed_ & (1U << part)) != 0; }
	[[nodiscard]] bool anySoloed() const { return soloed_ != 0; }
	[[nodiscard]] bool mutePending(int32_t part) const {
		return pending_.waiting({CommandKind::SetMute, static_cast<uint8_t>(part)}) != nullptr;
	}
	[[nodiscard]] bool soloPending(int32_t part) const {
		return pending_.waiting({CommandKind::SetSolo, static_cast<uint8_t>(part)}) != nullptr;
	}
	/// Whether the Arc silences a part on the last decided step: its gate is closed, or section rules (Break keeps,
	/// the cut, the Gap) stop all its notes. Its MUTE pad shows dim (spec §13.5).
	[[nodiscard]] bool partSilenced(int32_t part) const { return (silent_ & (1U << part)) != 0; }

	/// The actions waiting in the pending list, oldest first (QUEUE, the status line, spec §13.5).
	[[nodiscard]] int32_t queuedCount() const { return pending_.size(); }
	[[nodiscard]] const Command& queued(int32_t i) const { return pending_.at(i); }
	/// The step a waiting action lands on: its next line from nextStep().
	[[nodiscard]] int32_t landsAt(const Command& command) const;
	/// Whether the piece is over: the Outro's last step is decided, and nextStep() is the phrase line where the
	/// transport stops (spec §13.2). Nothing is decided after it. A force waiting to land on that line cancels the
	/// ending: the piece goes on in the forced Section.
	/// M4: replay has no pending list (spec §2.6), so it can't see that waiting force. A replaying driver must not stop
	/// on ended() when the log holds a force stamped on the end line; it applies the force there and goes on.
	[[nodiscard]] bool ended() const {
		return arc_.section() == SectionKind::Outro && arc_.stepInSection() + 1 >= arc_.sectionSteps()
		       && !forceLandsNext();
	}
	/// Events a full event buffer has dropped. A harness spec fails on any.
	[[nodiscard]] uint32_t droppedEvents() const { return dropped_; }
	/// Quantised commands a full pending list refused. Every slot kind fits in it (static_assert below), so this
	/// stays 0; a harness spec fails on any.
	[[nodiscard]] uint32_t refusedSteering() const { return refused_; }
	/// Notes a full sounding-note table has ended early. A harness spec fails on any.
	[[nodiscard]] uint32_t evictedNotes() const { return evicted_; }

private:
	struct Step {
		int32_t index = 0;
		int32_t tick = 0;
		int32_t energy = 0;
		int32_t density = 0;
		int32_t complexity = 0;
		int32_t motion = 0;
		/// Where patterns change (spec §8.1): a Drop opens a fresh pick, and a phrase line inside a Groove may re-pick.
		bool drop = false;
		bool repickLine = false;
		bool sectionStart = false; ///< The first step of a Section: negative Lean clamps to 0 (spec §8.5).
		uint16_t groove = 0;       ///< The Groove occurrence playing, on a drop or a re-pick line.
		uint16_t phrase = 0;       ///< The phrase within it.

		// Section rules on this step (spec §9.5).
		const PartsAndLanes* keeps =
		    nullptr;       ///< In a Break or Build, what still plays; otherwise nullptr (everything).
		PartsAndLanes cut; ///< What the Build cut stops on this step.
		bool gap = false;  ///< In the Gap: no note starts.
		RollStage rollStage = RollStage::None; ///< The Build roll's stage on this step, or None.
		int16_t rollVelocity = 0;              ///< The roll's base velocity, climbing to the Gap.
		bool accent = false;                   ///< The Phrase accent plays on this step.
	};
	/// Works out the section rules for `step` (keeps, cut, Gap, roll, Phrase accent), and ends the pitched notes the
	/// Gap or the cut stops where they start.
	void sectionRules(Step& step);
	/// Whether section rules keep a part's notes on `lane` (Lane::None for a pitched part) from starting.
	[[nodiscard]] bool silenced(const Step& step, int32_t part, Lane lane) const;
	/// The length in ticks of the note an FX section rule plays on `lane` here, or 0 (spec §9.5).
	[[nodiscard]] int32_t fxNoteTicks(const LaneSpec& lane, const Step& step) const;
	/// One step of the Build roll on a lane (spec §8.3).
	void playRoll(int32_t partIndex, int32_t laneIndex, const Step& step);

	/// Makes a command take effect on nextStep(), with everything its landing does (a key change's note-offs and
	/// KeyScale event, a take's re-pick, a forced Section's start). The one path for live steering, pending commands
	/// as they land and, later, replay of the log (ADR 0005). Call it before the Arc enters the step.
	/// @return Whether it took effect, so it is logged on this step. False for an invalid command, never logged.
	bool apply(const Command& command);
	/// A quantised command joins the pending list (spec §2.6). An invalid one is dropped, and a force pressed again
	/// while it waits cancels it. @return false if the list had no room, so it was refused (counted).
	bool wait(Command command);
	/// A SetMute, SetSolo or SetHold pad press, made into the other state. @return false if it cancelled the press
	/// waiting for its line instead, so nothing else happens.
	bool resolvePress(Command& command);
	/// A pad press that lands on the next undecided step: the hold, or a mute or solo with SHIFT.
	static bool landsNow(const Command& command) {
		if (command.kind == CommandKind::SetHold) {
			return command.value == kPress;
		}
		return (command.kind == CommandKind::SetMute || command.kind == CommandKind::SetSolo)
		       && command.value == kPressNow;
	}
	/// Works out which parts the Arc silences on `step`: a closed gate, or section rules on every lane.
	void updateSilent(const Step& step);
	/// Whether a part's note-ons sound: while any part is soloed only the soloed ones do, otherwise every part not
	/// muted (spec §12.5).
	[[nodiscard]] bool audible(int32_t part) const {
		const uint32_t bit = 1U << part;
		return soloed_ != 0 ? (soloed_ & bit) != 0 : (muted_ & bit) == 0;
	}
	/// Whether a waiting force lands on nextStep().
	[[nodiscard]] bool forceLandsNext() const;
	/// Pending commands whose line is the next step land on it, each applied and logged there.
	void landPending(SteeringLog* log);
	void decidePart(int32_t partIndex, const Step& step);
	/// A Drums or FX part: each lane's pick and planned hit, then Replace pairs, then the hits that are left.
	void decideKit(int32_t partIndex, const Step& step);
	/// A note a Kit lane plays on a step: from its pattern, or from a section rule.
	struct LaneNote {
		bool on = false;
		VelocityLevel level = VelocityLevel::Normal;
		int32_t length = 0;      ///< Ticks.
		int32_t patternStep = 0; ///< Its step within the lane's pattern, keying the expression stage's draws.
	};
	void playLane(int32_t partIndex, int32_t laneIndex, const LaneNote& note, const Step& step);

	/// A note a flavour decided on a step, before the expression stage.
	struct Decided {
		Lane lane = Lane::None;
		uint8_t pitch = 0;
		VelocityLevel level = VelocityLevel::Normal;
		int32_t length = 0; ///< Ticks.
	};
	/// The expression stage, shared by every role (spec §8.2, §8.4, §8.5): Velocity spread, Lean, then an Ornament,
	/// then the note's hits. `pick` and `patternStep` key its draws.
	void express(int32_t partIndex, const Expression& expression, const PatternState& pick, int32_t patternStep,
	             const Decided& decided, const Step& step);
	enum class OrnamentKind : uint8_t { None, Ratchet2, Ratchet3, Ratchet4, Flam };
	/// Whether a note gets an Ornament, and which (spec §8.2).
	[[nodiscard]] OrnamentKind ornamentFor(int32_t partIndex, const Ornaments& ornaments, const PatternState& pick,
	                                       int32_t patternStep, Lane lane) const;
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
	/// A rule's effective strength, 0 to kFullStrength (100%), in 1/kMacroFull-percent units: its strength moved by
	/// its sensitivity over the Recipe's Complexity (spec §5.5).
	[[nodiscard]] int32_t effectiveStrength(const InterplayRule& rule) const;
	/// Energy's velocity layer at `energy`, scaled by the genre's weight (spec §8.4).
	[[nodiscard]] int32_t energyLayer(int32_t energy) const;
	/// A note's velocity: its base (the flavour's level) plus Energy's layer, clamped to 1–127.
	[[nodiscard]] uint8_t velocity(int32_t baseVelocity, int32_t energy) const;

	// ---- Gates and lines (spec §5.6, §12.5) ----

	/// Whether a part's notes on `lane` (Lane::None for a pitched part) sound: its gate and the lane's are open.
	[[nodiscard]] bool gateOpen(int32_t part, Lane lane) const;
	/// At a bar line, before the parts: moves every gate with its hysteresis. A closing gate ends its sounding notes
	/// on the line; an opening one lets the notes it held back enter late.
	void updateGates(const Step& step);
	/// At a Section line, before the parts: every sounding note ends on the line, and is held back to be re-struck by
	/// late entry (spec §12.5). FX notes aren't held: their tails ring out, and FX notes a gate held back are dropped.
	void cutSectionLine(const Step& step);
	/// At a bar line, after the parts: each held-back note of a part or lane now sounding starts its remainder on the
	/// line with the Energy then, if it lasts to the end of the bar and the step started no note on its key.
	void lateEntries(const Step& step);
	/// The earliest tick a new hit on `lane` and `pitch` may start: after every hit already started or buffered on
	/// that key, so steps that lean into each other never cross.
	[[nodiscard]] int32_t keyFloor(int32_t partIndex, Lane lane, uint8_t pitch) const;
	void rollLine(int32_t partIndex);
	void mutateLine(int32_t partIndex, int32_t motion);
	[[nodiscard]] Key key() const { return {recipe_.keyRoot, recipe_.scale}; }
	[[nodiscard]] uint8_t resolvePitch(const PartSpec& spec, const LineStep& lineStep) const;

	/// One hit of a note, for noteOn() to start.
	struct Note {
		Lane lane = Lane::None;
		uint8_t pitch = 0;
		int16_t baseVelocity = 0; ///< Before Energy's layer.
		uint8_t velocity = 0;
		int32_t onTick = 0;
		int32_t offTick = 0;
		bool ornament = false;
	};
	/// Buffers a note-on and tracks its note in the sounding table. A note held back on its key for late entry gives
	/// way to it.
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
	/// Quantised steering waiting for its line. A stop cancels it: every start is fresh (spec §2.6, §13.2).
	PendingList pending_;
	uint32_t refused_ = 0;
	/// Bit p: the roster's part p is muted, or soloed. Performance state (spec §12.5, §13.1).
	uint8_t muted_ = 0;
	uint8_t soloed_ = 0;
	/// Bit p: the Arc silences part p on the last decided step (its gate, Break keeps, the cut, the Gap).
	uint8_t silent_ = 0;
	PartState parts_[kMaxParts];
	SoundingNotes sounding_;
	/// Notes held back for late entry (spec §5.6): what gated-out parts and lanes would be playing, and, on a Section
	/// line, what the line cut.
	SoundingNotes held_;
	EventBuffer<kEventBufferSize> events_;
	uint32_t dropped_ = 0;
	uint32_t evicted_ = 0;
};

static_assert(PendingList::kCapacity >= 2 + 3 * kMaxParts,
              "the pending list holds a key change, a force, and a take, a mute and a solo per part at once");

/// H18 (spec §20): the state block stays small. Perform allocates two of them on entry.
static_assert(sizeof(Generator) <= 16 * 1024, "the Generator state block outgrew its budget");

} // namespace deluge::generative
