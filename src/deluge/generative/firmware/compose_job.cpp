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
#include "generative/core/variants.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/blank_song.h"
#include "generative/firmware/compose_song.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/free_memory.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/working_animation.h"
#include "generative/perform/clip_page.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/sound_editor.h"
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

/// Free SDRAM, counted until `enough`: the general regions, then the stealable one, which a song's allocations spill
/// into when the general ones are full. Stealable sample caches aren't counted, though a build may steal them.
uint32_t freeSdram(uint32_t enough = UINT32_MAX) {
	uint32_t total = 0;
	for (int32_t region : {MEMORY_REGION_EXTERNAL, MEMORY_REGION_EXTERNAL_SMALL, MEMORY_REGION_STEALABLE}) {
		total += freeInRegion(region, enough - total);
		if (total >= enough) {
			break;
		}
	}
	return total;
}

/// The general heaps a song's structures come from first: the fast one, then SDRAM's general regions.
int64_t freeGeneral() {
	return int64_t{freeInternal()} + freeInRegion(MEMORY_REGION_EXTERNAL) + freeInRegion(MEMORY_REGION_EXTERNAL_SMALL);
}

/// Everything a build could use now: the general heaps, then the stealable region they spill into.
int64_t freeBytes() {
	return freeGeneral() + freeInRegion(MEMORY_REGION_STEALABLE);
}

/// What a build holds beside the song: the Generator block, the driver (its ClipBuilder) and the Song adapter.
struct ComposeWork {
	ComposeWork() { generator.useResolvedGenre(&resolved); }
	Generator generator;
	compose::ComposeDriver driver;
	ComposeSong writer;
	Roster roster;
	ResolvedGenre resolved; ///< The genre with the Recipe's variants applied (spec §3.6), beside the block.
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
		soundsMissing_ = 0;
		alone_ = false;
		ran_ = false;
		progress_ = {};
		estimate_ = 0;
#if ENABLE_SYSEX_TEST
		freeBefore_ = freeAfter_ = setupDone_ = 0;
		notes_ = nodes_ = offScale_ = 0;
#endif
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
#if ENABLE_SYSEX_TEST
			waitAfterRoll(chunks);
#endif
		}
		// BACK during the roll's last card read: nothing may change past the roll, least of all the player's song,
		// which fitMemory() frees when the build fits only alone (spec §14.5).
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}
		const Recipe& recipe = currentRecipe.recipe;
		if (!fitMemory(recipe, chunks)) {
			return JobOutcome::Failed;
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled; // after freePlayerSong(), on the blank song (spec §14.5)
		}

		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(ComposeWork));
		if (memory == nullptr) {
			error_ = Error::INSUFFICIENT_RAM;
			return JobOutcome::Failed;
		}
		work_ = new (memory) ComposeWork();
#if ENABLE_SYSEX_TEST
		// H19's baseline, after the work block: used_kb and setup_kb count the song alone.
		reportFreeMemory("before_compose");
		freeBefore_ = freeGeneral();
#endif
		Song* song = nullptr;
		error_ = buildPerformSong(recipe, currentRecipe.sounds, currentRecipe.palette(), &song, &work_->roster, chunks,
		                          &soundsMissing_);
		if (error_ != Error::NONE) {
			freeWork();
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}

#if ENABLE_SYSEX_TEST
		setupDone_ = freeGeneral();
