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

// Perform entry and leaving, as modal runner jobs (spec §12.10, §13.8, §14.3): building Perform's song while the
// player's plays on, swapping it in, and tearing it down to a blank song.

#include "OSLikeStuff/scheduler_api.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/blank_song.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/working_animation.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/global_effectable/global_effectable.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <new>
#include <span>

extern void setUIForLoadedSong(Song* song);

namespace deluge::generative::firmware {

#if ENABLE_SYSEX_TEST
namespace {

/// Test hook: while the test port holds entry ("hold entry"), the entry job waits here, at its first boundary, so a
/// script can press BACK at a known point instead of racing the build. @return whether entry was cancelled meanwhile.
bool waitWhileEntryHeld(Chunks& chunks) {
	if (!TestPort::entryHeld()) {
		return false;
	}
	TEST_EVENT("gen_entry_held");
	while (TestPort::entryHeld()) {
		if (chunks.boundary()) {
			TestPort::releaseEntry();
			return true;
		}
	}
	return false;
}

} // namespace
#endif

// ---- Entry and leaving, as runner jobs (spec §12.10) ----

/// Perform entry (spec §14.3): modal; BACK cancels it until the swap.
class EntryJob final : public Job {
public:
	EntryJob() : Job(Kind::Modal) {}

	/// Before each submit: a job whose task can't start ends Failed without run(), so nothing else resets these.
	void prepare(const Genre& genre, EntryEnded ended) {
		genre_ = &genre;
		ended_ = ended;
		error_ = Error::NONE;
		soundsMissing_ = 0;
		ran_ = false;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		ran_ = true;
		// A fresh Recipe for the genre (spec §14.1). Its sound picks read the card, so it rolls here, in the job; on a
		// card error the Current Recipe stays as it was. Once rolled, it stays the Current Recipe: a later cancel or a
		// load error leaves the new roll in place (spec §14.2).
		error_ = currentRecipe.rollFromGenre(*genre_, &chunks);
		if (error_ == Error::ABORTED_BY_USER) {
			return JobOutcome::Cancelled;
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}
#if ENABLE_SYSEX_TEST
		if (waitWhileEntryHeld(chunks)) {
			return JobOutcome::Cancelled;
		}
#endif
		Song* song = nullptr;
		Roster roster;
		error_ = buildPerformSong(currentRecipe.recipe, currentRecipe.sounds, currentRecipe.palette(), &song, &roster,
		                          chunks, &soundsMissing_);
		if (error_ == Error::ABORTED_BY_USER) {
			return JobOutcome::Cancelled;
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		error_ = performAdapter.allocateBlocks();
		if (error_ == Error::NONE && chunks.boundary()) {
			error_ = Error::ABORTED_BY_USER;
		}
		if (error_ != Error::NONE) {
			performAdapter.freeBlocks();
			deletePrivateSong(song);
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}
		chunks.commit(); // BACK no longer cancels, and nothing yields from here to the swap's end
		performAdapter.swapIn(song, roster);
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		// H9 (spec §20): from the OK press to Perform's song swapped in.
		// ran=0: cancelled before its task ran.
		TEST_EVENT("gen_entry %s ran=%d ms=%lu",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           ran_ ? 1 : 0, msSince(submitted_));
#endif
		if (outcome == JobOutcome::Done && soundsMissing_ > 0) {
			// One warning however many sounds fell back: a part on the default synth or a silent lane (spec §10.1).
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
			TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(soundsMissing_));
		}
		if (ended_ != nullptr) {
			// Failed with no error of its own: the task couldn't start.
			ended_(outcome, outcome == JobOutcome::Failed && error_ == Error::NONE ? Error::UNSPECIFIED : error_);
		}
	}

private:
	const Genre* genre_ = nullptr;
	EntryEnded ended_ = nullptr;
	uint32_t submitted_ = 0;
	bool ran_ = false;
	int32_t soundsMissing_ = 0;
	Error error_ = Error::NONE;
};

/// Tearing Perform's song down on leave (spec §12.10, §13.8): modal and never cancelled. With nothing kept it lands
/// on a new blank song in session view, as SHIFT+LOAD → NEW builds it, and the sample cache is empty.
class LeaveJob final : public Job {
public:
	LeaveJob() : Job(Kind::Modal) {}

