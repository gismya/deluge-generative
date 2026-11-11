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

// Transitions in Perform (spec §9.7, §12.9, §12.10): the firmware side of perform/transition_lifecycle.h, which holds
// the phase and its rules. A Regenerate confirmed in the Perform menu runs one. Here is what needs the firmware: the
// preload job (a live runner job rolls the pending Recipe and loads the incoming roster into fresh Outputs, each
// linked into Perform's song in one chunk), the rosters' Outputs, the Current Recipe, popups and the test events.
// Genre changes (M2) and a mid-set LOAD (M3) run the same lifecycle, only with a pending Recipe of their own.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/firmware/perform_song.h"
#include "generative/perform/transition_lifecycle.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// The lifecycle's adapter in the firmware. It inherits privately: only the calls named here are public; the preload's
/// (loaded(), loadEnded(), newest()), follow() and reset() are for the job, the pump and forget().
class Transitions : private perform::TransitionLifecycle<Transitions> {
public:
	using TransitionLifecycle::Phase;
	using TransitionLifecycle::Request;

	// From the player and the views.
	using TransitionLifecycle::cancel;
	using TransitionLifecycle::cancellable;
	using TransitionLifecycle::holdsStart;
	using TransitionLifecycle::pending;
	using TransitionLifecycle::phase;
	using TransitionLifecycle::preloading;
	using TransitionLifecycle::starts;
	using TransitionLifecycle::startWhenLoaded;
	// From the Perform adapter.
	using TransitionLifecycle::dropped;
	using TransitionLifecycle::landed;
	using TransitionLifecycle::outgoingDrained;
	using TransitionLifecycle::playbackEnded;
	using TransitionLifecycle::started;

	/// REGENERATE, or GENRE on the genre playing, confirmed with `style` (spec §9.7, §13.6).
	Request regenerate(TransitionStyle style) { return request(style); }
	/// Perform's song is going: the incoming roster goes with it.
	void forget();

private:
	friend class perform::TransitionLifecycle<Transitions>;
	friend class TransitionJob;
	friend class Pump;

	/// The pump's call: the swing interval a start step left for task context, then the lifecycle's follow().
	void service();

	/// The preload for `request` ended well, in the job's task: its roster is in `building`.
	void preloaded(uint32_t request);

	// ---- The lifecycle's adapter calls (perform/transition_lifecycle.h says what each does) ----

	[[nodiscard]] perform::PerformDriver& driver();
	[[nodiscard]] SteeringQueue& queue();
	[[nodiscard]] perform::NoteSink& sink();
	[[nodiscard]] bool ready() const;
	[[nodiscard]] bool leaving() const;
	[[nodiscard]] bool transportRunning() const;
	void startPlayback(bool groove);
	[[nodiscard]] int32_t cutGlideTicks() const;
	bool submitPreload(uint32_t request);
	void cancelPreload();
	void preloadEnded();
	void dropQueuedSounds();
	void adoptIncoming();
	Generator& startIncoming(TransitionStyle style);
	void releaseIncoming();
	void showQueueFull();
	void rosterStarts(TransitionStyle style);
	void retireOutgoing(uint32_t cap);
	void commitStopped(uint32_t cap);
	[[nodiscard]] uint32_t now() const;
	[[nodiscard]] uint32_t phraseLength() const;
	[[nodiscard]] bool outgoingFreed() const;
	using Trace = perform::TransitionTrace;
#if ENABLE_SYSEX_TEST
	void phaseChanged(Phase phase);
	void traced(Trace what, TransitionStyle style);
#else
	void phaseChanged(Phase /*phase*/) {}
	void traced(Trace /*what*/, TransitionStyle /*style*/) {}
#endif

	/// The Current Recipe becomes the pending one, with the live macros (spec §12.9).
	void commitRecipe();

	/// The pending Recipe and its sound picks are rolled (a leave commits it, spec §12.9).
	bool rolled_ = false;
	/// The swing interval waits for task context after the start step.
	bool swingIntervalDue_ = false;
	Roster incoming_;
};

extern Transitions transitions;

} // namespace deluge::generative::firmware
