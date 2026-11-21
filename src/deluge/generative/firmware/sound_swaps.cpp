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

#include "generative/firmware/sound_swaps.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/compose_edits.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/restart_began.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <climits>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SoundSwaps soundSwaps{};

namespace {

/// Where a preload rolls its picks, the picks of each part's sound waiting for its line, and each part's previous
/// picks (spec §13.4: one previous pick per part). Parts never share a lane, so one SoundPicks holds every part's.
PLACE_SDRAM_BSS SoundPicks stagedPicks;
PLACE_SDRAM_BSS SoundPicks incomingPicks;
PLACE_SDRAM_BSS SoundPicks previousPicks;
/// The sound a Perform browser chose, for the preload it submits (spec §13.5).
PLACE_SDRAM_BSS SoundChoice chosenSound;

/// Copies `part`'s picks, its preset and its lanes' samples, from `from` to `to`.
void copyPartPicks(const Genre& genre, int32_t part, const SoundPicks& from, SoundPicks& to) {
	to.part[part] = from.part[part];
	for (const LaneSpec& lane : genre.parts[part].lanes) {
		const auto index = static_cast<int32_t>(lane.lane);
		to.lane[index] = from.lane[index];
	}
}

} // namespace

/// A sound re-roll's preload (spec §10.7): live, so Perform plays on and works as usual meanwhile (spec §12.10).
class SoundJob final : public Job {
public:
	SoundJob() : Job(Kind::Live) {}

	/// Before each submit. A Chosen source's choice is in `chosenSound`.
	void prepare(int32_t part, SoundSwaps::Source source, uint8_t token) {
		part_ = part;
		source_ = source;
		token_ = token;
		error_ = Error::NONE;
		missing_ = 0;
		built_ = PartOutput{};
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		const Genre* genre = currentRecipe.recipe.genre;
		Song* song = performAdapter.song_;
		if (genre == nullptr || song == nullptr) {
			return JobOutcome::Failed;
		}
#if ENABLE_SYSEX_TEST
		if (TestPort::preloadHeld()) {
			// Test hook: wait here, yielding, so a script's BACK lands while the job runs. A cancel goes on into the
			// roll, whose first folder scan sees it.
			TEST_EVENT("gen_sound_preload_held part=%ld", static_cast<long>(part_));
			TestPort::waitWhilePreloadHeld(chunks);
		}
#endif
		stagedPicks = currentRecipe.sounds;
		if (source_ == SoundSwaps::Source::Previous) {
			copyPartPicks(*genre, part_, previousPicks, stagedPicks);
		}
		else if (source_ == SoundSwaps::Source::Chosen) {
			// The browser's preset or lane file, whether or not the palette lists it. The Kit's other lanes keep
			// theirs.
			if (!chooseSound(*genre, part_, chosenSound, stagedPicks)) {
				return JobOutcome::Failed;
			}
		}
		else {
			// Fresh entropy, never the seed (spec §6.4). A Kit part re-rolls every lane it plays.
			CardFolders folders(&chunks);
			if (!rollPartSounds(*genre, currentRecipe.palette(), part_, stagedPicks, freshEntropy(), folders)) {
				if (folders.cancelled()) {
					return JobOutcome::Cancelled;
				}
				error_ = Error::SD_CARD;
				return JobOutcome::Failed;
			}
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}
		error_ = buildPartOutput(song, *genre, part_, stagedPicks, currentRecipe.palette(), &built_, chunks, &missing_);
		if (error_ == Error::ABORTED_BY_USER) {
			error_ = Error::NONE;
			return JobOutcome::Cancelled; // buildPartOutput() freed what it built
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed; // a card error: the old sound plays on (spec §10.7)
		}
		chunks.commit(); // linked into the song: it swaps in at its line, or is freed if that's cancelled
#if ENABLE_SYSEX_TEST
		if (TestPort::soundCommittedHeld()) {
			// Test hook: wait here, yielding, so a script's confirm finds a preload it can't cancel (#316).
			TEST_EVENT("gen_sound_preload_committed part=%ld", static_cast<long>(part_));
			TestPort::waitWhileSoundCommittedHeld(chunks);
		}
#endif
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		// A job that ended before its commit (failed, cancelled) leaves the hold for the next, unrelated, sound job.
		TestPort::releaseSoundCommitted();
#endif
		soundPreloadEnded();
		TEST_EVENT("gen_sound_preload %s part=%ld source=%s error=%d token=%u ms=%lu working=%d",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<long>(part_), perform::sourceName(source_), static_cast<int>(error_),
		           static_cast<unsigned>(token_), msSince(submitted_), workingAnimationShown() ? 1 : 0);
		if (outcome == JobOutcome::Failed) {
			// The stock card error (spec §10.7); a job whose task couldn't start has no error of its own.
			display->displayError(error_ != Error::NONE ? error_ : Error::UNSPECIFIED);
			return;
		}
		if (outcome != JobOutcome::Done) {
			return;
		}
		// SOUND MISSING only for an Output that is kept: one retired unplayed (a transition pending) never sounds.
		if (soundSwaps.preloadDone(part_, token_, built_, stagedPicks) && missing_ > 0) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
			TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(missing_));
		}
	}

	[[nodiscard]] int32_t part() const { return part_; }

