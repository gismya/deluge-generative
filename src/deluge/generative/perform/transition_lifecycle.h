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

// A transition's lifecycle in Perform (spec §9.7, §12.9, §12.10): its phase and the rules that move it. Portable, so
// the harness runs them against the real core and Perform driver.
//   Preloading: the adapter loads the incoming roster. The playing roster plays on.
//   Queued:     the incoming instance is started, and the transition's command waits in the playing core for its line.
//   Committed:  the command has landed: the playing roster plays the transition's Build or Outro.
//   Retiring:   the new roster plays from the start step; the outgoing Outputs ring out and are freed.
// From the confirm until the start step a transition is pending: fact edits, End, take and sound actions are refused.
//
// The transition's adapter is what needs the firmware (spec §2.5): the preload job, the Outputs, the Current Recipe,
// popups. It derives from TransitionLifecycle<Adapter>, so no call is virtual, and has these members, which the
// lifecycle calls:
//
//   Perform's side, as the Perform adapter holds it:
//     PerformDriver& driver();  SteeringQueue& queue();  NoteSink& sink();
//     bool ready() const;             a transition can run: Perform is entered with a genre, and isn't being left
//     bool leaving() const;           Perform is being left
//     bool transportRunning() const;  playback runs, whether or not a piece plays yet
//     void startPlayback(bool groove);  PLAY from stop
//     int32_t cutGlideTicks() const;  the Perform Glide setting's length, for a cut's tempo glide
//   The preload:
//     bool submitPreload(uint32_t request);  starts the preload for `request`, cancelling one that runs. It ends with
//                                     loaded(request) or loadEnded(request). @return false if it couldn't start.
//     void cancelPreload();           the preload running or waiting to run is cancelled: it frees what it built
//     void preloadEnded();            a preload ended without a roster (cancelled or failed), and none follows it
//     void dropQueuedSounds();        at the confirm: sound actions still queued are dropped, and a sound still
//                                     preloading is cancelled and freed (spec §12.9)
//   The incoming roster:
//     void adoptIncoming();           the preload's roster is the incoming one
//     Generator& startIncoming(TransitionStyle);  starts the idle instance with the pending Recipe, in task context
//     void releaseIncoming();         the incoming roster never played: its Outputs are retired, to be freed (the
//                                     free job frees them later). Runs in the tick at a stop when the piece ended by
//                                     itself after the core dropped the command, after driver().cancelIncoming(),
//                                     which then runs in the tick too.
//     void showQueueFull();           the steering queue had no room for a cancel or a loaded transition's command:
//                                     the QUEUE FULL popup. A request's refusal is its caller's to show.
//   The start, in the tick: these free nothing, show nothing and allocate nothing.
//     void rosterStarts(TransitionStyle);  the incoming roster plays from here, with the pending Recipe; the playing
//                                     one is the outgoing one
//     void retireOutgoing(uint32_t cap);  the outgoing roster has drained: it rings out, `cap` at most
//   A stop:
//     void commitStopped(uint32_t cap);  the incoming roster and the pending Recipe take over at once, stopped; the
//                                     roster that played rings out, `cap` at most. Runs in the tick when the piece
//                                     ended by itself (see playbackEnded()).
//   Time, for the outgoing roster's cap, in the adapter's unit (samples):
//     uint32_t now() const;  uint32_t phraseLength() const;
//   The end:
//     bool outgoingFreed() const;     every Output this transition retired has been freed
//   Test events (compiled out of release firmware):
//     void phaseChanged(TransitionPhase);  void traced(TransitionTrace, TransitionStyle);
//
// M2's genre change and M3's LOAD run this lifecycle unchanged: what differs is the pending Recipe the adapter's
// preload makes, and so what startIncoming() and rosterStarts() hand over.

#pragma once

#include "generative/core/generator.h"
#include "generative/core/steering.h"
#include "generative/perform/perform_driver.h"
#include "generative/perform/press_outcome.h"
#include "generative/perform/text.h"
#include <cstdint>