#endif
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
		// song, its Outputs and the render), not the sample cache; setup_kb: the Outputs alone, before the render. Both
		// 0 when the build stopped before them. working: the "Loading" animation is still up (it mustn't be). It
		// proves little when the player's song wasn't freed (alone=0): the stock preset loads in buildPerformSong()
		// take any animation down as a side effect. gen_compose_alone's working= is the check that counts.
		const int64_t used = freeAfter_ != 0 ? freeBefore_ - freeAfter_ : 0;
		const int64_t setup = setupDone_ != 0 ? freeBefore_ - setupDone_ : 0;
		TEST_EVENT("gen_compose %s ran=%d ms=%lu bars=%ld/%ld est_kb=%ld used_kb=%ld setup_kb=%ld alone=%d working=%d",
		           outcomeName(outcome), ran_ ? 1 : 0, msSince(submitted_), static_cast<long>(progress_.bars),
		           static_cast<long>(progress_.expected), static_cast<long>(estimate_ / 1024),
		           static_cast<long>(used / 1024), static_cast<long>(setup / 1024), alone_ ? 1 : 0,
		           workingAnimationShown() ? 1 : 0);
		if (outcome == JobOutcome::Done) {
			TEST_EVENT("gen_compose_song notes=%lu nodes=%lu off_scale=%lu name=%s", notes_, nodes_, offScale_,
			           currentSong->name.get());
		}
#endif
		if (outcome == JobOutcome::Done) {
			// The new song's UI replaces the stack before COMPOSING closes. Closing it first would re-focus what it
			// covers over the swapped-in song: the Compose screen's sound editor (BUILD), whose items still point
			// into the deleted song. Its item's session ends here, as leaving the editor would end it.
			if (isUIOpen(&soundEditor)) {
				soundEditor.getCurrentMenuItem()->endSession();
			}
			setUIForLoadedSong(currentSong); // arranger view (spec §15.7)
			currentUIMode = UI_MODE_NONE;
		}
		generativeModalUI.jobEnded(); // COMPOSING closes here, so no caller can leave it up
		if (ended_ != nullptr) {
			ended_(outcome);
		}
		if (outcome == JobOutcome::Failed) {
			if (tooLong_ >= 0) {
				// BUILD refused before anything changed: the longest LENGTH that fits (spec §14.5).
				showComposeTooLong(tooLong_);
				TEST_EVENT("gen_compose_too_long max_s=%ld", static_cast<long>(tooLong_));
			}
			else {
				// Out of memory (the SDRAM reserve) or a card error (spec §15.9).
				display->displayError(error_ == Error::NONE ? Error::UNSPECIFIED : error_);
				TEST_EVENT("gen_compose_failed error=%d", static_cast<int>(error_));
			}
		}
		if (outcome == JobOutcome::Done) {
			if (soundsMissing_ > 0) {
				// As Perform entry: one warning however many sounds fell back (spec §10.1).
				display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
				TEST_EVENT("gen_compose_sound_missing sounds=%ld", static_cast<long>(soundsMissing_));
			}
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
	bool fitMemory(const Recipe& recipe, Chunks& chunks) {
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
			freePlayerSong(chunks);
		}
		return true;
	}

	void freePlayerSong(const Chunks& chunks) {
		alone_ = true;
		stopForSwap();
		swapInBlankSong([] { deleteUnusedAudioFiles(); });
		// As Clear song, the UI goes to the new song at once, which closes COMPOSING: it opens again over it, so BACK
		// still cancels (spec §14.5: a cancel or failure then lands on the blank song). Not after a BACK in the blank
		// song's preset load: that closed COMPOSING for good.
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		if (!chunks.cancelled()) {
			generativeModalUI.openComposing(*this);
		}
		// working: the swap's "Loading" is still up over COMPOSING (it mustn't be).
		TEST_EVENT("gen_compose_alone working=%d", workingAnimationShown() ? 1 : 0);
	}

	/// Steps the core to the song's end, writing it into `song`, a step at a time between the runner's yields. Stops on
	/// BACK, or when the SDRAM reserve is reached (spec §15.9).
	JobOutcome render(const Recipe& recipe, Song* song, Chunks& chunks) {
		ComposeWork& work = *work_;
		const int32_t target = compose::targetSteps(seconds_, recipe.tempo);
		work.writer.prepare(song, work.roster, *recipe.genre);
		bool skipIntro = false;
#if ENABLE_SYSEX_TEST
		skipIntro = TestPort::goldenHeld(); // the firmware check builds the golden case from the Groove (spec §21.3)
#endif
		work.driver.begin(work.generator, recipe, currentRecipe.seed, currentRecipe.takes, target, work.writer,
		                  skipIntro);
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
#if ENABLE_SYSEX_TEST
		notes_ = work.writer.notes();
		nodes_ = work.writer.nodes();
		offScale_ = work.writer.offScale();
		freeAfter_ = freeGeneral();
		reportFreeMemory("after_compose");
#endif
		return JobOutcome::Done;
	}

	static bool reserveFree() { return sdramReserveFree(); }

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

	/// Test hook: while the test port holds the roll ("hold compose_roll"), the build waits after a genre roll, so a
	/// script can press BACK there. Only the run's own check after it sees the cancel.
	static void waitAfterRoll(Chunks& chunks) {
		if (!TestPort::composeRollHeld()) {
			return;
		}
		TEST_EVENT("gen_compose_roll_held");
		while (TestPort::composeRollHeld() && !chunks.cancelled()) {
			chunks.boundary();
		}
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
		stopForSwap();
		deleteCurrentSong();
		swapInSong(song);
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
	int32_t tooLong_ = -1;
	int32_t soundsMissing_ = 0; ///< Parts on the default synth and silent lanes: one warning on Done (spec §10.1)
	uint32_t submitted_ = 0;
#if ENABLE_SYSEX_TEST
	// H19's figures (spec §20), for the test events only. 0: not reached.
	int64_t freeBefore_ = 0; ///< Free general heap with the work block allocated, before the Outputs
	int64_t freeAfter_ = 0;
	int64_t setupDone_ = 0; ///< Free general heap once the Outputs are built, before the render
	unsigned long notes_ = 0;
	unsigned long nodes_ = 0;
	unsigned long offScale_ = 0;
#endif
	bool alone_ = false;
	bool ran_ = false;
};