private:
	int32_t part_ = 0;
	SoundSwaps::Source source_ = SoundSwaps::Source::Roll;
	uint8_t token_ = 0;
	Error error_ = Error::NONE;
	int32_t missing_ = 0;
	PartOutput built_;
	uint32_t submitted_ = 0;
};

namespace {
SoundJob soundJob;
} // namespace

bool SoundSwaps::synthPart(int32_t part) {
	const Genre* genre = currentRecipe.recipe.genre;
	return genre != nullptr && generative::synthPart(*genre, part);
}

bool SoundSwaps::cancelPreload() {
	if (!preloadRunning() || runner.cancelling()) {
		return false;
	}
	return runner.cancel();
}

void SoundSwaps::forgetSong() {
	SoundSwapLifecycle::forget();
	for (PartOutput& output : incoming_) {
		output = PartOutput{};
	}
}

void SoundSwaps::swapDropped(int32_t part) {
	performAdapter.driver().logSoundDropped(static_cast<uint8_t>(part));
}

const SoundPicks& SoundSwaps::waitingPicks() {
	return incomingPicks;
}

bool SoundSwaps::preloadDone(int32_t part, uint8_t token, const PartOutput& output, const SoundPicks& picks) {
	// One preload runs at a time, for a part with nothing waiting (a press on a waiting part cancels it), so the
	// part's incoming slot is free.
	return preloaded(part, token, [&] {
		incoming_[part] = output;
		copyPartPicks(*genre(), part, picks, incomingPicks);
	});
}

void SoundSwaps::adoptRestarted(int32_t part, const PartOutput& output, const SoundPicks& picks) {
	// The restart's line dropped every sound waiting for its line (pieceRestarts()), and while stopped a preload swaps
	// in as it ends: the part's incoming slot is free.
	incoming_[part] = output;
	copyPartPicks(*genre(), part, picks, incomingPicks);
	swapIn(part);
	swappedByRestart(part);
}

void SoundSwaps::retire(const PartOutput& output, Retirements::When when) {
	const Genre* current = genre();
	retirements.retire(output, when, Retirements::Owner::Sound,
	                   current != nullptr ? Retirements::phraseSamples(*current) : 0);
}

// ---- The adapter ----

const Genre* SoundSwaps::genre() const {
	return currentRecipe.recipe.genre;
}

bool SoundSwaps::ready() const {
	return performAdapter.active() && !performAdapter.leaving();
}

bool SoundSwaps::changePending() const {
	return transitions.pending();
}

const Generator* SoundSwaps::playingGenerator() const {
	return performAdapter.playingGenerator();
}

SteeringQueue& SoundSwaps::queue() {
	return performAdapter.steering().queue();
}

bool SoundSwaps::queueHeldFull() const {
#if ENABLE_SYSEX_TEST
	return TestPort::queueFullHeld(); // test hook: as if the queue had no room
#else
	return false;
#endif
}

bool SoundSwaps::runnerBusy() const {
	return runner.busy() || restartBegan.holdsSounds();
}

bool SoundSwaps::preloadRunning() const {
	return runner.active() == &soundJob;
}

bool SoundSwaps::submitPreload(int32_t part, Source source, uint8_t token, const SoundChoice* chosen) {
	if (chosen != nullptr) {
		chosenSound = *chosen;
	}
	soundJob.prepare(part, source, token);
	return runner.submit(soundJob);
}

void SoundSwaps::cancelJob() {
	runner.cancel();
}

int32_t SoundSwaps::retirementRoom() const {
	return retirements.room(Retirements::Owner::Sound);
}

void SoundSwaps::swapIn(int32_t part) {
	const Genre& current = *genre();
	Roster& roster = performAdapter.sink_.roster;
	// The old Output had its note-offs on the line (or the stop's). Its tail rings out, a phrase at most.
	retire(roster.parts[part], Retirements::When::AfterTail);
	roster.parts[part] = incoming_[part];
	incoming_[part] = PartOutput{};
	// The Current Recipe keeps the new picks, and the old ones are the part's previous sound (spec §13.4).
	copyPartPicks(current, part, currentRecipe.sounds, previousPicks);
	copyPartPicks(current, part, incomingPicks, currentRecipe.sounds);
	// The Compose screen's previous pick for the part was the one before a re-roll there, two picks back now.
	forgetPreviousPick(current, part);
	swaps_++;
}

void SoundSwaps::rebind(int32_t part) {
	soundControls.rebindPart(performAdapter.sink_.roster, *genre(), part);
}

void SoundSwaps::dropIncoming(int32_t part) {
	retire(incoming_[part], Retirements::When::Now);
	incoming_[part] = PartOutput{};
}

void SoundSwaps::showQueueFull() {
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
}

void SoundSwaps::traced([[maybe_unused]] const perform::SoundTraced& traced) {
#if ENABLE_SYSEX_TEST
	TEST_EVENT("%s", perform::traceText(traced).text);
	if (traced.trace == perform::SoundTrace::Press) {
		reportFreeMemory("before_sound");
	}
#endif
}

} // namespace deluge::generative::firmware
