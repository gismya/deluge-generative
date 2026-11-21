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

#include "generative/firmware/capture_job.h"
#include "generative/compose/compose_driver.h"
#include "generative/core/steering_log.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/blank_song.h"
#include "generative/firmware/compose_job.h"
#include "generative/firmware/compose_song.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/log_header.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/song_file.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/song/song.h"
#include <cstdio>
#include <new>
#include <string_view>

extern void setUIForLoadedSong(Song* song);

namespace deluge::hid::display {
std::string_view getErrorMessage(Error error);
}

namespace deluge::generative::firmware {

namespace {

/// What a build holds beside the song, as Compose's: the Generator block, the driver and the Song adapter.
struct CaptureWork {
	Generator generator;
	compose::ComposeDriver driver;
	ComposeSong writer;
	Roster roster;
};

/// The Capture build (spec §17.7): modal; BACK stops it at once.
class CaptureJob final : public Job, public CaptureHost {
public:
	CaptureJob() : Job(Kind::Modal) {}

	void prepare(SteeringLog& log, perform::LeaveOption keep) {
		log_ = &log;
		keep_ = keep;
		error_ = Error::NONE;
		song_ = 0;
		songs_ = 0;
		saved_ = 0;
		stage_ = perform::CaptureStage::Building;
		building_ = nullptr;
		bars_ = expected_ = 0;
		saveFailed_ = false;
		submitted_ = getSystemTicks();
		for (int32_t i = perform::nextKept(log, keep, -1); i >= 0; i = perform::nextKept(log, keep, i)) {
			songs_++;
		}
	}

	JobOutcome run(Chunks& chunks) override {
		chunks_ = &chunks;
		// No move may be under way while a piece is read (spec §17.4); the log changes no more.
		log_->finishCompaction();
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(CaptureWork));
		if (memory == nullptr) {
			error_ = Error::INSUFFICIENT_RAM;
			return JobOutcome::Failed;
		}
		work_ = new (memory) CaptureWork();
		JobOutcome outcome = JobOutcome::Done;
		for (int32_t i = perform::nextKept(*log_, keep_, -1); i >= 0 && outcome == JobOutcome::Done;
		     i = perform::nextKept(*log_, keep_, i)) {
			outcome = buildPiece(log_->piece(i), chunks);
		}
		freeWork();
		return outcome;
	}

	void finished(JobOutcome outcome) override {
		if (building_ != nullptr) {
			deletePrivateSong(building_); // the song being built is thrown away (spec §17.7)
			building_ = nullptr;
		}
		performAdapter.freeLog();
		// The last saved song in arranger view, or a blank session (spec §17.7): setUIForLoadedSong() picks it.
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		generativeModalUI.jobEnded();
#if ENABLE_SYSEX_TEST
		static constexpr const char* kOutcomes[] = {"done", "cancelled", "failed"};
		TEST_EVENT("gen_capture_end %s songs=%ld saved=%ld ms=%lu error=%d name=%s",
		           kOutcomes[outcome == JobOutcome::Done        ? 0
		                     : outcome == JobOutcome::Cancelled ? 1
		                                                        : 2],
		           static_cast<long>(songs_), static_cast<long>(saved_), msSince(submitted_), static_cast<int>(error_),
		           currentSong->name.isEmpty() ? "-" : currentSong->name.get());
		reportFreeMemory("after_capture");
#endif
		if (outcome == JobOutcome::Failed) {
			// The stock error, and how many songs made it to the card (spec §17.7).
			char text[64];
			char saved[perform::kCaptureTitleChars + 1];
			perform::captureTitle(perform::CaptureStage::Saved, saved_, songs_, saved);
			const Error error = error_ == Error::NONE ? Error::UNSPECIFIED : error_;
			snprintf(text, sizeof(text), "%s\n%s", deluge::hid::display::getErrorMessage(error).data(), saved);
			display->displayPopup(text);
		}
	}

