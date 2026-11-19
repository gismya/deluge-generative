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

#include "generative/firmware/compose_job.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/compose/compose_driver.h"
#include "generative/compose/song_size.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/blank_song.h"
#include "generative/firmware/compose_song.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "generative/perform/clip_page.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "memory/memory_region.h"
#include "model/action/action_logger.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <cstdio>
#include <new>

extern void setUIForLoadedSong(Song* song);

namespace deluge::generative::firmware {

namespace {

/// Free bytes in `region`'s empty spaces, counted until `enough`: the reserve check stops as soon as it has it.
uint32_t freeIn(int32_t region, uint32_t enough = UINT32_MAX) {
	auto& spaces = GeneralMemoryAllocator::get().regions[region].emptySpaces;
	uint32_t total = 0;
	for (int32_t i = 0; i < spaces.getNumElements() && total < enough; i++) {
		total += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(i))->length;
	}
	return total;
}

/// Free SDRAM, counted until `enough`: the general regions, then the stealable one, which a song's allocations spill
/// into when the general ones are full. Stealable sample caches aren't counted, though a build may steal them.
uint32_t freeSdram(uint32_t enough = UINT32_MAX) {
	uint32_t total = 0;
	for (int32_t region : {MEMORY_REGION_EXTERNAL, MEMORY_REGION_EXTERNAL_SMALL, MEMORY_REGION_STEALABLE}) {
		total += freeIn(region, enough - total);
		if (total >= enough) {
			break;
		}
	}
	return total;
}

/// The general heaps a song's structures come from first: the fast one, then SDRAM's general regions.
int64_t freeGeneral() {
	return int64_t{freeIn(MEMORY_REGION_INTERNAL)} + freeIn(MEMORY_REGION_INTERNAL_SMALL)
	       + freeIn(MEMORY_REGION_EXTERNAL) + freeIn(MEMORY_REGION_EXTERNAL_SMALL);
}

/// Everything a build could use now: the general heaps, then the stealable region they spill into.
int64_t freeBytes() {
	return freeGeneral() + freeIn(MEMORY_REGION_STEALABLE);
}

/// What a build holds beside the song: the Generator block, the driver (its ClipBuilder) and the Song adapter.
struct ComposeWork {
	Generator generator;
	compose::ComposeDriver driver;
	ComposeSong writer;
	Roster roster;
};

/// The Compose build (spec §15.7): modal; BACK cancels it until the swap.
class ComposeJob final : public Job {
public:
	ComposeJob() : Job(Kind::Modal) {}

	/// Before each submit: a job whose task can't start ends Failed without run(), so nothing else resets these.
	void prepare(const Genre* genre, int32_t lengthSeconds, ComposeEnded ended) {
		genre_ = genre;
		seconds_ = lengthSeconds;
		ended_ = ended;
		error_ = Error::NONE;
		tooLong_ = -1;
		alone_ = false;
		ran_ = false;
		progress_ = {};
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		ran_ = true;
		if (genre_ != nullptr) {
			// As a genre pick does (spec §14.1); a later cancel or failure leaves the new roll in place (spec §14.2).
			error_ = currentRecipe.rollFromGenre(*genre_, &chunks);
			if (error_ != Error::NONE) {
				return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
			}
		}
		const Recipe& recipe = currentRecipe.recipe;
		if (!fitMemory(recipe)) {
			return JobOutcome::Failed;
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}

		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(ComposeWork));
		if (memory == nullptr) {
			error_ = Error::INSUFFICIENT_RAM;
			return JobOutcome::Failed;
		}
		work_ = new (memory) ComposeWork();
		Song* song = nullptr;
		int32_t soundsMissing = 0;
		error_ = buildPerformSong(recipe, currentRecipe.sounds, currentRecipe.palette(), &song, &work_->roster, chunks,
		                          &soundsMissing);
		if (error_ != Error::NONE) {
			freeWork();
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}

