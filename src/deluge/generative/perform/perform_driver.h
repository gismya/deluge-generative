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

// The portable Perform driver (spec §2.5): it keeps the clock, decides each step 12 ticks ahead, drains the event
// buffer and emits to a NoteSink. Portable like the core, so the harness runs the real driver logic; the firmware
// adapter (generative/firmware/) plugs in a sink that plays the parts' Outputs.

#pragma once

#include "generative/core/event.h"
#include "generative/core/generator.h"
#include "generative/core/steering.h"
#include "generative/perform/glide.h"
#include "generative/perform/phrase_history.h"
#include <cstdint>

namespace deluge::generative::perform {

/// Where Perform's events go: the firmware adapter's Outputs, or a harness recorder. Muted note-ons never arrive
/// (Perform drops them at the output, spec §12.5).
class NoteSink {
public:
	virtual ~NoteSink() = default;
	virtual void play(const Event& event) = 0;
	/// Sets the song tempo, in hundredths of a BPM: on each tick that changes it while a glide runs (spec §12.7).
	virtual void setTempo(int32_t /*tempo*/) {}
	/// Sets the song swing amount, -kMaxSwing to kMaxSwing: only on a step boundary while playing (spec §12.7).
	virtual void setSwing(int32_t /*amount*/) {}
	/// A jump, of any kind, has just ended every note the core knew of: the place for the safety net (spec §12.5).
	/// Called once per jump, after its note-offs and before anything resumes.
	virtual void jumped() {}

	// ---- A transition (spec §12.9) ----

	/// The new roster plays from here on: play() gets the incoming instance's events, and the outgoing one's go to
	/// playOutgoing(). `startTick` is the start step's tick in the outgoing instance's time; event ticks from the
	/// incoming one count from it. Called at the incoming instance's first decision, 12 ticks before the start step,
	/// before any of its events plays.
	virtual void transitionStarts(int32_t /*startTick*/) {}
	/// An event of the outgoing instance after the start step: its note-offs, hits leaning past it, and its Throws'
	/// Space as they hold and fall (spec §11.4), for the outgoing roster's Outputs.
	virtual void playOutgoing(const Event& event) { play(event); }
	/// The outgoing instance has played everything: its Outputs only ring out now.
	virtual void outgoingDrained() {}
};

/// Times the step decisions inside advance(), for H1 (spec §20). Called only when a decision is due.
class DecisionTimer {
public:
	virtual ~DecisionTimer() = default;
	virtual void decisionsBegin() = 0;
	virtual void decisionsEnd(int32_t decided) = 0;
};

class PerformDriver {
public:
	/// Starts driving `generator`, which start() has just reset, with the master at `songTick` (song ticks at
	/// `magnitude` since its tick 0). Step 0 lands on the master's next bar line, at once from 0 (a Start), and the
	/// driver is silent until then (a Continue or SPP, spec §12.4).
	void begin(Generator& generator, int32_t songTick = 0, int32_t magnitude = kGeneratorTickMagnitude);
	[[nodiscard]] bool running() const { return generator_ != nullptr; }
	/// Whether the last piece stopped by itself, at the end of its Outro, rather than by stop().
	[[nodiscard]] bool ended() const { return ended_; }
	/// Jumps of any kind so far, for a caller that wants to know whether a call jumped.
	[[nodiscard]] uint32_t jumps() const { return jumps_; }

	/// Advances the clock by `songTicks` swung song ticks at tick magnitude `magnitude` (6 << magnitude song ticks
	/// per 16th), and decides every step whose decision is due, playing each event as it falls due, in order. Once
	/// the Outro's last line is reached, the piece is over: every note is released there and the driver goes idle,
	/// with ended() set, so the adapter stops the transport (spec §13.2). A move that is negative or longer than a
	/// bar is a jump (spec §12.3), resuming at the master's next bar line as jumpTo() does.
	/// @return Song ticks until the driver next needs a call, at least 1. Never early: an event plays at the first
	/// call whose clock has reached it (spec §12.3).
	int32_t advance(int32_t songTicks, int32_t magnitude, SteeringQueue& queue, SteeringLog* log, NoteSink& sink);