	Error nextSong(ComposeSong& writer, int32_t tick) override {
		// A split (spec §17.5): this song is done; the next starts at `tick` with the state there.
		const Error saved = finishSong();
		if (saved != Error::NONE) {
			return saved;
		}
		if (chunks_->cancelled()) {
			return Error::ABORTED_BY_USER;
		}
		songs_++;
		pieceSong_++;
		Error error = startSong(writer.playingPicks());
		if (error != Error::NONE) {
			return error;
		}
		// The header's params for every part still on its first sound, then each param's newest edit and home.
		Roster first = work_->roster;
		for (int32_t p = 0; p < kMaxParts; p++) {
			if ((writer.rerolledParts() & partBit(p)) != 0) {
				first.parts[p] = PartOutput{};
			}
		}
		restoreSnapshot(*piece_, first, *genre_, *building_);
		writer.restoreEdits(work_->roster, *building_);
		writer.restart(building_, work_->roster, tick);
		return Error::NONE;
	}

	void screen(CaptureScreen& out) const {
		perform::captureTitle(stage_, song_, songs_, out.title);
		out.bars = bars_;
		out.expected = expected_;
	}

private:
	JobOutcome buildPiece(const SteeringLog::Piece& piece, Chunks& chunks) {
		piece_ = &piece;
		const PieceStart& start = piece.start();
		genre_ = start.recipe.genre;
		recipe_ = &start.recipe;
		seed_ = start.seed;
		pieceSong_ = 1;
		SoundPicks picks;
		char path[kMaxSoundPath];
		for (int32_t slot = 0; slot < kPickSlots; slot++) {
			if (piece.pick(slot, path)) {
				(slot < kMaxParts ? picks.part[slot] : picks.lane[slot - kMaxParts]).set(path);
			}
		}
		// The palette the piece played from: the Current Recipe's for its genre, else the genre's read from the card.
		const Palette* palette = &currentRecipe.palette();
		if (currentRecipe.recipe.genre != genre_) {
			error_ = currentRecipe.stagePalette(*genre_);
			if (error_ != Error::NONE) {
				return JobOutcome::Failed;
			}
			palette = &currentRecipe.stagedPalette();
		}
		error_ = startSong(picks);
		if (error_ != Error::NONE) {
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}
		restoreSnapshot(piece, work_->roster, *genre_, *building_);

		CaptureWork& work = *work_;
		work.writer.prepare(building_, work.roster, *genre_);
		work.writer.prepareCapture(picks, *palette, chunks, *this);
		work.driver.beginReplay(work.generator, piece, work.writer);
		const int32_t span = piece.cutAt() >= 0 && (piece.endStep() < 0 || piece.cutAt() < piece.endStep())
		                         ? piece.cutAt()
		                         : piece.endStep();
		expected_ = std::max<int32_t>(span / kStepsPerBar, 1);
		bars_ = 0;
		while (!work.driver.run(1)) {
			if (work.writer.error() != Error::NONE) {
				return stopBuild(work.writer.error());
			}
			const int32_t bars = work.generator.nextStep() / kStepsPerBar;
			if (bars != bars_) {
				bars_ = bars;
				renderUIsForOled();
				if (!sdramReserveFree()) {
					return stopBuild(Error::INSUFFICIENT_RAM); // as a failed save (spec §17.7)
				}
			}
			if (chunks.boundary()) {
				return stopBuild(Error::ABORTED_BY_USER);
			}
		}
		if (work.writer.error() != Error::NONE) {
			return stopBuild(work.writer.error());
		}
#if ENABLE_SYSEX_TEST
		TEST_EVENT("gen_capture_piece songs=%ld notes=%lu nodes=%lu cuts=%lu tracks=%lu dropped=%lu off_scale=%lu",
		           static_cast<long>(work.driver.songs()), static_cast<unsigned long>(work.writer.notes()),
		           static_cast<unsigned long>(work.writer.nodes()), static_cast<unsigned long>(work.writer.cuts()),
		           static_cast<unsigned long>(work.writer.newTracks()),
		           static_cast<unsigned long>(work.writer.droppedSegments()),
		           static_cast<unsigned long>(work.writer.offScale()));
#endif
		error_ = finishSong();
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		return chunks.cancelled() ? JobOutcome::Cancelled : JobOutcome::Done;
	}

