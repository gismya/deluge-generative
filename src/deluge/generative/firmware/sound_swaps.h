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

// Sound re-rolls in Perform (spec §10.7, §13.4, §13.5): NEW SOUND, or SHIFT for the previous sound, preloads the
// focused part's new Output as a live runner job while the old one plays, then queues SetSound for the next phrase
// line. The core's Sound event swaps it into the roster there, and the old Output retires: it is freed once silent
// (partSilent()), or a phrase after the swap at the latest (spec §12.9), by another live job. A card error or BACK
// cancels the preload and the old sound plays on; pressing the part's sound action again cancels one still waiting.

#pragma once

#include "generative/core/event.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/firmware/perform_song.h"
#include <cstdint>

namespace deluge::generative::firmware {

class SoundSwaps {
public:
	/// What a NEW SOUND or previous-sound press did.
	enum class Press : uint8_t {
		Preloading, ///< the preload started
		Cancelled,  ///< the part's sound was preloading or waiting for its line: pressed again, it's cancelled
		Busy,       ///< the runner is busy: refused (the caller shows the popup, spec §12.10)
		NoPrevious, ///< no previous sound to go back to
		Ignored,    ///< nothing to act on (no Perform song, leaving, or the swap already decided on its line)
	};
	/// The part's NEW SOUND, or with `previous` its previous sound (SHIFT, spec §13.5). Task context.
	Press press(int32_t part, bool previous);
	/// BACK tap: cancels a preload still running, the newest action. @return whether there was one.
	bool cancelPreload();

	/// In the tick: the core's Sound event. The part's preloaded Output takes its place in `roster`, the old one
	/// retires, and the sound controls bind to the new one.
	void swap(const Event& event, Roster& roster);
	/// Task context, a few times a second: notices a waiting sound the core dropped (a BACK tap, a full pending list),
	/// and frees cancelled and retired Outputs once the runner is free.
	void service();
	/// The transport stopped: the core's pending list is gone, so every waiting sound is cancelled.
	void playbackEnded();
	/// Perform's song is going: every Output here goes with it.
	void forget();

	// For the CLIP page and the PART page.
	[[nodiscard]] bool preloading(int32_t part) const;
	/// Whether any part's preload runs: its working animation is up.
	[[nodiscard]] bool preloadRunning() const;
	[[nodiscard]] bool waiting(int32_t part) const { return incoming_[part].token != kNoSound; }
	[[nodiscard]] bool hasPrevious(int32_t part) const { return hasPrevious_[part]; }
	/// Counts the swaps: the view binds the gold knobs again when it changes, before the old Output is freed.
	[[nodiscard]] uint32_t swaps() const { return swaps_; }

private:
	friend class SoundJob;
	friend class FreeJob;

	/// When a retired Output is freed.
	enum class Retire : uint8_t {
		Now, ///< it never played: a cancelled preload
		/// It played: once silent, or a phrase from now at the latest (spec §12.9). The cap cuts it: its 1-beat fade
		/// comes with #115's retiring.
		AfterTail,
	};

	/// A preload ended well, in the job's task: queues the swap for the line, or swaps at once while stopped.
	void preloaded(int32_t part, uint8_t token, const PartOutput& output, const SoundPicks& picks);
	/// Swaps `output` in as `part`'s, with its picks; the old one retires after its tail.
	void swapIn(int32_t part, const PartOutput& output, Roster& roster);
	/// Queues `output` to be freed.
	void retire(const PartOutput& output, Retire when);
	/// Cancels `part`'s sound waiting for its line: its Output is freed.
	void dropIncoming(int32_t part);
	[[nodiscard]] uint8_t nextToken();
	/// How many re-rolls may still end in a retiring Output: one preloading and every part's waiting one.
	[[nodiscard]] int32_t retirementsOwed() const;

	/// A part's preloaded Output, waiting for its line.
	struct Incoming {
		PartOutput output;
		uint8_t token = kNoSound;
		int32_t pushedAt = 0; ///< The core's next step when SetSound was queued: drained once it has moved on.
	};
	Incoming incoming_[kMaxParts];
	/// Outputs to free: retired by a swap, or cancelled before they played.
	struct Retiring {
		PartOutput output;
		Retire when = Retire::Now;
		uint32_t until = 0; ///< AfterTail: the cap, in AudioEngine::audioSampleTimer
	};
	/// Every re-roll ends in exactly one retiring Output: the old one its swap replaces, or its own if cancelled.
	/// press() starts one only while every re-roll in flight (one preloading, one waiting per part) still fits, so
	/// retire() never finds the list full. Each part's retiring Output lasts at most a phrase, and the list leaves
	/// room for a second per part while the runner is busy.
	static constexpr int32_t kMaxRetiring = 2 * kMaxParts;
	static_assert(kMaxRetiring >= kMaxParts + 1, "every re-roll that can be in flight at once fits");
	Retiring retiring_[kMaxRetiring];
	int32_t retiringCount_ = 0;
	bool hasPrevious_[kMaxParts] = {};
	uint8_t lastToken_ = 0;
	uint32_t swaps_ = 0;
};

extern SoundSwaps soundSwaps;

} // namespace deluge::generative::firmware