namespace deluge::generative::perform {

enum class TransitionPhase : uint8_t { Idle, Preloading, Queued, Committed, Retiring };

/// What a transition request did.
enum class TransitionRequest : uint8_t {
	Started,   ///< its preload started, or waits for the runner (one Preloading or Queued is cancelled for it)
	Refused,   ///< Committed or Retiring: a third roster doesn't fit (spec §12.9). The caller shows the popup.
	QueueFull, ///< the Queued one it replaces couldn't be cancelled: the steering queue is full. The caller shows it.
	Ignored,   ///< nothing to act on (no Perform song, or leaving)
};

/// A transition request's outcome as a press's, for its popup.
[[nodiscard]] constexpr PressOutcome pressOutcome(TransitionRequest request) {
	switch (request) {
	case TransitionRequest::Started:
		return PressOutcome::Accepted;
	case TransitionRequest::Refused:
		return PressOutcome::ChangePending;
	case TransitionRequest::QueueFull:
		return PressOutcome::QueueFull;
	case TransitionRequest::Ignored:
		return PressOutcome::NothingToDo;
	}
	return PressOutcome::NothingToDo;
}

/// What the lifecycle did, for the test port's `gen_transition` events.
enum class TransitionTrace : uint8_t {
	RequestStarted,
	RequestRefused,
	CancelPreload,
	CancelQueued,
	CancelQueuedRefused,
	CancelReplaced,
	CancelReplacedRefused,
	CancelDropped,
	CommitOnLoad,
	StartStopped,
	StartWhenLoaded,
	StartWhenLoadedOff,
	StartWhenLoadedPlaying,
	StartWhenLoadedAlreadyPlaying,
};

[[nodiscard]] constexpr const char* phaseName(TransitionPhase phase) {
	switch (phase) {
	case TransitionPhase::Idle:
		return "idle";
	case TransitionPhase::Preloading:
		return "preloading";
	case TransitionPhase::Queued:
		return "queued";
	case TransitionPhase::Committed:
		return "committed";
	case TransitionPhase::Retiring:
		return "retiring";
	}
	return "?";
}

/// A `gen_transition` event's text after that prefix.
struct TransitionTraceText {
	char text[48];
};

/// A trace's text in its `gen_transition` event. The two request traces carry `style`, and a refusal `phase` too. Both
/// adapters send this text, so the specs read what the test port sends.
[[nodiscard]] inline TransitionTraceText traceText(TransitionTrace trace, TransitionStyle style,
                                                   TransitionPhase phase) {
	TransitionTraceText buffer;
	Text out(buffer.text);
	switch (trace) {
	case TransitionTrace::RequestStarted:
	case TransitionTrace::RequestRefused:
		out.add("request style=");
		out.add(static_cast<int32_t>(style));
		if (trace == TransitionTrace::RequestStarted) {
			out.add(" started");
		}
		else {
			out.add(" refused phase=");
			out.add(phaseName(phase));
		}
		break;
	case TransitionTrace::CancelPreload:
		out.add("cancel preload");
		break;
	case TransitionTrace::CancelQueued:
		out.add("cancel queued");
		break;
	case TransitionTrace::CancelQueuedRefused:
		out.add("cancel queued refused");
		break;
	case TransitionTrace::CancelReplaced:
		out.add("cancel replaced");
		break;
	case TransitionTrace::CancelReplacedRefused:
		out.add("cancel replaced refused");
		break;
	case TransitionTrace::CancelDropped:
		out.add("cancel dropped");
		break;
	case TransitionTrace::CommitOnLoad:
		out.add("commit_on_load");
		break;
	case TransitionTrace::StartStopped:
		out.add("start stopped");
		break;
	case TransitionTrace::StartWhenLoaded:
		out.add("start_when_loaded");
		break;
	case TransitionTrace::StartWhenLoadedOff:
		out.add("start_when_loaded off");
		break;
	case TransitionTrace::StartWhenLoadedPlaying:
		out.add("start_when_loaded playing");
		break;
	case TransitionTrace::StartWhenLoadedAlreadyPlaying:
		out.add("start_when_loaded already_playing");
		break;
	}
	return buffer;
}

/// The outgoing roster's cap once it has drained (spec §12.9): a phrase after the start step, not after the drain, so
/// what the drain took comes off it.
[[nodiscard]] constexpr uint32_t capAfterDrain(uint32_t phrase, uint32_t sinceStart) {
	return sinceStart < phrase ? phrase - sinceStart : 0;
}

/// Valid when zero-filled, and holds no pointer: the adapter may sit in memory that is only cleared.
template <typename Adapter>
class TransitionLifecycle {
public:
	using Phase = TransitionPhase;
	using Request = TransitionRequest;
	using Trace = TransitionTrace;