	/// Before each submit, as EntryJob's.
	void prepare(LeaveEnded ended) {
		ended_ = ended;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& /*chunks*/) override {
#if ENABLE_SYSEX_TEST
		// After every preload the leave cancelled has ended: the leave's animation is still up (spec §12.10).
		TEST_EVENT("gen_leave_teardown working=%d", workingAnimationShown() ? 1 : 0);
#endif
		// MIDI Start or Continue, or a learned PLAY, may have restarted the transport since leave() stopped it.
		if (playbackHandler.playbackState) {
			playbackHandler.endPlayback();
		}
		performAdapter.dropSong();

		// From here this mirrors stock ClearSong::acceptCurrentOption() (gui/context_menu/clear_song.cpp) on its
		// stopped path, landing in session view (spec §13.8).
#if ENABLE_SYSEX_TEST
		const uint32_t started = getSystemTicks();
#endif
		swapInBlankSong([&] {
			// Perform's song is gone: every Output of both rosters.
			performAdapter.freeBlocks();
			deleteUnusedAudioFiles(); // before the blank song exists, so it lands in the same place every time
#if ENABLE_SYSEX_TEST
			deleteCounts_ = getSystemTicks() - started;
#endif
		});
		return JobOutcome::Done;
	}

	[[nodiscard]] bool cancellable() const override { return false; }

	void finished(JobOutcome outcome) override {
		if (outcome != JobOutcome::Done) {
			// Only when the job's task couldn't start: run() never ran, and Perform's song is still whole and
			// current. Perform carries on; the caller says why.
			performAdapter.undoLeave();
			display->removeWorkingAnimation();
			if (ended_ != nullptr) {
				ended_(outcome);
			}
			return;
		}
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		display->removeWorkingAnimation();
#if ENABLE_SYSEX_TEST
		TEST_EVENT("gen_left ms=%lu delete_us=%lu working=%d", msSince(submitted_), ticksToNs(deleteCounts_) / 1000,
		           workingAnimationShown() ? 1 : 0);
		reportFreeMemory("after_leave");
#endif
		if (ended_ != nullptr) {
			ended_(outcome);
		}
	}

private:
	LeaveEnded ended_ = nullptr;
	uint32_t submitted_ = 0;
#if ENABLE_SYSEX_TEST
	uint32_t deleteCounts_ = 0;
#endif
};

namespace {
EntryJob entry;
LeaveJob teardown;
} // namespace

#if ENABLE_SYSEX_TEST
namespace {
/// Raw SDRAM for delayTestGenre()'s copies, so they cost no internal RAM and need no constructor at boot.
struct DelayTestTables {
	alignas(Genre) uint8_t genre[sizeof(Genre)];
	alignas(PartSpec) uint8_t parts[kMaxParts][sizeof(PartSpec)];
	alignas(LaneSpec) uint8_t lanes[kMaxLanesPerPart][sizeof(LaneSpec)];
};
PLACE_SDRAM_BSS DelayTestTables delayTestTables;

/// The test roster of "hold delay_setups" (TestPort::delaySetupsHeld()): `genre` with a dotted-half ping-pong delay on
/// its Bass, a dotted-quarter one on its Drums kit at kit level and an analog quarter triplet on the Snare row (spec
/// §10.6), long notes so a slow tempo reaches the largest buffers (spec §20), and a Break that keeps only its Pad.
/// Built in place each entry. The Recipe then points at this copy, not at the genre in kGenres, so while it plays a
/// Regenerate of the same genre, or anything else that compares genre pointers, sees a genre change: a script under the
/// hold shouldn't rely on those.
const Genre& delayTestGenre(const Genre& genre) {
	Genre& copy = *new (delayTestTables.genre) Genre(genre);
	auto* parts = reinterpret_cast<PartSpec*>(delayTestTables.parts);
	auto* lanes = reinterpret_cast<LaneSpec*>(delayTestTables.lanes);
	const int32_t count = partCount(genre);
	for (int32_t p = 0; p < count; p++) {
		PartSpec& part = *new (&parts[p]) PartSpec(genre.parts[p]);
		if (part.role == Role::Bass) {
			part.delay = {.on = true, .note = DelayNote::Half, .feel = DelayFeel::Dotted, .pingPong = true};
		}
		if (part.role == Role::Drums) {
			part.delay = {.on = true, .note = DelayNote::Quarter, .feel = DelayFeel::Dotted};
			const int32_t laneTotal = laneCount(part);
			for (int32_t l = 0; l < laneTotal; l++) {
				new (&lanes[l]) LaneSpec(part.lanes[l]);
				if (lanes[l].lane == Lane::Snare) {
					lanes[l].delay = {
					    .on = true, .note = DelayNote::Quarter, .feel = DelayFeel::Triplet, .analog = true};
				}
			}
			part.lanes = std::span<const LaneSpec>(lanes, laneTotal);
		}
	}
	copy.parts = std::span<const PartSpec>(parts, count);
	// The Break keeps only the Pad (Acid's Drone), so the Bass and the kit both throw into it, and their two buffers
	// are allocated together (spec §20). Builds keep what they kept.
	copy.arc.buildKeeps = genre.arc.buildKeeps == kSameAsBreakKeeps ? genre.arc.breakKeeps : genre.arc.buildKeeps;
	PartsAndLanes keeps;
	for (int32_t p = 0; p < count; p++) {
		keeps.parts |= genre.parts[p].role == Role::Pad ? static_cast<uint8_t>(1U << p) : 0U;
	}
	copy.arc.breakKeeps = keeps;
	TEST_EVENT("gen_delay_setups genre=%s", genre.name);
	return copy;
}
} // namespace
#endif

bool PerformAdapter::beginEntry(const Genre& genre, EntryEnded ended) {
	if (active() || runner.busy()) {
		return false;
	}
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_entry");
	entry.prepare(TestPort::delaySetupsHeld() ? delayTestGenre(genre) : genre, ended);
#else
	entry.prepare(genre, ended);
#endif
	return runner.submit(entry); // only false if entry is already queued, which the busy check rules out
}

Error PerformAdapter::allocateBlocks() {
	// Two Generator blocks, from the internal heap, once per entry (spec §2.1).
	for (Generator*& block : blocks_) {
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Generator));
		if (memory == nullptr) {
			freeBlocks();
			return Error::INSUFFICIENT_RAM;
		}
		block = new (memory) Generator();
	}
	return Error::NONE;
}