		setupDone_ = freeGeneral();
		const JobOutcome outcome = render(recipe, song, chunks);
		if (outcome != JobOutcome::Done) {
			work_->writer.abandon();
			deletePrivateSong(song); // the private song goes at once; nothing else changed (spec §14.5)
			freeWork();
			return outcome;
		}
		nameSong(song);
		chunks.commit(); // BACK no longer cancels, and nothing yields from here to the swap's end
		freeWork();
		swapIn(song);
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		// H19 (spec §20): the build's figures against the estimate. used_kb: the general heaps the build took (the
		// song, its Outputs and the render), not the sample cache; setup_kb: the Outputs alone, before the render.
		TEST_EVENT("gen_compose %s ran=%d ms=%lu bars=%ld/%ld est_kb=%ld used_kb=%ld setup_kb=%ld alone=%d",
		           outcomeName(outcome), ran_ ? 1 : 0, msSince(submitted_), static_cast<long>(progress_.bars),
		           static_cast<long>(progress_.expected), static_cast<long>(estimate_ / 1024),
		           static_cast<long>((freeBefore_ - freeAfter_) / 1024),
		           static_cast<long>((freeBefore_ - setupDone_) / 1024), alone_ ? 1 : 0);
		if (outcome == JobOutcome::Done) {
			TEST_EVENT("gen_compose_song notes=%lu nodes=%lu off_scale=%lu name=%s", notes_, nodes_, offScale_,
			           currentSong->name.get());
		}
#endif
		if (ended_ != nullptr) {
			ended_(outcome);
		}
		if (outcome == JobOutcome::Failed) {
			if (tooLong_ >= 0) {
				// BUILD refused before anything changed: the longest LENGTH that fits (spec §14.5).
				char text[24];
				snprintf(text, sizeof(text), l10n::get(l10n::String::STRING_FOR_COMPOSE_TOO_LONG),
				         static_cast<int>(tooLong_ / 60), static_cast<int>(tooLong_ % 60));
				display->displayPopup(text);
				TEST_EVENT("gen_compose_too_long max_s=%ld", static_cast<long>(tooLong_));
			}
			else {
				// Out of memory (the SDRAM reserve) or a card error (spec §15.9).
				display->displayError(error_ == Error::NONE ? Error::UNSPECIFIED : error_);
				TEST_EVENT("gen_compose_failed error=%d", static_cast<int>(error_));
			}
		}
		if (outcome == JobOutcome::Done) {
			setUIForLoadedSong(currentSong); // arranger view (spec §15.7)
			currentUIMode = UI_MODE_NONE;
		}
	}

	[[nodiscard]] ComposeProgress progress() const { return progress_; }

private:
#if ENABLE_SYSEX_TEST
	static const char* outcomeName(JobOutcome outcome) {
		return outcome == JobOutcome::Done ? "done" : outcome == JobOutcome::Cancelled ? "cancelled" : "failed";
	}
#endif

	/// The size estimate against free memory (spec §15.9): false if it doesn't fit even alone. If it fits only alone,
	/// playback ends and the player's song is freed first, with no prompt: DISCARD SONG? was answered already.
	bool fitMemory(const Recipe& recipe) {
		estimate_ = compose::estimateBytes(recipe, seconds_);
		const int64_t needed = estimate_ + kSdramReserveBytes;
		tooLong_ = composeTooLong(recipe, seconds_);
		if (tooLong_ >= 0) {
			return false;
		}
		bool alone = needed > freeBytes();
#if ENABLE_SYSEX_TEST
		alone = alone || TestPort::composeAloneHeld();
#endif
		if (alone) {
			freePlayerSong();
		}
#if ENABLE_SYSEX_TEST
		reportFreeMemory("before_compose");
#endif
		freeBefore_ = freeGeneral();
		freeAfter_ = freeBefore_;
		setupDone_ = freeBefore_;
		return true;
	}

	void freePlayerSong() {
		alone_ = true;
		if (playbackHandler.playbackState) {
			playbackHandler.endPlayback();
		}
		playbackHandler.recording = RecordingMode::OFF;
		swapInBlankSong([] { deleteUnusedAudioFiles(); });
		// As Clear song, the UI goes to the new song at once, which closes COMPOSING: it opens again over it, so BACK
		// still cancels (spec §14.5: a cancel or failure then lands on the blank song).
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		generativeModalUI.openComposing(*this);
		TEST_EVENT("gen_compose_alone");
	}

	/// Steps the core to the song's end, writing it into `song`, a step at a time between the runner's yields. Stops on
	/// BACK, or when the SDRAM reserve is reached (spec §15.9).
	JobOutcome render(const Recipe& recipe, Song* song, Chunks& chunks) {
		ComposeWork& work = *work_;
		const int32_t target = compose::targetSteps(seconds_, recipe.tempo);
		work.writer.prepare(song, work.roster, *recipe.genre);
		work.driver.begin(work.generator, recipe, currentRecipe.seed, currentRecipe.takes, target, work.writer);
		progress_ = {0, compose::expectedBars(recipe, target)};
		while (!work.driver.run(1)) {
			if (work.writer.error() != Error::NONE) {
				error_ = work.writer.error();
				return JobOutcome::Failed;
			}
			const int32_t bars = work.generator.nextStep() / kStepsPerBar;
			if (bars != progress_.bars) {
				progress_.bars = bars;
				renderUIsForOled(); // the COMPOSING bar
				if (!reserveFree()) {
					error_ = Error::INSUFFICIENT_RAM;
					return JobOutcome::Failed;
				}
#if ENABLE_SYSEX_TEST
				if (waitWhileHeld(chunks)) {
					return JobOutcome::Cancelled;
				}
#endif
			}
			if (chunks.boundary()) {
				return JobOutcome::Cancelled;
			}
		}
		if (work.writer.error() != Error::NONE) {
			error_ = work.writer.error();
			return JobOutcome::Failed;
		}
		progress_.bars = work.driver.endTick() / (kStepsPerBar * kTicksPerStep);
		notes_ = work.writer.notes();
		nodes_ = work.writer.nodes();
		offScale_ = work.writer.offScale();
		freeAfter_ = freeGeneral();
#if ENABLE_SYSEX_TEST
		reportFreeMemory("after_compose");
#endif
		return JobOutcome::Done;
	}

