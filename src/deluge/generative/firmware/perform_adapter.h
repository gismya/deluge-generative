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
	/// Writes the song tempo without logging an undo action (spec §12.7). Never under an external clock, which owns it.
	void setTempo(int32_t tempo) override;
	void setSwing(int32_t amount) override;
	/// After every jump: allNotesOff(), the safety net (spec §12.5).
	void jumped() override { allNotesOff(); }
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
	/// player's song plays, then swapped in stopped with the Generator blocks allocated. The job first rolls a fresh
	/// Current Recipe for `genre` (its sound picks read the card). A cancel before the swap, or a card error, leaves
	/// the player's song untouched, and a card error in the roll leaves the Current Recipe as it was.
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

	// ---- Transport (spec §13.2, §12.4) ----

	/// PLAY from stop: SHIFT held starts the next piece in Groove, with no Intro. Called before playback starts.
	void playPressed(bool shift) { startInGroove_ = shift; }
	/// A song position pointer while following an external clock: the next tick is a jump to `songTick` (spec §12.4).
	void songPositionPointer(int32_t songTick);
	/// End, armed by holding its pad: the Outro from the next phrase line, then the transport stops where it ends.
	void armEnd();
	/// A tap on End: cancels it while it waits.
	void cancelEnd();
	/// Whether End waits for its line. One still in the steering queue doesn't count until the next step decision.
	[[nodiscard]] bool endQueued() const;

	// ---- Tempo and swing (spec §12.7, §13.7) ----

	/// Sets the Recipe tempo (hundredths of a BPM), gliding to it at the Glide setting while playing. Ignored under an
	/// external clock, which owns the tempo.
	void setTempo(int32_t tempo);
	/// Sets the Recipe swing amount (-kMaxSwing to kMaxSwing), gliding to it on step boundaries while playing.
	void setSwing(int32_t amount);
	/// Sets the Recipe swing interval (a note value, 0 the 16th), at once as stock does.
	void setSwingInterval(int32_t interval);
	/// The Perform Glide setting, kept until power-off (spec §13.3). Its menu is #114's.
	perform::GlideLength glide = perform::kDefaultGlide;

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

	void startPiece(int32_t swungTick, int32_t magnitude);
	/// The Glide setting's length in Generator ticks for the genre playing (spec §13.3).
	[[nodiscard]] int32_t glideLengthTicks() const;
	/// Once per bar under an external clock: the Recipe tempo tracks it, rounded to a whole BPM (spec §13.7).
	void sampleExternalTempo();
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
	/// The song's tick magnitude at the last tick: an external clock can change it (spec §12.3).
	int32_t magnitude_ = 0;
	/// Whether an external Start's magnitude matching was running at the last tick.
	bool matchingBefore_ = false;
	bool startInGroove_ = false;
	/// The piece ended at its Outro's end and stopped the transport; nothing starts until playback ends.
	bool pieceEnded_ = false;
	/// A mid-play song position pointer waiting for the next tick, and where the master went.
	bool positionPending_ = false;
	int32_t positionTick_ = 0;
	/// The next step whose decision samples the external clock's tempo: each bar line.
	int32_t nextTempoSample_ = 0;
};

extern PerformAdapter performAdapter;

} // namespace deluge::generative::firmware
