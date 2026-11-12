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
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_song.h"
#include "generative/firmware/transitions.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/keyboard_page.h"
#include "generative/perform/perform_driver.h"
#include "generative/perform/perform_steering.h"
#include "generative/perform/press_outcome.h"
#include "generative/runner/runner.h"
#include <cstdint>

class ModelStackWithAutoParam;
class Song;

namespace deluge::generative::firmware {

class EntryJob;
class FreeJob;
class LeaveJob;
class SoundJob;
class SoundSwaps;
class TransitionJob;
class Transitions;

#if ENABLE_SYSEX_TEST
/// Test builds: free bytes on the heaps Perform uses, as a gen_mem event tagged `when`.
void reportFreeMemory(const char* when);
/// Test builds: milliseconds since a getSystemTicks() reading, for the jobs' timing events.
unsigned long msSince(uint32_t start);
#endif

/// Plays events into the roster's Outputs, building a ModelStack per call.
class OutputSink final : public perform::NoteSink {
public:
	void play(const Event& event) override;
	/// Writes the song tempo without logging an undo action (spec §12.7). Never under an external clock, which owns it.
	void setTempo(int32_t tempo) override;
	void setSwing(int32_t amount) override;
	/// After every jump: allNotesOff(), the safety net (spec §12.5).
	void jumped() override { allNotesOff(); }
	/// A sample's length at the system rate, for a lane row that plays one to its end (spec §9.5): 0 for a synth row.
	int32_t sampleMicros(const Event& noteOn) override;
	/// Under an external clock the master owns tempo, and setTempo() writes nothing (spec §9.7, §12.7).
	bool tempoFollowsMaster() override;
	/// A transition's new roster plays from here (spec §12.9): `roster` becomes `outgoing`, and the incoming roster
	/// takes its place.
	void transitionStarts(int32_t startTick) override;
	/// The transition's command landed, or the core dropped it: the lifecycle's phase follows (spec §12.9).
	void transitionLanded() override;
	void transitionDropped() override;
	/// The outgoing instance's note-offs and late hits, for the outgoing roster.
	void playOutgoing(const Event& event) override;
	/// The outgoing roster has had its last note-off: it retires.
	void outgoingDrained() override;
	/// The safety net after a flush: every voice on every part's Output released. Tails ring out (spec §12.5).
	void allNotesOff();

	Roster roster;
	/// After a transition's start step, until the outgoing instance has drained: the roster it plays.
	Roster outgoing;
	/// A live ramp's length: one unswung 16th in the song's internal ticks, at its magnitude now (spec §11.7).
	int32_t rampTicks = kTicksPerStep;
#if ENABLE_SYSEX_TEST
	uint32_t outgoingSpaces =
	    0; ///< The outgoing roster's Throw Space changes since its start step, for its drain event.
#endif
	/// The last Section event played, for the OLED.
	SectionKind section = SectionKind::Intro;
	uint16_t occurrence = 0;
	int32_t sectionTick = 0; ///< Where it started, in Generator ticks.
	bool sectionKnown = false;
#if ENABLE_SYSEX_TEST
	/// Test builds: note-ons played from tick `fillFrom` up to `fillTo`, while a held fill played (gen_fill_notes).
	int32_t fillFrom = INT32_MAX;
	int32_t fillTo = INT32_MAX;
	int32_t fillKicks = 0;
	int32_t fillPitched = 0;
	int32_t fillDrums = 0;
	/// Test builds: the audio sample time the Riser's note-on played at (0: none sounding), and its sample's length.
	uint32_t riserOn = 0;
	int32_t riserSampleMicros = 0;
#endif

private:
	void note(const Roster& parts, const Event& event, bool on);
#if ENABLE_SYSEX_TEST
	/// Test builds: gen_riser_on and gen_riser, how close a Riser's sample ended to its note-off (the Drop).
	void timeRiser(const Event& event);
#endif
	static void allNotesOff(const Roster& parts);
};

/// The steering's firmware adapter (perform/perform_steering.h): the Current Recipe, the transition's phase, the
/// song, MIDI feedback, the tick's wake and the test port. Its presses are the view's, the menus' and MIDI's.
class Steering final : public perform::PerformSteering<Steering> {
private:
	friend class perform::PerformSteering<Steering>;
	[[nodiscard]] perform::PerformDriver& driver() const;
	[[nodiscard]] perform::NoteSink& sink() const;
	[[nodiscard]] Recipe& recipe() const { return currentRecipe.recipe; }
	[[nodiscard]] Takes& takes() const { return currentRecipe.takes; }
	[[nodiscard]] bool changePending() const;
	void glideStarted();
	void swingIntervalChanged(int32_t interval);
	void keyChanged(Key key);
	void macroCentreMoved(Macro macro);
#if ENABLE_SYSEX_TEST
	void takeSetStopped(int32_t part, int32_t take);
	void keySet(Key key, perform::KeySet how);
	void fillPressed(int32_t value);
#else
	void takeSetStopped(int32_t /*part*/, int32_t /*take*/) {}
	void keySet(Key /*key*/, perform::KeySet /*how*/) {}
	void fillPressed(int32_t /*value*/) {}
#endif
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

	/// F14: the panel's PLAY under Perform, with SHIFT as `groove`. From stop it starts the next piece, in Groove with
	/// SHIFT, unless a transition that commits when loaded still preloads: then playback starts once it has loaded,
	/// and PLAY again cancels that (spec §12.10). Playing, it stops, as stock PLAY does.
	void pressPlay(bool groove);
	/// A song position pointer while following an external clock: the next tick is a jump to `songTick` (spec §12.4).
	void songPositionPointer(int32_t songTick);

