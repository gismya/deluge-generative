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

// The Perform driver's firmware adapter (spec §2.5): it owns the Generator blocks, the steering queue and the
// portable driver for as long as Perform is entered, runs them from the Session tick (F1, spec §12.1), and plays
// their events into the roster's Outputs with sendNote and noteOnPreKitArp.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/generator.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/firmware/perform_song.h"
#include "generative/perform/perform_driver.h"
#include "generative/runner/runner.h"
#include <cstdint>

class Song;

namespace deluge::generative::firmware {

class EntryJob;
class LeaveJob;

/// Plays events into the roster's Outputs, building a ModelStack per call.
class OutputSink final : public perform::NoteSink {
public:
	void play(const Event& event) override;
	/// The safety net after a flush: every voice on every part's Output released. Tails ring out (spec §12.5).
	void allNotesOff();

	Roster roster;
	/// The last Section event played, for the OLED.
	SectionKind section = SectionKind::Intro;
	uint16_t occurrence = 0;
	bool sectionKnown = false;

private:
	void note(const Event& event, bool on);
};

/// Perform entry ended, in the job's task. On Done, Perform's song is current and stopped, and the caller opens the
/// Perform view; otherwise the player's song is untouched, and `error` says why on Failed.
using EntryEnded = void (*)(JobOutcome outcome, Error error);
/// Leaving ended, in the job's task. On Done a new blank song is current, in session view. Otherwise (the job's task
/// couldn't start) Perform's song is still current and Perform carries on.
using LeaveEnded = void (*)(JobOutcome outcome);

class PerformAdapter {
public:
	/// Builds Perform's song from the Current Recipe as a modal runner job (spec §14.3, §12.10): privately while the
	/// player's song plays, then swapped in stopped with the Generator blocks allocated. The Current Recipe is rolled
	/// fresh for `genre` first. A cancel before the swap, or a card error, leaves the player's song untouched.
	/// @return false, with nothing started and the Current Recipe unchanged, if the runner is busy.
	bool beginEntry(const Genre& genre, EntryEnded ended);
	/// LEAVE (spec §13.8): stops the transport and the piece at once, cancels the runner's work, and tears Perform's
	/// song and the Generator blocks down as a runner job of its own, which ends on a new blank song.
	void leave(LeaveEnded ended);
	[[nodiscard]] bool active() const { return generator_ != nullptr; }

	/// F1: called from Session::doTickForward with the swung tick count since playback started.
	/// @return Song ticks until it next needs a call.
	int32_t tick(int32_t swungTick);
	/// The transport stopped: flush and release every note (spec §12.5).
	void playbackEnded();

	/// Steers a macro: sets the Current Recipe's centre at once, and the core's from the next undecided step.
	void setMacro(Macro macro, int32_t units);

	/// Queues a key and scale change to the next phrase line, or an Arc Drop if one lands first (spec §9.7). The
	/// Current Recipe and the song key follow when it lands. @return false if the queue is full (the caller shows the
	/// stock popup).
	bool queueKeyChange(uint8_t keyRoot, Scale scale);
	/// Whether a key change has reached the core and waits for its line. One still in the steering queue doesn't count
	/// until the next step decision drains it.
	[[nodiscard]] bool keyChangePending() const;

	// For the Perform view.
	[[nodiscard]] bool playing() const { return driver_.running(); }
	/// Generator ticks since step 0's grid line, while playing.
	[[nodiscard]] int32_t clock() const { return driver_.clock(); }
	[[nodiscard]] const OutputSink& output() const { return sink_; }

private:
	friend class EntryJob;
	friend class LeaveJob;

	void startPiece(int32_t swungTick);
	void stopPiece();
	Error allocateBlocks();
	void freeBlocks();
	/// Swaps Perform's built song in, stopped (spec §14.3).
	void swapIn(Song* song, const Roster& roster);
	/// Forgets Perform's song, roster and Generator, as the teardown starts.
	void dropSong();

	/// Perform allocates two blocks on entry, because a transition overlaps two instances (spec §2.1, §12.9).
	Generator* blocks_[2] = {};
	Generator* generator_ = nullptr;
	/// Perform's song, from the swap to the leave: the tick plays only while it is current.
	Song* song_ = nullptr;
	/// Between LEAVE and the teardown's start: the tick plays nothing, and a teardown that can't start undoes it.
	bool leaving_ = false;
	SteeringQueue queue_;
	perform::PerformDriver driver_;
	OutputSink sink_;
	int32_t lastSwungTick_ = 0;
};

extern PerformAdapter performAdapter;

} // namespace deluge::generative::firmware
