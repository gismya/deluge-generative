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

// Sound re-rolls in Perform (spec §10.7, §13.4, §13.5): the firmware's adapter for perform::SoundSwapLifecycle, which
// decides every press, token and drop. NEW SOUND, SHIFT for the previous sound, or a preset chosen in the preset
// browser preloads the focused part's new Output as a live runner job (SoundJob) while the old one plays; the core's
// Sound event swaps it into the roster on its line, and the old Output retires: it is freed once silent
// (partSilent()), or a phrase after the swap at the latest (spec §12.9), by another live job. Here: the job, the
// Outputs, the sound picks, the sound controls' rebind, popups and test events.

#pragma once

#include "generative/core/event.h"
#include "generative/core/recipe.h"
#include "generative/firmware/perform_song.h"
#include "generative/firmware/retirements.h"
#include "generative/perform/press_outcome.h"
#include "generative/perform/sound_swap_lifecycle.h"
#include <cstdint>

namespace deluge::generative::firmware {

class SoundSwaps : public perform::SoundSwapLifecycle<SoundSwaps> {
public:
	using Press = perform::SoundPress;
	using Source = perform::SoundSource;

	/// Whether `part` is a synth part of the Current Recipe's genre (generative::synthPart()): SYNTH opens the preset
	/// browser on it, and only it takes a chosen sound (spec §13.5).
	[[nodiscard]] static bool synthPart(int32_t part);
	/// A press's outcome, for its popup. NoPrevious has a popup of its own, which the caller shows.
	[[nodiscard]] static constexpr perform::PressOutcome outcome(Press press) { return perform::pressOutcome(press); }
	/// BACK tap: cancels a preload still running, the newest action. @return whether there was one.
	bool cancelPreload();
	/// Perform's song is going: every Output here goes with it, nothing waits, and no part has a previous sound.
	void forgetSong();
	/// Counts the swaps: the view binds the gold knobs again when it changes, so the player's knobs follow the new
	/// sound at once.
	[[nodiscard]] uint32_t swaps() const { return swaps_; }
	/// The picks of each part's sound waiting for its line, from its preload's end until it swaps in: what the
	/// steering log keeps of a re-roll as its SetSound lands (spec §17.2).
	[[nodiscard]] static const SoundPicks& waitingPicks();

	// The adapter (perform::SoundSwapAdapter, which says what each does). Public so the concept sees them; only the
	// lifecycle calls them.
	[[nodiscard]] const Genre* genre() const;
	[[nodiscard]] bool ready() const;
	[[nodiscard]] bool changePending() const;
	[[nodiscard]] const Generator* playingGenerator() const;
	[[nodiscard]] SteeringQueue& queue();
	[[nodiscard]] bool queueHeldFull() const;
	[[nodiscard]] bool runnerBusy() const;
	[[nodiscard]] bool preloadRunning() const;
	bool submitPreload(int32_t part, Source source, uint8_t token, const SoundChoice* chosen);
	void cancelJob();
	[[nodiscard]] int32_t retirementRoom() const;
	void swapIn(int32_t part);
	void rebind(int32_t part);
	void dropIncoming(int32_t part);
	void swapDropped(int32_t part);
	void showQueueFull();
	void traced(const perform::SoundTraced& traced);

private:
	friend class SoundJob;
	friend class Pump;
	/// The pump's call (SoundSwapLifecycle::service()), private to it.
	using perform::SoundSwapLifecycle<SoundSwaps>::service;
	/// Meant to be called only by preloadDone() and forgetSong(), which hold the Outputs the lifecycle names by part.
	using perform::SoundSwapLifecycle<SoundSwaps>::preloaded;
	using perform::SoundSwapLifecycle<SoundSwaps>::forget;

	/// A preload ended well, in the job's task: `output` is `part`'s incoming Output, with `picks`, and the lifecycle
	/// decides (SoundSwapLifecycle::preloaded()). @return whether the Output is kept.
	bool preloadDone(int32_t part, uint8_t token, const PartOutput& output, const SoundPicks& picks);
	/// Queues `output` to be freed: at once, or after its tail, a phrase at most.
	void retire(const PartOutput& output, Retirements::When when);

	/// Each part's preloaded Output, from its preload's end until it swaps in or is dropped.
	PartOutput incoming_[kMaxParts];
	uint32_t swaps_ = 0;
};

static_assert(perform::SoundSwapAdapter<SoundSwaps>);

extern SoundSwaps soundSwaps;

} // namespace deluge::generative::firmware
