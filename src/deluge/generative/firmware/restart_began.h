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

// RESTART ▸ AS IT BEGAN (spec §13.2): the piece starts again as it began, from its header in the steering log (spec
// §17.2): the Recipe with its seed and takes, the sound picks and the param snapshot. The parts whose picks changed
// since (a re-roll, a chosen preset) preload the header's first, one after another in one live runner job under the
// working animation. Then, playing, the restart queues to the next bar line as AS IT IS does (PerformSteering), where
// the preloaded Outputs swap in, the snapshot is written back and the Current Recipe becomes the header's; stopped,
// the same happens at once and the piece starts.
//
// The header is read through the log's API (SteeringLog::Piece), so a change to its layout stays in log_header.*.

#pragma once

#include "generative/core/steering_log.h"
#include "generative/core/types.h"
#include "generative/firmware/perform_song.h"
#include "generative/runner/runner.h"
#include <cstdint>

namespace deluge::generative::firmware {

class RestartBegan {
public:
	/// RESTART ▸ AS IT BEGAN, in task context, with its popup. Playing: a restart already ahead is cancelled (RESTART
	/// CANCELLED), as X+PLAY's press again does; refused with CHANGE PENDING while a transition is pending, and BUSY
	/// while the runner is or the Outputs to free are full (spec §12.10); else the changed sounds preload, and the
	/// restart queues once they have (RESTART). A piece whose header the log couldn't keep is refused with LOG FULL.
	/// Stopped: the last piece's start state is restored and it starts, once its sounds have loaded; with no piece of
	/// the Current Recipe logged, it is PLAY.
	void press();
	/// Whether its preload runs (the working animation's rule, BACK's newest action).
	[[nodiscard]] bool preloading() const;
	/// While AS IT BEGAN preloads or its restart is ahead: sound re-rolls and chosen sounds are refused BUSY
	/// (SoundSwaps::runnerBusy()), since one landing before the restart's line would outlive it.
	[[nodiscard]] bool holdsSounds() const;
	/// BACK, X+PLAY or PLAY while its preload runs: it is cancelled, and nothing restarts. @return whether one ran.
	bool cancelPreload();
	/// The restart's line, in the tick, after the first run's flush and its sounds' homes, with the sounds waiting for
	/// their line dropped. For a restart pressed AS IT BEGAN: the preloaded Outputs swap in and the param snapshot is
	/// written back. @return the header's start, for the steering's restartOnLine(), or nullptr for AS IT IS.
	const PieceStart* lands();
	/// The pump: Outputs preloaded for a restart that is no longer ahead (BACK, a press again, a full queue) are freed.
	void service();
	/// The transport stopped: a restart pressed while playing is gone, with its Outputs, and its preload is cancelled.
	void playbackEnded();
	/// Perform's song is going: the Outputs here go with it.
	void forgetSong();

private:
	friend class BeganJob;
	/// The job's end, in its task: Done with every changed part's Output built (`built_`).
	void preloadEnded(JobOutcome outcome);
	/// Which parts' picks differ from `piece`'s header: a bit a part.
	/// Which parts' picks differ from `piece`'s header, or whose re-roll waits for its line: a bit a part.
	[[nodiscard]] uint32_t changedParts(const SteeringLog::Piece& piece) const;
	/// Pushes the restart AS IT BEGAN, with its popup and test event; frees the Outputs if it isn't accepted.
	void queueRestart();
	/// Stopped: the Outputs swap in, the snapshot and the Current Recipe are restored, and the piece starts.
	void startStopped();
	/// Each preloaded Output takes its part's place (SoundSwaps::adoptRestarted()), and the snapshot is written back.
	void restore(const SteeringLog::Piece& piece);
	/// The preloaded Outputs never play: retired to be freed at once.
	void dropBuilt();

	/// Each changed part's Output, from the preload's end until it swaps in or is dropped.
	PartOutput built_[kMaxParts];
	uint32_t builtParts_ = 0;
	/// The log's piece the press restarts from: SteeringLog::newestWithHeader() at the press. Read only for a restart
	/// pressed AS IT BEGAN, so its zero-filled value (SDRAM's BSS) is never read.
	int32_t piece_ = 0;
	/// The press was made stopped: the piece starts once the sounds have loaded.
	bool startsStopped_ = false;
};

extern RestartBegan restartBegan;

} // namespace deluge::generative::firmware
