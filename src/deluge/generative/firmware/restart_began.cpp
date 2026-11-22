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

#include "generative/firmware/restart_began.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/log_header.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/settings_tracker.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_popups.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "playback/playback_handler.h"
#include <bit>
#include <cstring>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS RestartBegan restartBegan{};

namespace {

/// The picks the restart plays: the Current Recipe's, with each changed part's from the header.
PLACE_SDRAM_BSS SoundPicks beganPicks;

/// Whether `path` differs from the header's pick in `slot` (none: an empty path).
bool pickDiffers(const SteeringLog::Piece& piece, int32_t slot, const SoundPath& path) {
	char header[kMaxSoundPath];
	piece.pick(slot, header);
	return std::strcmp(header, path.c_str()) != 0;
}

/// Copies the header's pick in `slot` to `out`: empty where it has none.
void takePick(const SteeringLog::Piece& piece, int32_t slot, SoundPath& out) {
	char header[kMaxSoundPath];
	piece.pick(slot, header);
	out.clear();
	out.set(header);
}

} // namespace

/// AS IT BEGAN's preload (spec §13.2): each changed part's Output from the header's picks, built beside the one playing
/// as a re-roll's is (spec §10.7), live, so Perform plays on meanwhile (spec §12.10).
class BeganJob final : public Job {
public:
	BeganJob() : Job(Kind::Live) {}

	void prepare(uint32_t parts) {
		parts_ = parts;
		missing_ = 0;
		error_ = Error::NONE;
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
			// first build, whose first boundary sees it.
			TEST_EVENT("gen_restart_preload_held parts=%lu", static_cast<unsigned long>(parts_));
			TestPort::waitWhilePreloadHeld(chunks);
		}
#endif
		for (int32_t p = 0; p < partCount(*genre); p++) {
			if ((parts_ & (1U << p)) == 0) {
				continue;
			}
			PartOutput built;
			error_ = buildPartOutput(song, *genre, p, beganPicks, currentRecipe.palette(), &built, chunks, &missing_);
			if (error_ != Error::NONE) {
				freeBuilt(song);
				if (error_ == Error::ABORTED_BY_USER) {
					error_ = Error::NONE;
					return JobOutcome::Cancelled;
				}
				return JobOutcome::Failed; // a card error: the sounds playing play on (spec §10.7)
			}
			restartBegan.built_[p] = built;
			restartBegan.builtParts_ |= 1U << p;
			// Apply any header settings (including sample/wavetable paths, which need task context) onto the rebuilt
			// part alone (never the Song, which stays untouched until the restart's line).
			const SteeringLog& log = performAdapter.steeringLog();
			if (log.newestWithHeader() >= 0) {
				Roster single{};
				single.parts[p] = built;
				restoreSettings(log.newestHeader(), single, *genre, *song, true, false);
			}
			if (chunks.boundary()) {
				freeBuilt(song);
				return JobOutcome::Cancelled;
			}
		}
		chunks.commit(); // every Output is linked in: they swap in on the restart's line, or are freed if it's gone
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
		// working: the animation was up for the whole preload (it comes down just after).
		TEST_EVENT("gen_restart_preload %s parts=%lu error=%d missing=%ld ms=%lu working=%d",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<unsigned long>(parts_), static_cast<int>(error_), static_cast<long>(missing_),
		           msSince(submitted_), workingAnimationShown() ? 1 : 0);
		soundPreloadEnded();
		if (outcome == JobOutcome::Failed) {
			display->displayError(error_ != Error::NONE ? error_ : Error::UNSPECIFIED);
		}
		restartBegan.preloadEnded(outcome);
		if (outcome == JobOutcome::Done && missing_ > 0 && restartBegan.builtParts_ != 0) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
		}
	}

private:
	/// A cancel or a card error: what this run built so far is freed, in the job's task.
	static void freeBuilt(Song* song) {
		for (int32_t p = 0; p < kMaxParts; p++) {
			if ((restartBegan.builtParts_ & (1U << p)) != 0) {
				freePartOutput(song, restartBegan.built_[p]);
			}
		}
		restartBegan.builtParts_ = 0;
	}

	uint32_t parts_ = 0;
	int32_t missing_ = 0;
	Error error_ = Error::NONE;
	uint32_t submitted_ = 0;
};

namespace {
BeganJob beganJob;
} // namespace

bool RestartBegan::preloading() const {
	return runner.active() == &beganJob;
}

bool RestartBegan::cancelPreload() {
	if (!preloading() || runner.cancelling()) {
		return false;
	}
	return runner.cancel();
}

bool RestartBegan::holdsSounds() const {
	const auto& steering = performAdapter.steering();
	return preloading() || (steering.restartAhead() && steering.restartFrom() == perform::RestartFrom::AsItBegan);
}