	// ---- From the player, in task context ----

	/// A transition confirmed with `style` (spec §9.7, §13.6). One Preloading or Queued is replaced by it. The caller
	/// shows the popup for its result (pressOutcome()).
	Request request(TransitionStyle style);
	/// CANCEL CHANGE, or a BACK tap while the preload runs: cancels a transition still Preloading or Queued. A PLAY
	/// waiting for the preload starts the roster that plays. @return false if there was none to cancel, or the
	/// steering queue was full (the QUEUE FULL popup is shown).
	bool cancel();
	/// PLAY from stop while a transition that commits when loaded (confirmed or stopped while stopped) still preloads:
	/// playback starts when it ends, in Groove with `groove` (spec §12.10). Pressed again while it waits, the wait is
	/// cancelled. @return whether this PLAY was taken here, so the caller starts nothing.
	bool startWhenLoaded(bool groove);

	// ---- Queries ----

	[[nodiscard]] Phase phase() const { return phase_; }
	/// From the confirm until the start step (spec §12.9): fact edits, End, take and sound actions are refused.
	[[nodiscard]] bool pending() const {
		return phase_ == Phase::Preloading || phase_ == Phase::Queued || phase_ == Phase::Committed;
	}
	/// A press of `kind` before it acts: ChangePending if a pending transition refuses it (spec §12.9), else Accepted,
	/// and the press goes on.
	[[nodiscard]] PressOutcome gate(PressKind kind) const { return perform::gate(kind, pending()); }
	/// Whether cancel() would cancel one: Preloading, or Queued with its command not landed (CANCEL CHANGE shows).
	[[nodiscard]] bool cancellable() const { return phase_ == Phase::Preloading || phase_ == Phase::Queued; }
	/// Whether the preload runs, or waits for the runner: its working animation is up.
	[[nodiscard]] bool preloading() const { return phase_ == Phase::Preloading; }
	/// Whether a piece mustn't start yet: a transition that commits when loaded still preloads. MIDI Start waits too.
	[[nodiscard]] bool holdsStart() const { return phase_ == Phase::Preloading && commitOnLoad_; }
	/// Counts the new rosters started: the view drops focus to song level when it changes.
	[[nodiscard]] uint32_t starts() const { return starts_; }
	/// Whether `request` is the newest: a preload for an older one changes nothing when it ends.
	[[nodiscard]] bool newest(uint32_t request) const { return request == request_; }

	// ---- From the preload, in its job's task ----

	/// The preload for `request` ended well. Playing, the incoming instance starts and the transition's command is
	/// queued; stopped, it commits at once. Nothing changes when Perform is gone or being left: what was loaded goes
	/// with Perform's song. @return false, with nothing changed, if `request` is an older one: the caller frees what
	/// it loaded.
	bool loaded(uint32_t request);
	/// The preload for `request` ended otherwise: cancelled or failed. A waiting PLAY starts the roster that plays.
	void loadEnded(uint32_t request);