	/// A mid-play song position pointer: the master is now at `songTick` (song ticks at `magnitude`). Every note
	/// ends at once and nothing plays until the master's next bar line, where the Generator carries on from its own
	/// next bar line. The Arc is never rewound (spec §12.4).
	void jumpTo(int32_t songTick, int32_t magnitude, NoteSink& sink);
	/// A jump where the master's position doesn't move, such as a clock scale change mid-piece: notes end, and the
	/// piece resumes on its own next bar line, which stays on the master's.
	void jump(NoteSink& sink);

	// ---- Tempo and swing (spec §12.7). Driver-side: the core never sees them. ----

	/// Sets the song tempo (hundredths of a BPM) at once, as a piece starts.
	void setTempo(int32_t tempo, NoteSink& sink);
	/// Sets the song swing amount at once, as a piece starts.
	void setSwing(int32_t amount, NoteSink& sink);
	/// Glides the song tempo from where it is now to `tempo` over `ticks` Generator ticks, writing it on each tick
	/// that changes it.
	/// 0 ticks, or a stopped driver, sets it at once.
	void glideTempo(int32_t tempo, int32_t ticks, NoteSink& sink);
	/// Glides the swing amount from where it is now to `amount` over `ticks`, changing it only on step boundaries
	/// (an Instant glide on the next one). A stopped driver sets it at once.
	void glideSwing(int32_t amount, int32_t ticks, NoteSink& sink);

	/// Times the step decisions in each advance(); nullptr for none.
	void setDecisionTimer(DecisionTimer* timer) { timer_ = timer; }

	/// Stop, exit or a jump: flushes the Generator and plays its note-offs. The driver is idle afterwards.
	void stop(NoteSink& sink);

	// ---- A transition (spec §9.7, §12.9) ----

	/// The incoming instance, started with Generator::startIncoming(), waits for the playing one's transition command
	/// (queued by the caller) to land and play its Build or Outro, or to reach the cut line. From the start step it
	/// decides, with the live macros, and the playing one only drains. Tempo glides to the incoming Recipe's across the
	/// Build or Outro, or over `cutGlideTicks` ending on a cut line; swing switches on the start step.
	void queueIncoming(Generator& incoming, int32_t cutGlideTicks);
	/// The transition was cancelled before its command landed. A cut's glide that had started goes back to the
	/// playing Recipe's tempo over the same length.
	void cancelIncoming(NoteSink& sink);
	/// The incoming instance waiting for its start step, or nullptr.
	[[nodiscard]] const Generator* incoming() const { return incoming_; }
	/// The outgoing instance still draining after a start step, or nullptr.
	[[nodiscard]] const Generator* outgoing() const { return outgoing_; }
	/// The instance deciding steps now, or nullptr when stopped.
	[[nodiscard]] const Generator* playing() const { return generator_; }

