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

// Transitions in Perform (spec §9.7, §12.9, §12.10). A Regenerate confirmed in the Perform menu runs one:
//   Preloading: a live runner job rolls the pending Recipe and loads the incoming roster into fresh Outputs, each
//               linked into Perform's song in one chunk. The playing roster plays on.
//   Queued:     the incoming Generator block is started with the pending Recipe, and the transition's command waits
//               in the playing core for its line.
//   Committed:  the playing roster plays the transition's Build or Outro.
//   Retiring:   the new roster plays from the start step; the outgoing Outputs ring out and are freed.
// From the confirm until the start step a transition is pending: fact edits, End, take and sound actions are refused.
// Genre changes (M2) and a mid-set LOAD (M3) run the same lifecycle, only with a pending Recipe of their own.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/firmware/perform_song.h"
#include <cstdint>

namespace deluge::generative::firmware {

class Transitions {
public:
	enum class Phase : uint8_t { Idle, Preloading, Queued, Committed, Retiring };
	/// What a transition request did.
	enum class Request : uint8_t {
		Started,   ///< its preload started, or waits for the runner (one Preloading or Queued is cancelled for it)
		Refused,   ///< Committed or Retiring: a third roster doesn't fit (spec §12.9). The caller shows the popup.
		QueueFull, ///< the Queued one it replaces couldn't be cancelled: the steering queue is full (QUEUE FULL)
		Ignored,   ///< nothing to act on (no Perform song, or leaving)
	};

	/// REGENERATE, or GENRE on the genre playing, confirmed with `style` (spec §9.7, §13.6).
	Request regenerate(TransitionStyle style);
	/// CANCEL CHANGE, or a BACK tap while the preload runs: cancels a transition still Preloading or Queued. A PLAY
	/// waiting for the preload starts the roster that plays. @return false if there was none to cancel, or the
	/// steering queue was full (the QUEUE FULL popup is shown).
	bool cancel();

	[[nodiscard]] Phase phase() const { return phase_; }
	/// From the confirm until the start step (spec §12.9): fact edits, End, take and sound actions are refused.
	[[nodiscard]] bool pending() const {
		return phase_ == Phase::Preloading || phase_ == Phase::Queued || phase_ == Phase::Committed;
	}
	/// Whether cancel() would cancel one: Preloading, or Queued with its command not landed (CANCEL CHANGE shows).
	[[nodiscard]] bool cancellable() const;
	/// Whether the preload runs, or waits for the runner: its working animation is up.
	[[nodiscard]] bool preloading() const { return phase_ == Phase::Preloading; }
	/// Counts the new rosters started: the view drops focus to song level when it changes.
	[[nodiscard]] uint32_t starts() const { return starts_; }

	/// PLAY from stop while a transition that commits when loaded (confirmed or stopped while stopped) still preloads:
	/// playback starts when it ends, in Groove with `groove` (spec §12.10). Pressed again while it waits, the wait is
	/// cancelled. @return whether this PLAY was taken here, so the caller starts nothing.
	bool startWhenLoaded(bool groove);
	/// Whether a piece mustn't start yet: a transition that commits when loaded still preloads. MIDI Start waits too.
	[[nodiscard]] bool holdsStart() const { return phase_ == Phase::Preloading && commitOnLoad_; }

	// ---- From the Perform adapter ----

	/// In the tick, at the incoming instance's first decision: the new roster takes over (spec §12.9).
	void started();
	/// In the tick: the outgoing instance has played its last note-off. Its Outputs ring out, a phrase at most.
	void outgoingDrained();
	/// The transport stopped: a transition Queued or Committed commits at once, one Preloading when it ends.
	void playbackEnded();
	/// Perform's song is going: the incoming roster goes with it.
	void forget();

private:
	friend class TransitionJob;
	friend class Pump;

	/// The pump's call: follows the playing core (landed, or dropped by a BACK tap), and ends Retiring once the
	/// outgoing Outputs are freed.
	void service();

	/// The preload ended well, in the job's task.
	void preloaded(const Roster& roster, uint32_t request);
	/// The preload ended otherwise, in the job's task.
	void preloadEnded(uint32_t request);
	/// A Queued transition is cancelled: its command too, and its Outputs are freed. @return false, changing nothing,
	/// if the steering queue had no room for the cancel: the transition would land anyway.
	bool cancelQueued(const char* why);
	/// A PLAY waiting for the preload starts now, if there is one: the preload ended without committing.
	void startWaitingPlay();
	/// The Current Recipe becomes the pending one, with the live macros (spec §12.9).
	void commitRecipe();
	/// Stopped: the pending Recipe and the incoming roster take over at once, and the outgoing Outputs retire.
	void commitStopped();
	/// Takes and sounds still queued are dropped, and a sound still preloading is cancelled (spec §12.9).
	void dropTakesAndSounds();
	void setPhase(Phase phase);

	Phase phase_ = Phase::Idle;
	TransitionStyle style_ = TransitionStyle::Drop;
	/// Counts requests: a job ending for an older one changes nothing.
	uint32_t request_ = 0;
	/// The pending Recipe and its sound picks are rolled (a leave commits it, spec §12.9).
	bool rolled_ = false;
	/// Confirmed while stopped, or a stop came while Preloading: the transition commits when the preload ends.
	bool commitOnLoad_ = false;
	/// PLAY came while that preload runs: playback starts when it ends.
	bool startWhenLoaded_ = false;
	bool startInGroove_ = false;
	/// The playing core's next step when the command was queued: once past it, a command it no longer has was dropped.
	int32_t pushedAt_ = 0;
	/// The swing interval waits for task context after the start step.
	bool swingIntervalDue_ = false;
	uint32_t starts_ = 0;
	/// AudioEngine::audioSampleTimer at the last start step: the outgoing Outputs' cap counts from it.
	uint32_t startedAt_ = 0;
	Roster incoming_;
};

extern Transitions transitions;

} // namespace deluge::generative::firmware