ComposeJob job;

} // namespace

bool sdramReserveFree() {
#if ENABLE_SYSEX_TEST
	if (TestPort::composeReserveHeld()) {
		return false;
	}
#endif
	return freeSdram(kSdramReserveBytes) >= kSdramReserveBytes;
}

int32_t composeTooLong(const Recipe& recipe, int32_t lengthSeconds) {
	constexpr int64_t kRoom = kEmptyDelugeFreeBytes - kSdramReserveBytes;
	return compose::estimateBytes(recipe, lengthSeconds) <= kRoom ? -1 : compose::longestFitting(recipe, kRoom);
}

void showComposeTooLong(int32_t longest) {
	if (longest <= 0) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_COMPOSE_TOO_LONG_ANY));
		return;
	}
	// "TOO LONG: MAX 20:00" is 19 characters (LENGTH is at most 20:00); the rest is room for a longer translation.
	char text[32];
	snprintf(text, sizeof(text), l10n::get(l10n::String::STRING_FOR_COMPOSE_TOO_LONG), static_cast<int>(longest / 60),
	         static_cast<int>(longest % 60));
	display->displayPopup(text);
}

ComposeStart beginCompose(const Genre* genre, int32_t lengthSeconds, ComposeEnded ended) {
	if (performAdapter.active() || rootUIIsGenerativePerform()) {
		return ComposeStart::PerformActive;
	}
	if (runner.busy()) {
		return ComposeStart::RunnerBusy;
	}
	if (genre == nullptr && !currentRecipe.exists) {
		return ComposeStart::NoRecipe;
	}
	job.prepare(genre, lengthSeconds, ended);
	generativeModalUI.openComposing(job);
	if (!runner.submit(job)) {
		generativeModalUI.jobEnded(); // only if the job is already queued, which the busy check rules out
		return ComposeStart::RunnerBusy;
	}
	return ComposeStart::Started;
}

ComposeProgress composeProgress() {
	return job.progress();
}

} // namespace deluge::generative::firmware