uint32_t RestartBegan::changedParts(const SteeringLog::Piece& piece) const {
	const Genre& genre = *currentRecipe.recipe.genre;
	const SoundPicks& now = currentRecipe.sounds;
	uint32_t parts = 0;
	for (int32_t p = 0; p < partCount(genre); p++) {
		bool changed = pickDiffers(piece, pickSlot(p), now.part[p]);
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			changed = changed || pickDiffers(piece, pickSlot(lane.lane), now.lane[static_cast<int32_t>(lane.lane)]);
		}
		// A re-roll waiting for its phrase line could swap in before the restart's: the header's sound is built for
		// its part too, and swaps in over it on the line.
		if (changed || soundSwaps.waiting(p)) {
			parts |= 1U << p;
		}
	}
	// An Output edited since the header was written, which the header holds no block for (it was as its preset loaded
	// then, spec §17.2), or whose non-param settings need a rebuild/path reload: built again from its preset.
	const EditedOutputs inHeader = snapshotParts(piece);
	parts |= performAdapter.editedOutputs().parts & ~inHeader.parts;
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (settingsTracker.partNeedsRebuild(piece, p)) {
			parts |= 1U << p;
		}
	}
	return parts;
}

void RestartBegan::press() {
	using perform::PressOutcome;
	PerformAdapter& adapter = performAdapter;
	if (!adapter.active() || adapter.leaving() || currentRecipe.recipe.genre == nullptr) {
		return;
	}
	auto& steering = adapter.steering();
	const bool playing = playbackHandler.playbackState != 0;
	if (builtParts_ != 0 && !preloading() && !steering.restartAhead()) {
		dropBuilt(); // a restart gone before the pump saw it: its Outputs never play
	}
	if (preloading() || (playing && steering.restartAhead())) {
		// Pressed again: the restart ahead, either kind, is cancelled (spec §13.2): its preload, playing or stopped,
		// or the restart queued. PerformAdapter::pressRestart() does both, with RESTART CANCELLED.
		adapter.pressRestart();
		return;
	}
	if (gui::refusedWithPopup(transitions.gate(perform::PressKind::Restart))) {
		TEST_EVENT("gen_restart_press refused");
		return;
	}
	const SteeringLog& log = adapter.steeringLog();
	int32_t at = log.newestWithHeader();
#if ENABLE_SYSEX_TEST
	if (playing && TestPort::logFullHeld()) {
		at = -1; // test hook: as if the log couldn't keep the playing piece's header
	}
#endif
	if (!playing) {
		// The last piece played, if it is the Current Recipe's: a stopped transition, a Regenerate or a genre change
		// make another piece, which hasn't begun yet.
		const bool same = at >= 0 && log.piece(at).start().recipe.genre == currentRecipe.recipe.genre
		                  && log.piece(at).start().seed == currentRecipe.seed;
		if (!same || transitions.playWaits()) {
			TEST_EVENT("gen_restart_press began_stopped_play");
			adapter.pressRestart(); // as PLAY
			return;
		}
	}
	else if (at < 0) {
		// The log couldn't keep the playing piece's header (spec §13.2): refused, with RECORD's popup. Nothing queues.
		TEST_EVENT("gen_restart_press began_log_full");
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOG_FULL));
		return;
	}
	piece_ = at;
	startsStopped_ = !playing;
	const SteeringLog::Piece piece = log.newestHeader(); // the piece `at`: its header stays in RAM (spec §17.2)
	const uint32_t parts = changedParts(piece);
	if (parts == 0) {
		TEST_EVENT("gen_restart_press began parts=0");
		if (startsStopped_) {
			startStopped();
		}
		else {
			queueRestart();
		}
		return;
	}
	// One Output to retire for each changed part: the preloaded one if it never plays, else the one it replaces.
	const auto owed = static_cast<int32_t>(std::popcount(parts));
	if (runner.busy() || owed + soundSwaps.waitingSounds() > retirements.room(Retirements::Owner::Sound)) {
		TEST_EVENT("gen_restart_press busy parts=%lu", static_cast<unsigned long>(parts));
		gui::refusedWithPopup(PressOutcome::Busy);
		return;
	}
	beganPicks = currentRecipe.sounds;
	const Genre& genre = *currentRecipe.recipe.genre;
	for (int32_t p = 0; p < partCount(genre); p++) {
		if ((parts & (1U << p)) == 0) {
			continue;
		}
		takePick(piece, pickSlot(p), beganPicks.part[p]);
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			takePick(piece, pickSlot(lane.lane), beganPicks.lane[static_cast<int32_t>(lane.lane)]);
		}
	}
	beganJob.prepare(parts);
	const bool submitted = pressShowingWork([&] {
		steering.restartPreloads(true); // before the submit: a job whose task can't start ends inside it
		const bool taken = runner.submit(beganJob);
		if (!taken) {
			steering.restartPreloads(false);
		}
		return taken;
	});
	TEST_EVENT("gen_restart_press began_preload parts=%lu stopped=%d submitted=%d", static_cast<unsigned long>(parts),
	           startsStopped_ ? 1 : 0, submitted ? 1 : 0);
	if (!submitted) {
		gui::refusedWithPopup(PressOutcome::Busy);
	}
}