	/// The steering presses and edits (spec §2.6): the view, the menus and MIDI press them here.
	[[nodiscard]] Steering& steering() { return steering_; }
	/// The piece playing, for the sidebar's and status line's queries (spec §2.8); nullptr when stopped.
	[[nodiscard]] const Generator* playingGenerator() const {
		return active() && driver_.running() ? generator_ : nullptr;
	}

	// ---- Homes (spec §11.6) ----

	/// F3: the player set the param `stack` addresses to `value`, from a knob, a fader, the sound editor or MIDI.
	/// While a piece plays, a param a sound control moves gets a new home, logged at the next undecided step.
	void playerEdited(const ModelStackWithAutoParam& stack, int32_t value);

	// For the Perform view.
	[[nodiscard]] bool playing() const { return driver_.running(); }
	/// Generator ticks since step 0's grid line, while playing.
	[[nodiscard]] int32_t clock() const { return driver_.clock(); }
	[[nodiscard]] const OutputSink& output() const { return sink_; }
	/// The phrases heard in the piece playing, for the SONG page's played columns (spec §2.8).
	[[nodiscard]] const perform::PhraseHistory& history() const { return driver_.history(); }

private:
	friend class EntryJob;
	friend class FreeJob;
	friend class LeaveJob;
	friend class SoundJob;
	friend class SoundSwaps;
	friend class TransitionJob;
	friend class Transitions;
	friend class OutputSink;
	friend class Steering;

	/// Starts playback from stop, the next piece in Groove with `groove`, as the panel's PLAY does.
	void startPlayback(bool groove);
	/// Whether the next piece starts in Groove, with no Intro: the panel's SHIFT+PLAY sets it just before playback
	/// starts, and clears it if nothing started. A start takes it up and clears it.
	void setStartInGroove(bool groove) { startInGroove_ = groove; }
	void startPiece(int32_t swungTick, int32_t magnitude);
	/// Once per bar under an external clock: the Recipe tempo tracks it, rounded to a whole BPM (spec §13.7).
	void sampleExternalTempo();
	void stopPiece();
	/// A Generator instance plays from its step 0, at a start or a transition's start step: the steering sends again
	/// what the new one has cleared (a FILL or a KEYBOARD fader still held), and the step counters start again.
	void instanceStarts();
	/// The steps counted from the playing instance's step 0 start again: a transition's new roster counts its own.
	void restartStepCounters();
	/// The Generator block not playing: a transition's incoming instance starts in it (spec §12.9).
	[[nodiscard]] Generator* idleBlock() const { return blocks_[0] == generator_ ? blocks_[1] : blocks_[0]; }
	Error allocateBlocks();
	void freeBlocks();
	/// Swaps Perform's built song in, stopped (spec §14.3).
	void swapIn(Song* song, const Roster& roster);
	/// Forgets Perform's song, roster and Generator, as the teardown starts.
	void dropSong();
	/// The teardown's task couldn't start: Perform's song is still whole and current, and Perform carries on.
	void undoLeave() { steering_.undoLeave(); }
	/// Between LEAVE and the teardown's start: the tick plays nothing, and a teardown that can't start undoes it.
	[[nodiscard]] bool leaving() const { return steering_.leaving(); }

	/// Perform allocates two blocks on entry, because a transition overlaps two instances (spec §2.1, §12.9). They
	/// alternate: each transition's new roster starts in the one not playing.
	Generator* blocks_[2] = {};
	Generator* generator_ = nullptr;
	/// Perform's song, from the swap to the leave: the tick plays only while it is current.
	Song* song_ = nullptr;
	/// Whether an external Start's magnitude matching was running at the last tick.
	bool matchingBefore_ = false;
	bool startInGroove_ = false;
	/// The piece ended at its Outro's end and stopped the transport; nothing starts until playback ends.
	bool pieceEnded_ = false;
	/// A mid-play song position pointer waiting for the next tick, and where the master went.
	bool positionPending_ = false;
	/// The steering queue (its first member), and every press and edit. Offset 20 is baked into
	/// Generator::decideNextStep()'s code, which addresses the queue from a constant: an offset that needs a longer
	/// instruction grows the step's code (see the implementation notes).
	Steering steering_;
	perform::PerformDriver driver_;
	OutputSink sink_;
	int32_t lastSwungTick_ = 0;
	/// The song's tick magnitude at the last tick: an external clock can change it (spec §12.3).
	int32_t magnitude_ = 0;
	int32_t positionTick_ = 0;
	/// The next step whose decision samples the external clock's tempo: each bar line.
	int32_t nextTempoSample_ = 0;
#if ENABLE_SYSEX_TEST
	/// H2 (spec §20): the next step at which the live ramps' cost is reported.
	int32_t nextH2Report_ = 0;
	/// The last step whose decision "hold stress" ramped every param on.
	int32_t stressStep_ = -1;
	/// The held fill entry the last tick saw the Generator play, for the gen_fill test event.
	int32_t fillShown_ = -1;
	/// The macros the last tick saw the Generator play, for the gen_macro_landed test event.
	Macros macrosShown_;
#endif
};

extern PerformAdapter performAdapter;

inline perform::PerformDriver& Steering::driver() const {
	return performAdapter.driver_;
}
inline perform::NoteSink& Steering::sink() const {
	return performAdapter.sink_;
}
inline bool Steering::changePending() const {
	return transitions.pending();
}

} // namespace deluge::generative::firmware
