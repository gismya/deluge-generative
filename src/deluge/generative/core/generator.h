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
#include "generative/core/arc_plan.h"
#include "generative/core/capacity.h"
#include "generative/core/draw.h"
#include "generative/core/event.h"
#include "generative/core/event_buffer.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/sounding_notes.h"
#include "generative/core/steering.h"
#include "generative/core/types.h"
#include "generative/core/voicing.h"
#include <array>
#include <cstdint>
#include <span>

namespace deluge::generative {

/// A step of a line (spec §5.2): pitch content over the pattern's rhythm. A Loop step and a Motif slot share it, since
/// a Motif's is a Loop's without the slide. Packed in 2 bytes: every part keeps a line of kMaxPatternSteps in both
/// Generator blocks.
struct LineStep {
	int8_t degree = 0;     ///< A Loop's scale steps from the chord root, or a Motif's chord position.
	int8_t octave : 2 = 0; ///< -1, 0 or +1.
	bool accent : 1 = false;
	bool slide : 1 = false;
};
static_assert(sizeof(LineStep) == 2, "a line step packs into 2 bytes");

/// Which pool entry a lane or patterned part plays (spec §8.1). Every draw for it is keyed on its pick: the Groove
/// and phrase where the pattern last changed by a Drop or a re-pick (the Intro shares the first Groove's), with the
/// take it was picked with. The entry is a pure function of those and the tier, never of how it got there, so
/// turning Complexity up and back down, or stepping back to a take, replays the same music.
struct PatternState {
	/// Where a pick happened.
	struct Pick {
		uint16_t groove = 0; ///< The Groove occurrence.
		uint16_t phrase = 0; ///< The phrase within that Groove.
		uint16_t bar = 0;    ///< The bar it began on, counted from the piece's start (modulo 2^16): Answers key on it.
	};
	int8_t entry = -1;       ///< Index into the pool, -1 before the first pick and after a thaw takes a waiting one.
	bool hasWaiting = false; ///< A Drop or re-pick came while the part was frozen (spec §6.7).
	Pick pick;               ///< The pick playing.
	Pick waiting;            ///< With hasWaiting: the pick it takes on unfreeze.
};

/// `key` positioned on a pick: its Groove in word 0, its phrase in word 1's top half and its pool entry in word 2's
/// top byte. The purpose fills the rest: an `index` in word 1's low half (passes since a roll, a rule) and a `step`
/// in word 2's low 24 bits (a step within the pattern or line, with any field in its low byte).
DrawKey onPick(DrawKey key, const PatternState& state, uint16_t index, uint32_t step);

/// The controls that drift (drifts()): the walk keeps a point pair for each.
constexpr int32_t kDriftingControls = [] {
	int32_t count = 0;
	for (int32_t c = 0; c < kNumSoundControls; c++) {
		count += drifts(static_cast<SoundControl>(c)) ? 1 : 0;
	}
	return count;
}();
/// The drifting controls in SoundControl order: a walk's point pair `d` is kDrifting[d]'s.
constexpr std::array<SoundControl, kDriftingControls> kDrifting = [] {
	std::array<SoundControl, kDriftingControls> drifting{};
	for (int32_t c = 0, d = 0; c < kNumSoundControls; c++) {
		if (drifts(static_cast<SoundControl>(c))) {
			drifting[d++] = static_cast<SoundControl>(c);
		}
	}
	return drifting;
}();
/// The resolution a walk's points are kept at.
constexpr int32_t kWalkQuantum = 8;

/// One set of sound controls (spec §11.1): a part's, kit-wide or a lane's on Drums, or an FX lane's row controls.
/// `sent` is each control's offset as last sent: home (0) until a Control event says otherwise.
struct ControlSet {
	uint8_t part = 0;
	Lane lane = Lane::None;
	ControlSetKind kind = ControlSetKind::Part;
	int8_t sent[kNumSoundControls] = {};
	/// A drop-out's Throw owns `thrown` (spec §8.8): set only by the target's spike in the window before it, cleared
	/// when the Throw ends or another rule takes it over. Fills the byte of padding before `thrown`.
	bool dropOutHeld = false;
	/// Throw (spec §11.4): steps since the hit that spiked Space, or -1 while no Throw plays.
	int16_t thrown = -1;
	/// Drift's walk (spec §11.5), for the drifting controls in SoundControl order (Brightness, Decay, Space, Wobble):
	/// the seeded points it walks between, per-mille ÷ kWalkQuantum, and the low 16 bits of `from`'s index (kNoWalk
	/// before the first). Drawn again only when the walk passes a point, which keeps the draws out of most steps.
	int8_t walkFrom[kDriftingControls] = {};
	int8_t walkTo[kDriftingControls] = {};
	uint16_t walkPoint = kNoWalk;
	static constexpr uint16_t kNoWalk = UINT16_MAX;
};

/// Mutes and solos (spec §12.5, §13.4): bit p is the roster's part p. The Generator's, which apply() lands, and the
/// Arc plan's copy, which lands the waiting ones where they wait.
struct MuteSolo {
	uint8_t muted = 0;
	uint8_t soloed = 0;
	/// Whether a part's note-ons sound: while any part is soloed only the soloed ones do, otherwise every part not
	/// muted (spec §12.5).
	[[nodiscard]] constexpr bool audible(int32_t part) const {
		const uint32_t bit = 1U << part;
		return soloed != 0 ? (soloed & bit) != 0 : (muted & bit) == 0;
	}
	/// The mask a SetMute or SetSolo sets: `muted` or `soloed`.
	[[nodiscard]] constexpr uint8_t of(CommandKind kind) const { return kind == CommandKind::SetMute ? muted : soloed; }
	/// A valid SetMute or SetSolo lands: its part's bit set or cleared (spec §13.4).
	constexpr void land(const Command& command) {
		uint8_t& parts = command.kind == CommandKind::SetMute ? muted : soloed;
		const auto bit = static_cast<uint8_t>(1U << command.target);
		parts = command.value != 0 ? parts | bit : parts & ~bit;
	}
};

static_assert(kMaxLanesPerPart <= 16, "a part's lane gates fit Gates::lanes");
/// A part's Density gates as of the last bar line (spec §5.6): its own, and bit l for lane l's. PartState holds the
/// playing ones; the Arc plan steps a copy.
struct Gates {
	bool open = false;
	uint16_t lanes = 0;
	[[nodiscard]] constexpr bool laneOpen(int32_t lane) const { return (lanes & (1U << lane)) != 0; }
	/// Crosses a bar line at `density`: each gate opens or closes by gateOpens(). updateGates() moves the playing
	/// gates and then cuts what a closing one stops; the Arc plan moves its copy.
	void crossBar(const PartSpec& spec, int32_t density);
};

/// Whether any of `spec`'s lanes passes `test`, called with the lane's index and its Lane.
template <typename Test>
constexpr bool anyLane(const PartSpec& spec, Test test) {
	for (int32_t l = 0; l < laneCount(spec); l++) {
		if (test(l, spec.lanes[l].lane)) {
			return true;
		}
	}
	return false;
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

/// Whether `spec` holds the Build roll's lane and the roll plays at `stage` (spec §8.3, §9.5): it plays whatever the
/// part's and the lane's gates, so mayPlay() counts it as sounding.
constexpr bool rollsIn(const PartSpec& spec, Lane rollLane, RollStage stage) {
	return stage != RollStage::None && laneIndexOf(spec, rollLane) >= 0;
}

/// Whether a part may sound under `gates` (spec §5.6, §13.5): its gate is open and `test` lets it, for a Kit on any
/// lane whose own gate is open, for a pitched part as a whole (Lane::None).
template <typename Test>
constexpr bool maySound(const PartSpec& spec, Gates gates, Test test) {
	if (!gates.open) {
		return false;
	}
	if (spec.lanes.empty()) {
		return test(Lane::None);
	}
	return anyLane(spec, [&](int32_t l, Lane lane) { return gates.laneOpen(l) && test(lane); });
}

/// Whether a part may play on a step (spec §8.3, §13.5): maySound() under `gates` and `test`, or the Build roll rolls
/// in it at `roll`. The MUTE pad's dim (Generator::updateSilent()) and the Arc plan (Generator::planArc()) both ask
/// it, each with its own test. Nothing the test stops stops a rolling lane: rollStageOn() is None in the Gap, the cut
/// leaves the roll lane (rollUncut(), which genreValid() holds the built-in genres to, not a spec's own genre data)
/// and Keeps::lets() keeps it in a Build. A new way a lane plays past its gates, or a new way to stop the roll lane,
/// goes here.
template <typename Test>
constexpr bool mayPlay(const PartSpec& spec, Gates gates, Lane rollLane, RollStage roll, Test test) {
	return maySound(spec, gates, test) || rollsIn(spec, rollLane, roll);
}

struct PartState {
	PatternState lanes[kMaxLanesPerPart]; ///< Drums and FX: one per lane. A patterned pitched part uses lanes[0].
	LineStep line[kMaxPatternSteps];      ///< Loop and Motif parts.
	uint16_t passes = 0;                  ///< Passes since the line was rolled; keys mutation.
	/// The Answer playing this bar, or None for the call (spec §5.2). Drawn at its first step.
	AnswerKind answer = AnswerKind::None;
	int8_t answerWay = 1; ///< A pitch shift's way: +1 up, -1 down.
	bool slid = false;    ///< The part's last Loop note slides into the next step.
	bool newTake = false; ///< A new take landed on this step: every pattern re-picks.
	Gates gates;
	/// While frozen: the first step of the phrase it froze in, whose draws it replays (spec §6.7).
	int32_t frozenPhraseStart = 0;
	/// A Sustain or Stab part's voicing as of the last bar line (spec §5.2): the whole voicing before Density
	/// thinning, which the next chord's voice leading starts from.
	HeldVoicing voicing;
	/// A Sustain part's state as of the last bar line (spec §5.2).
	struct Sustain {
		/// Bit per VoicePlace: whether Density lets that note play, with the gate hysteresis.
		uint8_t openPlaces : kVoicePlaces = 0;
		/// Its notes started muted: a mute or unmute re-strikes them on the next bar line.
		bool muted : 1 = false;
	} sustain;
};

class Generator {
public:
	/// Begins a piece: every counter and the Arc reset, and nothing pending (every start is fresh, spec §13.2). The
	/// Intro starts on step 0, or with `skipIntro` (SHIFT+PLAY) a Groove.
	void start(const Recipe& recipe, uint32_t seed, const Takes& takes, bool skipIntro = false);
	/// Begins a transition's new roster (spec §9.7, §12.9), as start() does, with the pending Recipe: on a Drop for
	/// `Drop`, with its Intro for `Fade`, in Groove for `Cut`. Its step 0 is the start step; the driver decides it once
	/// the outgoing instance stops.
	void startIncoming(const Recipe& recipe, uint32_t seed, const Takes& takes, TransitionStyle style);
	/// At the start step, before the first decision: the new roster plays the live macros, as a genre change and
	/// Regenerate keep them (spec §12.9), and a held Arc stays held from its first Groove (spec §9.7).
	void carryOver(const Macros& macros, bool held);

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

	// ---- A transition, in the outgoing instance (spec §2.8, §12.9) ----

	/// Whether a transition waits in the pending list for its line: it can still be cancelled.
	[[nodiscard]] bool transitionWaiting() const { return pending_.has(CommandKind::Transition); }
	/// The waiting transition's command, or nullptr.
	[[nodiscard]] const Command* waitingTransition() const { return pending_.waiting({CommandKind::Transition}); }
	/// Once the transition has landed, its start step: the Drop, the seam or the cut line. Otherwise -1.
	[[nodiscard]] int32_t transitionStart() const { return stopAt_ == kNever ? -1 : stopAt_; }
	/// Whether the instance decides nothing more: it has reached its transition's start step. Its buffered events still
	/// play (note-offs, hits leaning late) until isDrained().
	[[nodiscard]] bool stoppedDeciding() const { return nextStep_ >= stopAt_; }
	/// Whether a stopped instance has nothing left to play: it can go idle.
	[[nodiscard]] bool isDrained() const { return stoppedDeciding() && events_.size() == 0 && !throwsFalling(); }
	/// A stopped instance's Throws still holding or falling (spec §11.4): the outgoing parts' Space after the start
	/// step. The driver calls fallThrows() once a step, at the step's decision tick, while one is.
	[[nodiscard]] bool throwsFalling() const;
	/// Moves a stopped instance's Throws on by its next step, its parts silent, sending each Space that changes.
	void fallThrows();

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
	/// The chord playing on the last decided step, in steps from the tonic (spec §7.4): `voiced_`, which
	/// voiceChordAt() voices on each bar line, and as a piece starts, from the walk's root at the Extension level the
	/// Recipe's Complexity gives then, within the genre's floor and ceiling (the Arc never moves Complexity). Its root
	/// is chordRoot(). Before the first start(), the plain home triad.
	[[nodiscard]] Chord chord() const;
	/// The playing chord's degree from the tonic, 0 for home: the walk's place on the last decided step. An empty
	/// Harmony slot stays home.
	[[nodiscard]] int32_t chordRoot() const { return voiced_.steps[0]; }
	/// Whether a key or scale change is waiting for its line (the SCALE LED blinks, spec §13.5).
	[[nodiscard]] bool keyChangePending() const { return pending_.has(CommandKind::SetKey); }
	/// The key the piece goes on in: the waiting change's key, or else the key that plays (the key and scale editor
	/// starts from it, spec §13.5).
	[[nodiscard]] Key keyAhead() const;
	/// Whether a forced Section is waiting for its line (its pad blinks, spec §13.5).
	[[nodiscard]] bool sectionForced() const { return pending_.has(CommandKind::ForceSection); }
	/// Whether `kind` is the forced Section waiting for its line.
	[[nodiscard]] bool sectionForced(SectionKind kind) const {
		const Command* force = waitingForce();
		return force != nullptr && force->target == static_cast<uint8_t>(kind);
	}
	/// Whether End is waiting for its line (its pad blinks, spec §13.5).
	[[nodiscard]] bool endQueued() const { return pending_.has(CommandKind::End); }
	/// A part's mute and solo as landed, and whether a press of its pad waits for the bar line (it blinks).
	[[nodiscard]] bool partMuted(int32_t part) const { return (mutes_.muted & (1U << part)) != 0; }
	[[nodiscard]] bool partSoloed(int32_t part) const { return (mutes_.soloed & (1U << part)) != 0; }
	[[nodiscard]] bool anySoloed() const { return mutes_.soloed != 0; }
	/// Whether a part is frozen (its CROSS-SCREEN LED, spec §13.5).
	[[nodiscard]] bool partFrozen(int32_t part) const { return (frozen_ & (1U << part)) != 0; }
	/// A part's take on the last decided step (spec §6.6).
	[[nodiscard]] int32_t take(int32_t part) const { return takes_.take[part]; }
	/// The take waiting for a part's next phrase line, or -1.
	[[nodiscard]] int32_t takeWaiting(int32_t part) const { return waitingValue(CommandKind::SetTake, part); }
	/// The token of a part's new sound waiting for its phrase line, or -1 (spec §10.7).
	[[nodiscard]] int32_t soundWaiting(int32_t part) const { return waitingValue(CommandKind::SetSound, part); }
	/// The token of the last sound that landed on a part in this piece, or -1. Its Sound event plays on the line.
	[[nodiscard]] int32_t soundLanded(int32_t part) const {
		return part >= 0 && part < kMaxParts && landedSound_[part] != kNoSound ? landedSound_[part] : -1;
	}
	[[nodiscard]] bool mutePending(int32_t part) const {
		return pending_.waiting({CommandKind::SetMute, static_cast<uint8_t>(part)}) != nullptr;
	}
	[[nodiscard]] bool soloPending(int32_t part) const {
		return pending_.waiting({CommandKind::SetSolo, static_cast<uint8_t>(part)}) != nullptr;
	}
	/// The fill entry FILL plays while held, by its index in the genre's fills, or -1 (spec §8.7).
	[[nodiscard]] int32_t heldFill() const { return heldFill_; }
	/// Whether the Arc silences a part on the last decided step: its gate is closed, or section rules (Break keeps,
	/// the cut, the Gap) or a drop-out stop all its notes, unless the Build roll plays one of its lanes and the cut or
	/// the Gap doesn't stop it. Its MUTE pad shows dim (spec §13.5).
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
	/// A transition's Outro ends where the new roster starts, so it never ends the piece, and nor does an Outro while a
	/// transition waits.
	[[nodiscard]] bool ended() const {
		return outroEndsAfter(arc_, nextStep_ - 1) && stopAt_ == kNever && !transitionWaiting();
	}
	/// The Arc plan (spec §2.8): from the phrase the last decided step is in, up to ArcPlan::kMaxPhrases phrases as
	/// the Arc will play them, with the pending list landed where it waits and the Recipe as it is now. Worked out on
	/// demand from a copy of the Arc's counters and its keyed draws: no cache. Costs a step of Arc arithmetic per step
	/// planned, so the SONG page asks only when a phrase line passes or the queue changes.
	void planArc(ArcPlan& out) const;
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
		Keeps keeps;       ///< What still plays: in a Break or Build its keeps, otherwise everything.
		PartsAndLanes cut; ///< What the Build cut stops on this step.
		bool gap = false;  ///< In the Gap: no note starts.
		RollStage rollStage = RollStage::None; ///< The Build roll's stage on this step, or None.
		int16_t rollVelocity = 0;              ///< The roll's base velocity, climbing to the Gap.
		bool accent = false;                   ///< The Phrase accent plays on this step.

		// Fills on this step (spec §8.7), after the section rules they give way to.
		PartsAndLanes rest; ///< What a Rest stops.
		PartsAndLanes swap; ///< Lanes a Swap plays fill patterns on.
		/// What a drop-out silences on this step (spec §8.8). Worked out before the fills, which give way to it.
		PartsAndLanes dropOut;
	};
	/// start() and startIncoming(): resets everything, with the Arc entering `first` (a Drop's Groove with `onDrop`).
	void begin(const Recipe& recipe, uint32_t seed, const Takes& takes, SectionKind first, bool onDrop);
	/// Works out the section rules for `step` (keeps, cut, Gap, roll, Phrase accent), and ends the pitched notes the
	/// Gap or the cut stops where they start.
	void sectionRules(Step& step);
	/// Works out the fill playing on `step`, after the section rules: a held fill, or the phrase's automatic fill,
	/// drawn on its first step. A Rest ends the notes sounding on its pitched parts as it starts.
	void fillRules(Step& step);
	/// Works out the drop-out on `step`, after the section rules and before the fills (spec §8.8): a Section line ends
	/// one playing and cancels one waiting, and on a Groove's bar line, with none playing or waiting and the gap
	/// passed, one may be drawn to start on the next bar line.
	void dropOutRules(Step& step);
	/// The automatic fill for the phrase starting on `step`, or -1 (spec §8.7).
	[[nodiscard]] int32_t drawAutoFill(const Step& step) const;
	/// Whether this Groove phrase is the Groove's last before a Break or Build: by Motion as it is now, or by a force
	/// waiting in the pending list.
	[[nodiscard]] bool lastGroovePhrase() const;
	/// Whether the bar starting on this step is the one before the phrase's automatic fill starts: Loop parts answer in
	/// it (spec §5.2).
	[[nodiscard]] bool barBeforeAutoFill() const;
	/// Where the phrase's automatic fill starts, in steps from the phrase line, for an entry `beats` long.
	[[nodiscard]] int32_t autoFillStart(int32_t beats) const;
	/// Whether section rules or a drop-out keep a part's notes on `lane` (Lane::None for a pitched part) from starting.
	[[nodiscard]] bool silenced(const Step& step, int32_t part, Lane lane) const;
	/// Whether section rules or a Rest keep a part's notes on `lane` from starting.
	[[nodiscard]] bool keptOut(const Step& step, int32_t part, Lane lane) const {
		return silenced(step, part, lane) || step.rest.has(part, lane);
	}
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
	/// A SetMute, SetSolo, SetHold or SetFreeze pad press, made into the other state. @return false if it cancelled the
	/// press waiting for its line instead, so nothing else happens.
	bool resolvePress(Command& command);
	/// A pad press that lands on the next undecided step: the hold, a freeze, or a mute or solo with SHIFT.
	static bool landsNow(const Command& command) {
		if (command.kind == CommandKind::SetHold || command.kind == CommandKind::SetFreeze) {
			return command.value == kPress;
		}
		return (command.kind == CommandKind::SetMute || command.kind == CommandKind::SetSolo)
		       && command.value == kPressNow;
	}
	/// Works out each control set's target on `step`, after its notes are decided, and sends a Control event for each
	/// control whose target changed since it was last sent: ramped over the step, or snapped on the piece's first
	/// step, a Reset, and where a Throw starts or a part playing again ends it (spec §11.3–§11.7).
	void updateControls(const Step& step);
	/// The macro layer's offset for `control` on the last decided step (spec §11.3).
	[[nodiscard]] int32_t macroLayer(SoundControl control) const;
	/// Where a Throw's line is, if the step just decided is in the window before it (spec §11.4).
	struct ThrowLine {
		bool near = false;
		GestureMoment moment = GestureMoment::ThrowBreak;
		/// The drop-out's throw target, if one was drawn (spec §8.8): its hits spike in the window before it starts
		/// (`dropOutNear`), and its Throw holds while it plays (`dropOutLeft` steps left, this one included) and falls
		/// over its last beat. In the beat from where it ended (`dropOutFalling`), a Throw it held falls, so one a
		/// Section line ended early falls over a beat from that line.
		PartsAndLanes dropOutTarget;
		bool dropOutNear = false;
		bool dropOutFalling = false;
		int32_t dropOutLeft = 0;
	};
	[[nodiscard]] ThrowLine throwLine() const;
	/// Close and Wash's level on `step`, and Rise's on the Arc's step, per-mille (spec §11.4).
	[[nodiscard]] int32_t closeWashLevel(int32_t step) const;
	[[nodiscard]] int32_t riseLevel() const;
	/// The genre's part `set` belongs to: its gestures and drift.
	[[nodiscard]] const PartSpec& partOf(const ControlSet& set) const { return genre_->parts[set.part]; }
	/// Whether `test(lane)` holds for any lane of `set`'s part: what a kit-wide set reads for its kit.
	template <typename Test>
	[[nodiscard]] bool anyLaneOf(const ControlSet& set, Test test) const {
		return anyLane(partOf(set), [&](int32_t, Lane lane) { return test(lane); });
	}
	/// Whether a gesture acts on `set`: a part's or FX lane's own, or on Drums the kit-wide set or the lane it aims at.
	[[nodiscard]] bool gestureActsOn(const ControlSet& set, Gesture gesture) const;
	/// Whether `mask` holds `set`'s notes: a pitched part's set its part, a lane's set its lane, a kit-wide set any
	/// lane of its kit.
	[[nodiscard]] bool touches(const ControlSet& set, const PartsAndLanes& mask) const;
	/// Moves `set`'s Throw on a step with `hits`; true if its Space snaps (a Throw starts, or a hit ends it).
	bool updateThrow(ControlSet& set, const ThrowLine& line, const PartsAndLanes& hits);
	/// Whether `set`'s notes stop at a Throw's line: a Break that doesn't keep them, or a Gap.
	[[nodiscard]] bool silencedAt(const ControlSet& set, ThrowLine line) const;
	/// The gesture layer's offset for `set`'s `control` (spec §11.4). `closeWash` and `rise` are Close and Wash's and
	/// Rise's levels on this step, per-mille.
	[[nodiscard]] int32_t gestureLayer(const ControlSet& set, SoundControl control, int32_t closeWash,
	                                   int32_t rise) const;
	/// The drift layer's offset for each of `set`'s controls on `step` (spec §11.5), its walk `walked` per-mille of the
	/// way between its points.
	void driftLayer(const ControlSet& set, const Step& step, int32_t walked,
	                int32_t (&offsets)[kNumSoundControls]) const;
	/// Moves `set`'s walk to where the Generator-wide walk stands at `walkPoint` and `walkTowards`: driftPoint_ and
	/// driftTowards_ on the step being decided, or, in fallThrows(), stepped back to the last decided step's. Draws the
	/// points it passed. Returns how far it is from `walkFrom` to `walkTo`, eased, per-mille.
	int32_t updateWalk(ControlSet& set, uint32_t walkPoint, int32_t walkTowards);
	/// Works out which parts the Arc silences on `step`: a closed gate, or section rules on every lane, unless the
	/// Build roll plays one of its lanes and the cut or the Gap doesn't stop it.
	void updateSilent(const Step& step);
	/// The value of `part`'s command of `kind` waiting in the pending list, or -1.
	[[nodiscard]] int32_t waitingValue(CommandKind kind, int32_t part) const {
		const Command* waiting = pending_.waiting({kind, static_cast<uint8_t>(part)});
		return waiting != nullptr ? waiting->value : -1;
	}
	/// The forced Section waiting in the slot it shares with End, or nullptr.
	[[nodiscard]] const Command* waitingForce() const;
	/// Whether the Outro ends the piece after `step`, with the Arc at `arc` on it (spec §13.2): it is the Outro's last
	/// step, and no waiting force lands on the line after it. ended() asks it of the last decided step, and the Arc
	/// plan of each step it plans.
	[[nodiscard]] bool outroEndsAfter(const Arc& arc, int32_t step) const;
	/// Steps from nextStep() to the line a pending command lands on, 0 if it lands there: the one rule for landing a
	/// command and for counting down to it.
	[[nodiscard]] int32_t stepsToLine(const Command& command) const;
	/// The Arc move a waiting `command` makes, if it makes one (sectionMoveOf()), with `out.at` where it lands, in
	/// steps from the last decided step. waitingMove() and walkChord()'s harness assert both count with it.
	[[nodiscard]] bool moveAt(const Command& command, Arc::Move& out) const;
	/// The waiting force, End or transition that moves the Arc first, and where it lands, from the last decided step;
	/// `at` is Arc::kNotAhead when none waits.
	[[nodiscard]] Arc::Move waitingMove() const;
	/// Pending commands whose line is the next step land on it, each applied and logged there.
	void landPending(SteeringLog* log);
	/// The start step: every note still sounding ends on it, as on a Section line, except a hit leaning past it, which
	/// keeps its own note-off (spec §12.9). The new roster plays from here.
	void endAtStop();
	/// How long the transition `command` plays before its start step: its Build, the Outro, or nothing for a cut.
	[[nodiscard]] int32_t transitionSteps(const Command& command) const;
	void decidePart(int32_t partIndex, const Step& step);
#ifdef GENERATOR_TEST_SEAMS
	/// Harness only (spec §21.2): stands in for the flavours the core has no code for yet. decidePart() hands it a part
	/// whose flavour it can't play, in that flavour's place in the step; nullptr leaves the part silent. TestFlavour,
	/// defined by the spec that sets it, may use the Generator's private members.
	friend struct TestFlavour;
	static inline void (*testFlavour_)(Generator& generator, int32_t partIndex, const Step& step) = nullptr;
#endif
	/// A Drums or FX part: each lane's pick and planned hit, then Replace pairs, then the hits that are left.
	void decideKit(int32_t partIndex, const Step& step);
	/// A note a Kit lane plays on a step: from its pattern, or from a section rule.
	struct LaneNote {
		bool on = false;
		VelocityLevel level = VelocityLevel::Normal;
		int32_t length = 0;      ///< Ticks.
		int32_t patternStep = 0; ///< Its step within the lane's pattern, keying the expression stage's draws.
		int8_t fill = -1;        ///< A Swap's fill pattern it plays, by its index in the lane's fill pool, or -1.
	};
	/// The pick a Swap's fill pattern plays as, keying its draws: the lane's own pick, with the fill pattern's index
	/// after the pool's entries, so no draw of it matches one of a normal pattern.
	[[nodiscard]] static PatternState fillPick(const PatternState& lanePick, const LaneSpec& lane, int32_t fill);
	void playLane(int32_t partIndex, int32_t laneIndex, const LaneNote& note, const Step& step);

	/// A note a flavour decided on a step, before the expression stage.
	struct Decided {
		Lane lane = Lane::None;
		uint8_t pitch = 0;
		VelocityLevel level = VelocityLevel::Normal;
		int32_t length = 0; ///< Ticks.
		/// It follows a slide: a note its slide ran into on the same pitch lasts on instead, so a slide onto the
		/// same note ties (spec §7.5, §8.6).
		bool tie = false;
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
	/// A Sustain part (spec §5.2, §8.1): on each bar line, its voicing thinned by Density. A note that still plays
	/// holds on to the next bar line; the whole voicing re-strikes at a chord change, a retrigger line, a Section line,
	/// a key change or a mute change; a note that comes in starts on the bar line, and one that leaves ends there.
	void decideSustain(int32_t partIndex, const Step& step);
	/// A Stab part (spec §5.2, §8.1): the voiced chord on each onset of its pattern that Density and the interplay
	/// rules let through. The voicing changes only on bar lines, voice-led from the one before; a stab lasts its gate,
	/// ending sooner at the pattern's next onset or the next bar line.
	void decideStab(int32_t partIndex, const Step& step);
	/// An Arp part (spec §5.2, §8.1): on each onset of its pattern that Density and the interplay rules let through,
	/// the next note of its pool entry's walk over the current chord. The walk position counts the pattern's onsets
	/// since the pass start, before thinning, so it keeps no state. A note lasts its gate, ending sooner at the
	/// pattern's next onset or the next bar line.
	void decideArp(int32_t partIndex, const Step& step);
	/// A Motif part (spec §5.2, §8.1): on each onset of its pattern that Density and the interplay rules let through,
	/// its line slot's chord position played on the current chord, so a remembered phrase follows each chord change.
	/// A note lasts its gate, ending sooner at the pattern's next onset or the next bar line.
	void decideMotif(int32_t partIndex, const Step& step);
	/// How long a Stab, Arp or Motif note on `patternStep` lasts: onsetTicks() at the step's place in its bar, with
	/// the Lean `expression` gives it.
	[[nodiscard]] int32_t onsetLength(const Pattern& pattern, int32_t patternStep, int32_t gate,
	                                  const Expression& expression) const;
	/// What updatePick() found on a step.
	enum class PickChange : uint8_t { None, Wrap, NewPick };
	/// What playingStep() found: the step in the pattern that plays, or -1, and how the pick moved.
	struct PlayingStep {
		int32_t step = -1;
		PickChange change = PickChange::None;
	};
	/// A patterned pitched part's step (spec §8.1): brings its pick up to date, then gives the step in its pattern
	/// when an onset plays here (Density thinning, the interplay rules and the section rules let it through), or -1.
	/// A part with no pool plays nothing.
	[[nodiscard]] PlayingStep playingStep(int32_t partIndex, const Step& step);
	/// A Loop or Motif line after its pick moved (spec §5.2, §6.6, §6.7): a new pick rolls it fresh; a pass end
	/// mutates it, or, while the part is frozen, only counts the pass.
	void updateLine(int32_t partIndex, PickChange change, const Step& step);
	/// Moves a lane's or patterned part's pick to `step` where a Drop or a re-pick opens one (spec §8.1), or, while the
	/// part is frozen, keeps it waiting. Lanes with only a fill pool need this alone: their fill patterns key on it.
	/// @return Whether the pick playing changed.
	bool placePick(PatternState& state, uint8_t part, Lane lane, const Step& step) const;
	/// Brings a lane's or patterned part's pick up to date on `step` (spec §8.1): a fresh pick at a Drop, a keyed
	/// re-pick chance at a phrase line in a Groove, the pick a new take would have now, and the Complexity tier at each
	/// wrap of its pattern.
	PickChange updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band, uint8_t part, Lane lane,
	                      const Step& step) const;
	/// The pool entry for a pick at `complexity`: the tier band, the remainder's keyed chance of the next tier up, the
	/// nearest non-empty tier at or below, and a weighted keyed pick within it.
	/// `purposes` key its two draws: ComplexityTier and PatternPick for a pool, FillTier and FillPattern for a Swap's
	/// fill pattern from a fill pool.
	struct PickPurposes {
		Purpose tier;
		Purpose pick;
	};
	[[nodiscard]] int32_t pickEntry(const PatternState& state, std::span<const Pattern> pool, TierBand band,
	                                uint8_t part, Lane lane, int32_t complexity, PickPurposes purposes) const;
	[[nodiscard]] bool plays(const Pattern& pattern, int32_t patternStep, uint8_t part, Lane lane,
	                         const PatternState& state, int32_t density) const;
	/// An importance with its keyed Density jitter, in the threshold's units: fixed per key, so more Density only ever
	/// adds what clears the threshold (spec §8.1).
	[[nodiscard]] int32_t jitteredImportance(DrawKey key, int32_t importance) const;

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
	/// Rolls a Loop or Motif part's line fresh from its pick and take, at the live Complexity (spec §5.2).
	void rollLine(int32_t partIndex, int32_t complexity);
	/// The end of a pass: each step that isn't an anchor re-rolls with the genre's chance on its flavour's curve, the
	/// Loop mutation curve or the Motif re-roll curve. A Motif slot keeps its accent.
	void mutateLine(int32_t partIndex, int32_t motion, int32_t complexity);
	[[nodiscard]] Key key() const { return {recipe_.keyRoot, recipe_.scale}; }
	/// The step a part's draws key on: `step`, or while the part is frozen, the same step of the phrase it froze in.
	[[nodiscard]] int32_t heldStep(int32_t partIndex, int32_t step) const;
	/// Unfreezes a part: each of its patterns takes the pick that came while it was frozen, if one did.
	void thaw(int32_t partIndex);
	/// At a bar line: whether the bar plays an Answer, and which, from the live Complexity (spec §5.2).
	void updateAnswer(int32_t partIndex, const Step& step);
	/// What a Loop step plays: its line step's pitch content, or the Answer's read-time transform of the line.
	struct LinePitch {
		int32_t steps = 0; ///< Scale steps from the chord root, the octave included.
		bool accent = false;
		bool slide = false;
	};
	/// @return false if the Answer silences the step.
	[[nodiscard]] bool linePitch(int32_t partIndex, const Pattern& pattern, int32_t patternStep, LinePitch& out) const;
	/// The Chord-graph walk (spec §7.4): on a chord line of a genre with a Chord graph, draws the chord that plays
	/// from step `step` on. Call it on bar lines only, after the Arc enters the step and a key change lands there.
	/// @return The playing chord's degree from this step on.
	int32_t walkChord(int32_t step);
	/// Voices the chord on `root` at the Extension level the Recipe's Complexity gives, within the genre's floor and
	/// ceiling: on bar lines, and as a piece starts.
	void voiceChordAt(int32_t root);
	/// Steps from this step to the harmonic rhythm's next grid line at or after it, 0 on one.
	[[nodiscard]] int32_t stepsToChordLine() const;
	/// A pitch in scale steps from the chord root → snap → Register fold → absolute note (spec §7.5).
	[[nodiscard]] uint8_t resolvePitch(const PartSpec& spec, int32_t steps) const;

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
	/// What the last decided step's Space was made of that a command landing on the stop step could change, so
	/// fallThrows() keeps a stopped instance's Space where that step left it: the Motion drift's walk moved on by after
	/// it, and the macro layer's Space. Beside takes_: the first fills its 2 B of padding, the second costs 4 B with
	/// the padding after it.
	int16_t driftLastMotion_ = 0;
	int16_t lastSpaceMacro_ = 0;
	static_assert(kMacroFull <= INT16_MAX, "driftLastMotion_ holds a Motion in 16 bits");
	static_assert(kMacroLayerRows * INT16_MAX * kMacroFull / kMacroLayerSpan * kMacroLayerDepth / 1000 <= INT16_MAX,
	              "lastSpaceMacro_ holds the macro layer in 16 bits");
	const Genre* genre_ = nullptr; ///< Constant data, never owned.

	int32_t nextStep_ = 0;
	static constexpr int32_t kNever = INT32_MAX;
	/// A landed transition's start step: nothing at or after it is decided. kNever otherwise.
	int32_t stopAt_ = kNever;
	Arc arc_;
	/// Quantised steering waiting for its line. A stop cancels it: every start is fresh (spec §2.6, §13.2).
	PendingList pending_;
	uint32_t refused_ = 0;
	/// The roster's mutes and solos. Performance state (spec §12.5, §13.1).
	MuteSolo mutes_;
	/// Bit p: part p is frozen. Performance state, like mutes (spec §6.7, §13.1).
	uint8_t frozen_ = 0;
	/// Bit p: the Arc silences part p on the last decided step (its gate, Break keeps, the cut, the Gap), unless the
	/// Build roll plays one of its lanes and the cut or the Gap doesn't stop it.
	uint8_t silent_ = 0;
	/// Each part's last landed SetSound token, kNoSound before any in this piece.
	uint8_t landedSound_[kMaxParts] = {};
	/// Fills (spec §8.7). Each an entry of the genre's fills, or -1: the phrase's automatic fill, drawn on its first
	/// step, and the one FILL plays while held. Performance state, lost on stop.
	int8_t autoFill_ = -1;
	int8_t heldFill_ = -1;
	/// The drop-out last drawn, by its index in the genre's drop-outs, or -1 (spec §8.8): it waits before
	/// dropOutFrom_, plays up to dropOutTo_ and has ended from there. Performance state, lost on stop.
	int8_t dropOut_ = -1;
	/// Bit p: pitched part p was in a Rest on the last decided step, so its notes have been ended.
	uint8_t rested_ = 0;
	/// Bit p: a new sound or a key change landed for part p on the step being decided, ending its notes on the line.
	/// Its notes there start on the line, as on a Section's first step (spec §8.5, §10.7, §7.6).
	uint8_t cutParts_ = 0;
	/// A fill covered the last decided step: a phrase line after it gets a Phrase accent (spec §9.5).
	bool filled_ = false;
	SectionKind closeWashBy_ = SectionKind::Break; ///< See closeWashFrom_.
	/// The chord walk (spec §7.4), beside the other byte fields: the different chord before the playing one (-1 for
	/// none, for the A-B-A rule), and the chords since the Section start, the cadence count (1 on a Section's first
	/// chord, counting up to the cadence period). Chords change only on bar lines, which a jump's skipTo() never
	/// crosses, so a jump leaves the walk where it is.
	int8_t previousChord_ = -1;
	uint8_t chordCount_ = 0;
	/// The playing chord, voiced on the last bar line (spec §7.4): the walk's degree is its root, `steps[0]`, and its
	/// Extension level the Recipe's Complexity then, within the genre's floor and ceiling. Chords, Extensions and keys
	/// change only on bar lines (a key change lands on a phrase line), so every part that plays chord tones reads it
	/// as it stands, and a Complexity turn mid-bar reaches the Pad, the Stab, the Arp and the Motif together on the
	/// next one.
	Chord voiced_;
	/// How many of controls_ the roster lays out, at most kMaxControlSets. A byte, beside the other byte fields.
	uint8_t controlCount_ = 0;
	static_assert(kMaxControlSets <= UINT8_MAX, "controlCount_ counts the control sets in a byte");
	PartState parts_[kMaxParts];
	ControlSet controls_[kMaxControlSets];
	/// Close and Wash (spec §11.4): the step they began easing in on, entering a Break or an Outro, or kNever. They
	/// hold until a Reset. `closeWashBy_` (beside the other byte fields, so no padding) is the Section kind whose
	/// moment switches them.
	int32_t closeWashFrom_ = kNever;
	/// The drop-out's steps (spec §8.8): it plays from dropOutFrom_ up to dropOutTo_, and the next may start from
	/// dropOutAfter_, its gap after the last that played.
	int32_t dropOutFrom_ = 0;
	int32_t dropOutTo_ = 0;
	int32_t dropOutAfter_ = 0;
	/// Drift's walk (spec §11.5): the seeded point it last passed, and how far it is towards the next, in Motion units
	/// out of kDriftPointSteps × Motion 25. Motion moves it each step; takes and freeze never touch it.
	uint32_t driftPoint_ = 0;
	int32_t driftTowards_ = 0;
	SoundingNotes sounding_;
	/// Notes held back for late entry (spec §5.6): what gated-out parts and lanes would be playing, and, on a Section
	/// line, what the line cut.
	SoundingNotes held_;
	EventBuffer<kEventBufferSize> events_;
	uint32_t dropped_ = 0;
	uint32_t evicted_ = 0;
};

static_assert(PendingList::kCapacity >= 3 + 4 * kMaxParts,
              "the pending list holds a key change, a force, a transition, and a take, a mute, a solo and a sound per "
              "part at once");

/// H18 (spec §20): the state block stays small. Perform allocates two of them on entry.
static_assert(sizeof(Generator) <= 16 * 1024, "the Generator state block outgrew its budget");

} // namespace deluge::generative