	/// Generator ticks since step 0's grid line.
	[[nodiscard]] int32_t clock() const { return clock_; }
	/// The phrases heard in this piece, kept from the events sent (spec §2.8). A start forgets them.
	[[nodiscard]] const PhraseHistory& history() const { return history_; }

private:
	/// Moves the clock by `songTicks` at `magnitude`, which setMagnitude() has taken, carrying any remainder. Never
	/// divides (spec §12.3).
	void moveClock(int32_t songTicks, int32_t magnitude);
	/// Takes `magnitude` as the song's, keeping the remainder's share of a Generator tick.
	void setMagnitude(int32_t magnitude);
	/// The master's position in song ticks at the current magnitude, as far as the clock knows it: the clock's bar
	/// lines are the master's.
	[[nodiscard]] int64_t masterTick() const;
	/// Flushes, then has the Generator carry on from its next bar line, landing on the master's next bar line after
	/// `songTick`: the clock is set that many song ticks before it.
	void resumeAt(int64_t songTick, NoteSink& sink);
	/// Sets the clock `songTicks` song ticks before `step`'s grid line.
	void placeBefore(int32_t step, int64_t songTicks);
	/// Writes the song tempo, if it differs from what was last written.
	void writeTempo(int32_t tempo, NoteSink& sink);
	/// Moves the glides on by `ticks` that played, writing the tempo, and the swing on a step boundary.
	void moveGlides(int32_t ticks, NoteSink& sink);
	/// The first step boundary after the clock.
	[[nodiscard]] int32_t nextStepLine() const;
	/// The step boundary at or after `tick`.
	[[nodiscard]] static int32_t stepLineAtOrAfter(int32_t tick);
	/// The bar-line step at or after `step`.
	[[nodiscard]] static int32_t barLineAtOrAfter(int32_t step);
	/// The piece is over (stop, or the Outro's end): glides land on their targets, and the driver goes idle.
	void finish(NoteSink& sink);
	/// Decides every step whose decision tick the clock has reached, playing what falls due before each.
	void decideDue(SteeringQueue& queue, SteeringLog* log, NoteSink& sink);
	/// Plays every buffered event due at or before `upTo`, in order.
	void playDue(NoteSink& sink, int32_t upTo);
	/// Plays the outgoing instance's events due by now, and lets it go once it is drained.
	void drainOutgoing(NoteSink& sink);
	/// Flushes the outgoing instance, if any, and lets it go.
	void flushOutgoing(Generator::FlushFor why, NoteSink& sink);
	/// The incoming instance's first decision is due: it takes over at the start step.
	void switchToIncoming(NoteSink& sink);
	/// Starts the transition's tempo glide once the clock reaches it.
	void glideIntoTransition(NoteSink& sink);
	/// Song ticks at `magnitude` until the next decision or event, at least 1.
	[[nodiscard]] int32_t nextWait(int32_t magnitude) const;

	Generator* generator_ = nullptr;
	DecisionTimer* timer_ = nullptr;
	Generator* incoming_ = nullptr;
	Generator* outgoing_ = nullptr;
	/// The start step's tick in the outgoing instance's time: its events play at their tick less this.
	int32_t outgoingBase_ = 0;
	/// A hard cut's glide length (the Perform Glide setting), at least 1 tick so an Instant one jumps on the line.
	int32_t cutGlideTicks_ = 1;
	/// The step the playing instance's transition landed on, once it has, or -1.
	int32_t transitionFrom_ = -1;
	bool transitionGlide_ = false; ///< The transition's tempo glide has started.
	/// Where the transition's tempo glide runs, in the playing instance's ticks, once known, and its tempo.
	bool tempoWindow_ = false;
	int32_t tempoFrom_ = 0;
	int32_t tempoTo_ = 0;
	int32_t tempoTarget_ = 0;
	PhraseHistory history_;
	bool ended_ = false;
	uint32_t jumps_ = 0;
	/// The end line whose steering was drained without deciding it, or -1.
	int32_t drainedEndLine_ = -1;
	int32_t clock_ = 0;
	/// Song ticks not yet worth a whole Generator tick, when the song runs at a finer magnitude than 24 per 16th.
	int32_t remainder_ = 0;
	/// The magnitude the remainder is counted at.
	int32_t magnitude_ = kGeneratorTickMagnitude;

	Glide tempo_;
	int32_t writtenTempo_ = 0; ///< The song tempo last written.
	Glide swing_;
	/// A glide started since the last advance(): it counts from that call's clock.
	bool tempoStarting_ = false;
	bool swingStarting_ = false;
	/// A swing value is waiting for the step boundary at swingLine_.
	bool swingDue_ = false;
	int32_t swingLine_ = 0;
};

} // namespace deluge::generative::perform
