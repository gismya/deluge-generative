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

// The sound swaps' adapter in the harness (perform/sound_swap_lifecycle.h): the real lifecycle against a piece's core,
// driver and steering queue, with plain records where the firmware has a job, Outputs and sound picks. An Output is
// the token of the preload that built it (0: the part's first sound). A spec ends a preload itself, with endPreload()
// or failPreload(), as the runner would. For sound_swap_spec, and transition_piece.h's confirms.

#pragma once

#include "generative/perform/sound_swap_lifecycle.h"
#include "harness.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace harness {

struct HarnessSoundSwaps : perform::SoundSwapLifecycle<HarnessSoundSwaps> {
	/// A preload submitted.
	struct Preload {
		int32_t part = 0;
		perform::SoundSource source = perform::SoundSource::Roll;
		uint8_t token = kNoSound;
		std::string chosen;     ///< A Chosen source's path.
		Lane lane = Lane::None; ///< A Chosen source's lane: None for a synth part's preset.
	};

	/// `recipe` is the Current Recipe, whose genre the swaps read.
	HarnessSoundSwaps(perform::PerformDriver& driver, SteeringQueue& queue, const Recipe& recipe)
	    : driver_(driver), queue_(queue), recipe_(recipe) {}

	// ---- What a spec sets ----

	bool entered = true;        ///< Perform is entered.
	bool left = false;          ///< Perform is being left.
	std::function<bool()> held; ///< A transition is pending, when set.
	bool queueHeld = false;     ///< The test port's QUEUE FULL hold.
	bool jobAhead = false;      ///< Another job runs or waits in the runner: a transition's preload, the free job.
	bool submitRefused = false; ///< The runner doesn't take the preload.
	int32_t freeing = 0;        ///< Outputs retired and not freed yet: freeRetired() frees them.

	// ---- What it records ----

	std::vector<Preload> preloads; ///< Each preload the runner took.
	bool running = false;          ///< The newest preload runs.
	int32_t cancelledJobs = 0;
	std::vector<std::string> events;  ///< Each `gen_sound_*` test event's text.
	int32_t playing[kMaxParts] = {};  ///< The Output each part plays.
	int32_t incoming[kMaxParts] = {}; ///< Each part's incoming Output: 0, none.
	std::vector<int32_t> tails;       ///< Outputs retired after their tail: they played.
	std::vector<int32_t> unplayed;    ///< Outputs retired to be freed at once: they never played.
	std::vector<int32_t> rebound;     ///< Each part whose sound controls bound to a new Output.
	int32_t queueFullPopups = 0;

	// ---- The runner's part ----

	/// The preload running ended Done, with its Output: as SoundJob::finished(), whether or not it was cancelled.
	void endPreload() {
		const Preload& preload = newest();
		running = false;
		preloaded(preload.part, preload.token, [&] { incoming[preload.part] = preload.token; });
	}
	/// The preload running was cancelled or failed: it built nothing.
	void failPreload() {
		static_cast<void>(newest());
		running = false;
	}
	/// The free job has freed every retired Output.
	void freeRetired() { freeing = 0; }

	[[nodiscard]] bool has(const std::string& event) const {
		return std::find(events.begin(), events.end(), event) != events.end();
	}

	// ---- The lifecycle's adapter ----

	[[nodiscard]] const Genre* genre() const { return recipe_.genre; }
	[[nodiscard]] bool ready() const { return entered && !left; }
	[[nodiscard]] bool changePending() const { return held && held(); }
	[[nodiscard]] const Generator* playingGenerator() const { return entered ? driver_.playing() : nullptr; }
	[[nodiscard]] SteeringQueue& queue() { return queue_; }
	[[nodiscard]] bool queueHeldFull() const { return queueHeld; }
	[[nodiscard]] bool runnerBusy() const { return jobAhead || running; }
	[[nodiscard]] bool preloadRunning() const { return running; }
	bool submitPreload(int32_t part, perform::SoundSource source, uint8_t token, const SoundChoice* chosen) {
		if (submitRefused) {
			return false;
		}
		preloads.push_back({part, source, token, chosen != nullptr ? std::string(chosen->path.c_str()) : std::string(),
		                    chosen != nullptr ? chosen->lane : Lane::None});
		running = true;
		return true;
	}
	void cancelJob() { cancelledJobs++; } // it unwinds, and ends with failPreload() or, past its commit, endPreload()
	[[nodiscard]] int32_t retirementRoom() const { return perform::kSoundRetirements - freeing; }
	void swapIn(int32_t part) {
		retire(playing[part], tails);
		playing[part] = incoming[part];
		incoming[part] = 0;
	}
	void rebind(int32_t part) { rebound.push_back(part); }
	void dropIncoming(int32_t part) {
		retire(incoming[part], unplayed);
		incoming[part] = 0;
	}
	void showQueueFull() { queueFullPopups++; }
	void traced(const perform::SoundTraced& traced) { events.emplace_back(perform::traceText(traced).text); }

private:
	/// The newest preload. A spec that ends one when none runs fails here, not at random.
	[[nodiscard]] const Preload& newest() const {
		if (!running) {
			throw std::logic_error("endPreload() or failPreload() with no preload running");
		}
		return preloads.back();
	}
	void retire(int32_t output, std::vector<int32_t>& list) {
		list.push_back(output);
		freeing++;
	}

	perform::PerformDriver& driver_;
	SteeringQueue& queue_;
	const Recipe& recipe_;
};

static_assert(perform::SoundSwapAdapter<HarnessSoundSwaps>);

} // namespace harness