void PerformAdapter::freeBlocks() {
	for (Generator*& block : blocks_) {
		if (block != nullptr) {
			block->~Generator();
			delugeDealloc(block);
			block = nullptr;
		}
	}
}

void PerformAdapter::swapIn(Song* song, const Roster& roster) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
	static_assert(offsetof(PerformAdapter, steering_) == 20, "offset 20 is baked into decideNextStep()'s code");
#pragma GCC diagnostic pop
	// Swap it in stopped (spec §14.3), as Clear song does. Perform keeps the undo log empty from entry (spec §12.9).
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback();
	}
	// RECORD armed in a stock view would stay on, and Perform can't turn it off (F9): disarm it.
	playbackHandler.recording = RecordingMode::OFF;
	playbackHandler.setLedStates();
	actionLogger.deleteAllLogs();
	nullifyUIs();
	deleteOldSongBeforeLoadingNew();
	preLoadedSong = song;
	playbackHandler.doSongSwap(false);
	audioFileManager.deleteAnyTempRecordedSamplesFromMemory(); // the old song's, as Clear song does
	// The swap leaves its loading mode and animation up for the caller to clear, as Clear song does.
	currentUIMode = UI_MODE_NONE;
	display->removeWorkingAnimation();

	sink_ = OutputSink{};
	sink_.roster = roster;
	song_ = song;
	generator_ = blocks_[0];
	steering_.enter(); // an empty queue, and a new Recipe: every take at 0 (spec §6.6)
}

void PerformAdapter::leave(LeaveEnded ended) {
	if (leaving()) {
		return; // the teardown already queued ends with its own callback
	}
	if (!active()) {
		if (ended != nullptr) {
			ended(JobOutcome::Failed);
		}
		return;
	}
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback(); // the view's playbackEnded() stops the piece
	}
	stopPiece();
	steering_.startLeaving(); // the tick plays nothing from here; the teardown drops the song, roster and blocks
	teardown.prepare(ended);
	if (!runner.cancelAllThen(teardown)) {
		// Only if teardown were already queued, which leaving() rules out: Perform carries on.
		steering_.undoLeave();
		if (ended != nullptr) {
			ended(JobOutcome::Failed);
		}
	}
}

const Job& PerformAdapter::entryJob() const {
	return entry;
}

const Job& PerformAdapter::leaveJob() const {
	return teardown;
}

} // namespace deluge::generative::firmware