	// ---- From the Perform driver's sink, in the tick ----

	/// The command landed (NoteSink::transitionLanded()): the transition can't be cancelled now.
	void landed() {
		if (phase_ == Phase::Queued) {
			setPhase(Phase::Committed);
		}
	}
	/// The core dropped the command (NoteSink::transitionDropped()): a BACK tap cancelled the newest action (spec
	/// §13.5). Only a flag: follow() ends the transition in task context, or a stop or cancel() that comes first.
	void dropped() { dropped_ = phase_ == Phase::Queued; }
	/// The incoming instance's first decision (NoteSink::transitionStarts()): the new roster takes over.
	void started();
	/// The outgoing instance has played its last note-off (NoteSink::outgoingDrained()). Its Outputs ring out, up to
	/// a phrase after the start step.
	void outgoingDrained() {
		adapter().retireOutgoing(capAfterDrain(adapter().phraseLength(), adapter().now() - startedAt_));
	}

	// ---- From the transport ----

	/// The transport stopped, and the driver with it: a transition Queued or Committed commits at once, one
	/// Preloading when it has loaded (spec §12.9). One whose command the core has dropped ends instead, as follow()
	/// would end it: the player cancelled it. A stop drops a waiting PLAY.
	/// Usually task context. In the tick when the piece ended by itself: only while Queued, with the command still in
	/// the steering queue when the Outro's end line took its steering, 12 ticks before the line, or dropped by a BACK
	/// tap that drain took with it (or that came earlier, the pump not having followed it yet). The core never ends a
	/// piece whose transition waits in its pending list or has landed.
	void playbackEnded();

	// ---- From the adapter's pump, in task context ----

	/// Ends a transition whose command the core dropped, and Retiring once the outgoing roster is drained and freed.
	/// Nothing when Perform is gone or being left.
	void follow();

	/// Perform's song is going. The adapter forgets the incoming roster, which goes with the song. The request counter
	/// stays: a preload the leave cancelled still ends through loadEnded(), so the adapter's preloadEnded() runs.
	void reset() {
		commitOnLoad_ = false;
		startWhenLoaded_ = false;
		dropped_ = false;
		phase_ = Phase::Idle;
	}

private:
	[[nodiscard]] Adapter& adapter() { return static_cast<Adapter&>(*this); }
	void setPhase(Phase phase) {
		if (phase != phase_) {
			phase_ = phase;
			dropped_ = false;
			adapter().phaseChanged(phase);
		}
	}
	void trace(Trace what) { adapter().traced(what, style_); }
	/// A Queued transition is cancelled: its command too, unless the core has dropped it, and its Outputs are freed.
	/// @return false, changing nothing, if the steering queue had no room for the cancel: the transition would land
	/// anyway. It traces `done`, or `refused`; QUEUE FULL is cancel()'s to show, or request()'s caller's.
	bool cancelQueued(Trace done, Trace refused);
	/// The core's command is gone, cancelled or dropped: the incoming instance and its roster go.
	void dropIncoming() {
		adapter().driver().cancelIncoming(adapter().sink());
		adapter().releaseIncoming();
	}
	/// The core dropped the Queued transition's command: the transition ends, and the roster that plays stays.
	void endDropped() {
		dropIncoming();
		trace(Trace::CancelDropped);
		setPhase(Phase::Idle);
	}
	/// Takes still queued are dropped at the confirm (spec §12.9), each the way a press of it again would.
	void dropQueuedTakes();
	/// Stopped: the pending Recipe and the incoming roster take over at once.
	void commitStopped();
	/// A PLAY waiting for the preload starts now, if there is one.
	void startWaitingPlay();