	JobOutcome stopBuild(Error error) {
		work_->writer.abandon();
		error_ = error == Error::ABORTED_BY_USER ? Error::NONE : error;
		return error == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
	}

	/// The next song's Outputs, from `picks`, built privately once the song before is freed: one song in RAM (spec
	/// §17.7).
	Error startSong(const SoundPicks& picks) {
		song_++;
		stage_ = perform::CaptureStage::Building;
		if (saved_ > 0) {
			stopForSwap();
			swapInBlankSong([] { deleteUnusedAudioFiles(); });
			showScreen();
		}
		renderUIsForOled();
		Song* song = nullptr;
		const Error error = buildPerformSong(*recipe_, picks, picksPalette(), &song, &work_->roster, *chunks_);
		if (error != Error::NONE) {
			return error;
		}
		building_ = song;
		return Error::NONE;
	}

	[[nodiscard]] const Palette& picksPalette() const {
		return currentRecipe.recipe.genre == genre_ ? currentRecipe.palette() : currentRecipe.stagedPalette();
	}

	/// The song built is swapped in stopped, named and saved (spec §17.7). A failed save leaves it current, unsaved.
	Error finishSong() {
		char name[perform::kCaptureNameChars + 1];
		perform::captureSongName(*genre_, seed_, pieceSong_, name);
		building_->name.set(name);
		stopForSwap();
		deleteCurrentSong();
		swapInSong(building_);
		building_ = nullptr;
		showScreen();
		stage_ = perform::CaptureStage::Saving;
		renderUIsForOled();
		char saved[kMaxSongFileName + 1];
		const Error error = saveCurrentSong(name, saved); // a save once started finishes, BACK or not
		TEST_EVENT("gen_capture_song n=%ld of=%ld error=%d name=%s", static_cast<long>(song_),
		           static_cast<long>(songs_), static_cast<int>(error), error == Error::NONE ? saved : name);
		if (error != Error::NONE) {
			saveFailed_ = true;
			return error;
		}
		saved_++;
		return Error::NONE;
	}

	/// After a swap the UI goes to the new song at once, as Clear song's does, which closes the Capture screen: it
	/// opens again over it unless BACK stopped Capture.
	void showScreen() {
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		if (!chunks_->cancelled()) {
			generativeModalUI.openCapture(*this);
		}
	}

	void freeWork() {
		if (work_ != nullptr) {
			work_->writer.abandon();
			work_->~CaptureWork();
			delugeDealloc(work_);
			work_ = nullptr;
		}
	}

	SteeringLog* log_ = nullptr;
	perform::LeaveOption keep_ = perform::LeaveOption::Nothing;
	CaptureWork* work_ = nullptr;
	Chunks* chunks_ = nullptr;
	const SteeringLog::Piece* piece_ = nullptr;
	const Genre* genre_ = nullptr;
	const Recipe* recipe_ = nullptr;
	uint32_t seed_ = 0;
	Song* building_ = nullptr; ///< The private song being built, not yet swapped in
	Error error_ = Error::NONE;
	int32_t song_ = 0;      ///< The song being built or saved, from 1
	int32_t songs_ = 0;     ///< Songs in all: a piece each, and one more at each split
	int32_t pieceSong_ = 1; ///< The song's number in its piece: LIVE, LIVE 2…
	int32_t saved_ = 0;
	int32_t bars_ = 0;
	int32_t expected_ = 0;
	perform::CaptureStage stage_ = perform::CaptureStage::Building;
	uint32_t submitted_ = 0;
	bool saveFailed_ = false;
};

CaptureJob job;

} // namespace

void beginCapture(SteeringLog& log, perform::LeaveOption keep) {
	job.prepare(log, keep);
	generativeModalUI.openCapture(job);
	if (!runner.submit(job)) {
		generativeModalUI.jobEnded(); // only if it is already queued: no Capture runs twice
		performAdapter.freeLog();
	}
	TEST_EVENT("gen_capture_begin keep=%d", static_cast<int>(keep));
}

void captureScreen(CaptureScreen& out) {
	job.screen(out);
}

} // namespace deluge::generative::firmware