void RestartBegan::preloadEnded(JobOutcome outcome) {
	PerformAdapter& adapter = performAdapter;
	adapter.steering().restartPreloads(false);
	if (outcome != JobOutcome::Done) {
		return; // the job freed what it built
	}
	if (!adapter.active() || adapter.leaving()) {
		builtParts_ = 0; // the Outputs go with Perform's song
		return;
	}
	const bool playing = playbackHandler.playbackState != 0;
	if (startsStopped_ && !playing) {
		startStopped();
	}
	else if (!startsStopped_ && playing) {
		queueRestart();
	}
	else {
		// The transport started or stopped meanwhile: the press is gone.
		TEST_EVENT("gen_restart_dropped transport parts=%lu", static_cast<unsigned long>(builtParts_));
		dropBuilt();
	}
}

void RestartBegan::queueRestart() {
	using perform::PressOutcome;
	const PressOutcome outcome = performAdapter.steering().pressRestart(perform::RestartFrom::AsItBegan);
	TEST_EVENT("gen_restart_press %s", perform::refused(outcome) ? "refused" : "began_queued");
	if (outcome != PressOutcome::Accepted) {
		dropBuilt();
		gui::refusedWithPopup(outcome);
		return;
	}
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_RESTART));
}

void RestartBegan::startStopped() {
	const SteeringLog::Piece piece = performAdapter.steeringLog().newestHeader(); // piece_'s, until the next opens
	restore(piece);
	performAdapter.steering().restoreStopped(piece.start());
	TEST_EVENT("gen_restart as_it_began stopped tempo=%ld energy=%ld", static_cast<long>(currentRecipe.recipe.tempo),
	           static_cast<long>(currentRecipe.recipe.macros[Macro::Energy]));
	performAdapter.startPlayback(false);
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_RESTART));
}

const PieceStart* RestartBegan::lands() {
	if (performAdapter.steering().restartFrom() != perform::RestartFrom::AsItBegan) {
		dropBuilt(); // none: only AS IT BEGAN preloads
		return nullptr;
	}
	const SteeringLog::Piece piece = performAdapter.steeringLog().newestHeader(); // piece_'s, until the next opens
	restore(piece);
	return &piece.start();
}

void RestartBegan::restore(const SteeringLog::Piece& piece) {
	PerformAdapter& adapter = performAdapter;
	const Genre& genre = *currentRecipe.recipe.genre;
	for (int32_t p = 0; p < kMaxParts; p++) {
		if ((builtParts_ & (1U << p)) != 0) {
			soundSwaps.adoptRestarted(p, built_[p], beganPicks);
			built_[p] = PartOutput{};
		}
	}
	builtParts_ = 0;
	// Restore non-param settings first (scalar only in the tick; paths were loaded in BeganJob), then params.
	if (settingsTracker.songNeedsReset(piece)) {
		resetSongSettings(*adapter.song_, true);
	}
	const int32_t settingsWritten = restoreSettings(piece, adapter.sink_.roster, genre, *adapter.song_, false, true);
	const int32_t written = restoreSnapshot(piece, adapter.sink_.roster, genre, *adapter.song_);
	// What the header holds blocks for is as it was edited then; an edited song it holds no block for goes back to
	// how it was built (spec §17.2).
	const EditedOutputs inHeader = snapshotParts(piece);
	if (!inHeader.song && adapter.editedOutputs().song) {
		resetSongParams(*adapter.song_);
	}
	adapter.outputsEdited(inHeader);
	settingsTracker.syncToHeader(piece, adapter.sink_.roster, genre, *adapter.song_);
	TEST_EVENT("gen_restart_snapshot params=%ld settings=%ld", static_cast<long>(written),
	           static_cast<long>(settingsWritten));
}

void RestartBegan::service() {
	if (builtParts_ != 0 && !preloading() && !performAdapter.steering().restartAhead()) {
		TEST_EVENT("gen_restart_dropped parts=%lu", static_cast<unsigned long>(builtParts_));
		dropBuilt();
	}
}

void RestartBegan::playbackEnded() {
	if (!startsStopped_) {
		cancelPreload();
		dropBuilt();
	}
}

void RestartBegan::forgetSong() {
	for (PartOutput& output : built_) {
		output = PartOutput{};
	}
	builtParts_ = 0;
}

void RestartBegan::dropBuilt() {
	const Genre* genre = currentRecipe.recipe.genre;
	for (int32_t p = 0; p < kMaxParts; p++) {
		if ((builtParts_ & (1U << p)) != 0) {
			retirements.retire(built_[p], Retirements::When::Now, Retirements::Owner::Sound,
			                   genre != nullptr ? Retirements::phraseSamples(*genre) : 0);
			built_[p] = PartOutput{};
		}
	}
	builtParts_ = 0;
}

} // namespace deluge::generative::firmware