	Phase phase_ = Phase::Idle;
	TransitionStyle style_ = TransitionStyle::Drop;
	/// Confirmed while stopped, or a stop came while Preloading: the transition commits when the preload ends.
	bool commitOnLoad_ = false;
	/// PLAY came while that preload runs: playback starts when it ends.
	bool startWhenLoaded_ = false;
	bool startInGroove_ = false;
	/// The core dropped the queued command; follow() hasn't ended the transition yet.
	bool dropped_ = false;
	/// Counts requests and cancelled preloads: a job ending for an older one changes nothing.
	uint32_t request_ = 0;
	uint32_t starts_ = 0;
	/// The adapter's now() at the last start step: the outgoing roster's cap counts from it.
	uint32_t startedAt_ = 0;
};

template <typename Adapter>
TransitionRequest TransitionLifecycle<Adapter>::request(TransitionStyle style) {
	if (!adapter().ready()) {
		return Request::Ignored;
	}
	if (phase_ == Phase::Committed || phase_ == Phase::Retiring) {
		adapter().traced(Trace::RequestRefused, style);
		return Request::Refused;
	}
	// A second request during Preloading or Queued replaces the first (spec §12.9).
	if (phase_ == Phase::Queued && !cancelQueued(Trace::CancelReplaced, Trace::CancelReplacedRefused)) {
		return Request::QueueFull;
	}
	dropQueuedTakes();
	adapter().dropQueuedSounds();
	request_++;
	// Confirmed while stopped, it commits when loaded, as a stop during the preload would (spec §12.9): no piece may
	// start on the old roster meanwhile. A PLAY already waiting for an earlier request waits for this one.
	commitOnLoad_ = !adapter().driver().running();
	startWhenLoaded_ = startWhenLoaded_ && commitOnLoad_;
	style_ = style;
	if (!adapter().submitPreload(request_)) {
		setPhase(Phase::Idle);
		return Request::Ignored;
	}
	setPhase(Phase::Preloading);
	trace(Trace::RequestStarted);
	return Request::Started;
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::dropQueuedTakes() {
	const Generator* piece = adapter().driver().playing();
	for (int32_t p = 0; piece != nullptr && p < kMaxParts; p++) {
		if (piece->takeWaiting(p) >= 0) {
			adapter().queue().push({CommandKind::SetTake, static_cast<uint8_t>(p), piece->take(p)});
		}
	}
}

template <typename Adapter>
bool TransitionLifecycle<Adapter>::cancel() {
	if (phase_ == Phase::Preloading) {
		// The UI reacts at once; the job unwinds and frees what it built (spec §12.10).
		request_++;
		adapter().cancelPreload();
		commitOnLoad_ = false;
		trace(Trace::CancelPreload);
		setPhase(Phase::Idle);
		startWaitingPlay();
		return true;
	}
	if (phase_ != Phase::Queued) {
		return false;
	}
	if (!cancelQueued(Trace::CancelQueued, Trace::CancelQueuedRefused)) {
		adapter().showQueueFull();
		return false;
	}
	setPhase(Phase::Idle);
	return true;
}

template <typename Adapter>
bool TransitionLifecycle<Adapter>::cancelQueued(Trace done, Trace refused) {
	// Its command goes before the line's decision can land it: the cancel is drained first (spec §2.6). With no room
	// for it, the transition would land with nothing to start: refused, and it plays on as queued. A command the core
	// has dropped can't land: no cancel, so a full queue refuses nothing. `done` is still traced then ("cancel queued"
	// or "cancel replaced"), though nothing was queued: the test port's text stays.
	if (!dropped_ && !adapter().queue().push({CommandKind::Transition, 0, kCancelTransition})) {
		trace(refused);
		return false;
	}
	dropIncoming();
	trace(done);
	return true;
}

template <typename Adapter>
bool TransitionLifecycle<Adapter>::loaded(uint32_t request) {
	if (!adapter().ready()) {
		return true; // what was loaded goes with Perform's song
	}
	if (request != request_) {
		return false; // cancelled after its last boundary, or replaced
	}
	adapter().adoptIncoming();
	if (commitOnLoad_ || !adapter().driver().running()) {
		// Stopped (spec §12.9): it commits at once, and the next start plays the new roster.
		commitStopped();
		startWaitingPlay(); // the new roster, from its start
		return true;
	}
	// The incoming instance starts in task context with the pending Recipe; its first decision waits for the start
	// step. The command goes to the queue first: once the driver has the instance, it reads a decision that leaves the
	// core without the command as a drop.
	Generator& incoming = adapter().startIncoming(style_);
	if (!adapter().queue().push({CommandKind::Transition, static_cast<uint8_t>(style_), 0})) {
		adapter().releaseIncoming();
		adapter().showQueueFull();
		setPhase(Phase::Idle);
		return true;
	}
	adapter().driver().queueIncoming(incoming, adapter().cutGlideTicks());
	setPhase(Phase::Queued);
	return true;
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::loadEnded(uint32_t request) {
	if (request != request_) {
		return; // a newer request or a cancel has moved on
	}
	adapter().preloadEnded();
	commitOnLoad_ = false;
	setPhase(Phase::Idle);
	startWaitingPlay(); // failed or cancelled: a waiting PLAY starts the roster that plays
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::started() {
	// The new roster plays from the start step (spec §12.9): it starts clean, with the pending Recipe.
	adapter().rosterStarts(style_);
	starts_++;
	startedAt_ = adapter().now();
	setPhase(Phase::Retiring);
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::commitStopped() {
	adapter().commitStopped(adapter().phraseLength());
	commitOnLoad_ = false;
	starts_++;
	trace(Trace::StartStopped);
	setPhase(Phase::Retiring);
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::playbackEnded() {
	switch (phase_) {
	case Phase::Preloading:
		commitOnLoad_ = true;     // Stop commits in any phase (spec §12.9): this one when its preload ends
		startWhenLoaded_ = false; // a stop drops a PLAY waiting from before playback began another way
		trace(Trace::CommitOnLoad);
		break;
	case Phase::Queued:
	case Phase::Committed:
		// The driver has stopped: the incoming instance never plays. A dropped command was the player's cancel, which
		// the pump hasn't followed yet (only Queued has the flag).
		if (dropped_) {
			endDropped();
		}
		else {
			commitStopped();
		}
		break;
	default:
		break;
	}
}

template <typename Adapter>
bool TransitionLifecycle<Adapter>::startWhenLoaded(bool groove) {
	if (!holdsStart()) {
		return false;
	}
	if (startWhenLoaded_) {
		// PLAY again: it stops, so nothing starts when the preload ends.
		startWhenLoaded_ = false;
		trace(Trace::StartWhenLoadedOff);
		return true;
	}
	startWhenLoaded_ = true;
	startInGroove_ = groove;
	trace(Trace::StartWhenLoaded);
	return true;
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::startWaitingPlay() {
	if (!startWhenLoaded_ || adapter().leaving()) {
		startWhenLoaded_ = false;
		return; // leaving drops it: the transport stays stopped
	}
	startWhenLoaded_ = false;
	// As it fires, not as it was armed: a MIDI Start may have begun playback meanwhile, which the tick has held
	// silent. PLAY now would stop it; the tick starts the piece instead (spec §12.9).
	if (adapter().transportRunning()) {
		trace(Trace::StartWhenLoadedAlreadyPlaying);
		return;
	}
	trace(Trace::StartWhenLoadedPlaying);
	adapter().startPlayback(startInGroove_);
}

template <typename Adapter>
void TransitionLifecycle<Adapter>::follow() {
	if (!adapter().ready()) {
		return;
	}
	if (dropped_ && phase_ == Phase::Queued) {
		endDropped();
	}
	if (phase_ == Phase::Retiring && adapter().driver().outgoing() == nullptr && adapter().outgoingFreed()) {
		setPhase(Phase::Idle);
	}
}

} // namespace deluge::generative::perform