	bool reserveFree() {
#if ENABLE_SYSEX_TEST
		if (TestPort::composeReserveHeld()) {
			return false;
		}
#endif
		return freeSdram(kSdramReserveBytes) >= kSdramReserveBytes;
	}

#if ENABLE_SYSEX_TEST
	/// Test hook: while the test port holds Compose ("hold compose"), the build waits here after its first bar, so a
	/// script can press BACK on a private song with notes in it. @return whether it was cancelled meanwhile.
	static bool waitWhileHeld(Chunks& chunks) {
		if (!TestPort::composeHeld()) {
			return false;
		}
		TEST_EVENT("gen_compose_held");
		while (TestPort::composeHeld()) {
			if (chunks.boundary()) {
				return true;
			}
		}
		return false;
	}
#endif

	/// Unsaved but named `GEN <GENRE> <seed code>` (spec §15.7).
	static void nameSong(Song* song) {
		char code[perform::kSeedCodeChars + 1];
		perform::seedCode(currentRecipe.seed, code);
		char name[64];
		snprintf(name, sizeof(name), "GEN %s %s", currentRecipe.recipe.genre->oledName, code);
		song->name.set(name);
	}

	/// Swapped in stopped, whatever the clock (spec §15.7), as Clear song swaps.
	static void swapIn(Song* song) {
		if (playbackHandler.playbackState) {
			playbackHandler.endPlayback();
		}
		playbackHandler.recording = RecordingMode::OFF;
		playbackHandler.setLedStates();
		actionLogger.deleteAllLogs();
		nullifyUIs();
		deleteOldSongBeforeLoadingNew();
		preLoadedSong = song; // only now: a launch event would swap in any non-null preLoadedSong
		playbackHandler.doSongSwap(false);
		audioFileManager.deleteAnyTempRecordedSamplesFromMemory(); // the old song's, as Clear song does
		// The swap leaves its loading mode and animation up for the caller to clear, as Clear song does.
		currentUIMode = UI_MODE_NONE;
		display->removeWorkingAnimation();
	}

	void freeWork() {
		if (work_ != nullptr) {
			work_->~ComposeWork();
			delugeDealloc(work_);
			work_ = nullptr;
		}
	}

	const Genre* genre_ = nullptr;
	int32_t seconds_ = compose::kDefaultLengthSeconds;
	ComposeEnded ended_ = nullptr;
	ComposeWork* work_ = nullptr;
	ComposeProgress progress_;
	Error error_ = Error::NONE;
	int64_t estimate_ = 0;
	int64_t freeBefore_ = 0;
	int64_t freeAfter_ = 0;
	int64_t setupDone_ = 0; ///< Free general heap once the Outputs are built, before the render
	int32_t tooLong_ = -1;
	uint32_t submitted_ = 0;
	unsigned long notes_ = 0;
	unsigned long nodes_ = 0;
	unsigned long offScale_ = 0;
	bool alone_ = false;
	bool ran_ = false;
};

ComposeJob job;

} // namespace

int32_t composeTooLong(const Recipe& recipe, int32_t lengthSeconds) {
	constexpr int64_t kRoom = kEmptyDelugeFreeBytes - kSdramReserveBytes;
	return compose::estimateBytes(recipe, lengthSeconds) <= kRoom ? -1 : compose::longestFitting(recipe, kRoom);
}

bool beginCompose(const Genre* genre, int32_t lengthSeconds, ComposeEnded ended) {
	if (performAdapter.active() || runner.busy() || (genre == nullptr && !currentRecipe.exists)) {
		return false;
	}
	job.prepare(genre, lengthSeconds, ended);
	return runner.submit(job); // only false if the job is already queued, which the busy check rules out
}

const Job& composeJob() {
	return job;
}

ComposeProgress composeProgress() {
	return job.progress();
}

} // namespace deluge::generative::firmware
