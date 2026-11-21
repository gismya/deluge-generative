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

#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/log_header.h"
#include "generative/firmware/perform_midi.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/perform/clock_follow.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/generative_popups.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/flash_storage.h"
#include <algorithm>

namespace deluge::generative::firmware {

alignas(8) PerformAdapter performAdapter{}; // 8: see incomingStarts()

void PerformAdapter::startPiece(int32_t swungTick, int32_t magnitude) {
	// Every start is fresh: the Arc resets and a new piece begins (spec §13.2). SHIFT+PLAY skips the Intro.
	const bool groove = startInGroove_;
	startInGroove_ = false;
	pieceEnded_ = false;
	steering_.startFromCurrentRecipe(*generator_, groove);
	pieceBegins();
	// Where the master is: playback started from metronomeOffset, a Continue's song position pointer, or 0. Step 0
	// lands on its next bar line (spec §12.4).
	const int32_t master = playbackHandler.metronomeOffset + swungTick;
	driver_.begin(*generator_, master, magnitude);
#if ENABLE_SYSEX_TEST
	driver_.setDecisionTimer(&decisionTiming);
#endif
	// The next piece starts at the Recipe's tempo: the last one tracked from an external clock, too (spec §13.7).
	driver_.setTempo(currentRecipe.recipe.tempo, sink_);
	driver_.setSwing(currentRecipe.recipe.swing, sink_);
	lastSwungTick_ = swungTick; // the first delta is 0: no catch-up burst when the hook first runs late
	magnitude_ = magnitude;
	positionPending_ = false;
	instanceStarts();
	TEST_EVENT("gen_start master=%ld to_step0=%ld groove=%d tempo=%ld", static_cast<long>(master),
	           static_cast<long>(-driver_.clock()), groove ? 1 : 0, static_cast<long>(currentRecipe.recipe.tempo));
}

void PerformAdapter::pieceBegins() {
	// Every control starts at home: read from the parts' Outputs as they are now (spec §11.6).
	soundControls.begin(sink_.roster, *currentRecipe.recipe.genre);
	sink_.sectionKnown = false;
#if ENABLE_SYSEX_TEST
	macrosShown_ = generator_->recipe().macros;
	biasShown_ = generator_->recipe().bias;
#endif
}

void PerformAdapter::restartPiece(Generator& generator) {
	// The safety net after the flush, as at a stop or the piece's end (spec §12.5).
	sink_.allNotesOff();
	// The first run's sounds go back home, a Throw's Space too, before the new piece reads them as home (spec §11.6),
	// as a stop puts them back. Its waiting sounds go with the core's pending list.
	soundControls.restoreHomes();
	soundSwaps.pieceRestarts();
	steering_.restartAsItIs(generator);
	pieceBegins();
	instanceStarts();
	restarts_++;
	TEST_EVENT("gen_restart as_it_is tempo=%ld", static_cast<long>(currentRecipe.recipe.tempo));
}

void OutputSink::restartPiece(Generator& generator) {
	performAdapter.restartPiece(generator);
}

void PerformAdapter::stopPiece() {
	if (driver_.running()) {
		driver_.stop(sink_);
		reportLog(true);
	}
	sink_.allNotesOff();
	soundControls.restoreHomes(); // every sound back as its preset was, or as the player left it while stopped
}

#if ENABLE_SYSEX_TEST
void PerformAdapter::reportLog(bool stopped) const {
	// H13 (spec §20): the newest piece's bytes, its header's, its records, its end and cut, and the whole log's use.
	if (log_.pieces() == 0) {
		return;
	}
	const SteeringLog::Piece piece = log_.piece(log_.pieces() - 1);
	int32_t records = 0;
	piece.forEachRecord([&](const LogRecord&) { records++; });
	const auto pieces = static_cast<long>(log_.pieces());
	const auto used = static_cast<long>(log_.used());
	const auto bytes = static_cast<long>(piece.bytes());
	const auto header = static_cast<long>(piece.headerBytes());
	const auto end = static_cast<long>(piece.endStep());
	const auto cut = static_cast<long>(piece.cutAt());
	TEST_EVENT("gen_log at=%s pieces=%ld used=%ld piece=%ld header=%ld records=%ld end=%ld cut=%ld full=%d",
	           stopped ? "stop" : "phrase", pieces, used, bytes, header, static_cast<long>(records), end, cut,
	           log_.full() ? 1 : 0);
}
#endif

void PerformAdapter::dropSong() {
	soundControls.forget();
	soundSwaps.forgetSong();
	transitions.forget();
	retirements.forget();
	generator_ = nullptr;
	song_ = nullptr;
	sink_ = OutputSink{};
	steering_.forget();
}

int32_t PerformAdapter::tick(int32_t swungTick) {
	if (!active() || leaving() || currentSong != song_) {
		return INT32_MAX; // the hook runs for every song; only Perform's plays the Generator
	}
	const int32_t magnitude = currentSong->getInputTickMagnitude();
	// Whether an external Start's magnitude matching ran at the last call, and (kept for the next call) runs now.
	const bool matchingBefore = matchingBefore_;
	matchingBefore_ = playbackHandler.tempoMagnitudeMatchingActiveNow;
	if (!driver_.running()) {
		if (pieceEnded_) {
			return INT32_MAX; // the piece ended; the transport is stopping
		}
		if (transitions.holdsStart()) {
			// A stop committed a transition whose preload still runs: the new roster starts once it has loaded, on
			// the master's next bar line (spec §12.10).
			lastSwungTick_ = swungTick;
			return 1;
		}
		startPiece(swungTick, magnitude);
	}
	else if (magnitude != magnitude_) {
		// The firmware rescales its tick count with the magnitude, so the count moves only at the new rate.
		lastSwungTick_ = magnitude > magnitude_ ? lastSwungTick_ << (magnitude - magnitude_)
		                                        : lastSwungTick_ >> (magnitude_ - magnitude);
		// Matching settling or a jump (spec §12.3). Asked here, after the rescale both share: a switch on the
		// rule's answer made GCC copy the rescale into each case (+68 bytes of tick).
		if (perform::magnitudeChange(magnitude_, magnitude, matchingBefore, matchingBefore_)
		    == perform::MagnitudeChange::Jump) {
			driver_.jump(sink_);
			// Count from here: a small step back in the rescaled tick count must not read as a second jump.
			lastSwungTick_ = swungTick;
			TEST_EVENT("gen_jump scale magnitude=%ld", static_cast<long>(magnitude));
		}
		magnitude_ = magnitude;
	}
	if (positionPending_) {
		positionPending_ = false;
		driver_.jumpTo(positionTick_, magnitude, sink_);
		TEST_EVENT("gen_jump spp master=%ld to_line=%ld", static_cast<long>(positionTick_),
		           static_cast<long>(stepTick(generator_->nextStep()) - driver_.clock()));
	}
	const int32_t delta = swungTick - lastSwungTick_;
	lastSwungTick_ = swungTick;
	sink_.rampTicks = 6 << magnitude; // one unswung 16th in internal ticks (spec §11.7)
	const uint32_t jumps = driver_.jumps();
	steering_.sampleFill(currentSong->isFillModeActive());
	int32_t wait = driver_.advance(delta, magnitude, steering_.queue(), sink_);
#if ENABLE_SYSEX_TEST
	if (generator_->heldFill() != fillShown_) {
		// A held fill landed on the last decided step, or ended there.
		fillShown_ = generator_->heldFill();
		const int32_t step = generator_->nextStep() - 1;
		TEST_EVENT("gen_fill entry=%ld step=%ld", static_cast<long>(fillShown_), static_cast<long>(step));
		if (fillShown_ >= 0) {
			sink_.fillFrom = stepTick(step);
			sink_.fillTo = INT32_MAX;
			sink_.fillKicks = sink_.fillPitched = sink_.fillDrums = 0;
		}
		else if (sink_.fillFrom != INT32_MAX) {
			sink_.fillTo = stepTick(step);
		}
	}
	for (int32_t m = 0; m < kNumMacros; m++) {
		// A SetMacro (a knob turn, a KEYBOARD fader's hold or its spring back) landed on the last decided step.
		const auto macro = static_cast<Macro>(m);
		if (generator_->recipe().macros[macro] != macrosShown_[macro]) {
			macrosShown_.set(macro, generator_->recipe().macros[macro]);
			TEST_EVENT("gen_macro_landed macro=%ld units=%ld step=%ld", static_cast<long>(m),
			           static_cast<long>(macrosShown_[macro]), static_cast<long>(generator_->nextStep() - 1));
		}
	}
	for (int32_t p = 0; p < kMaxParts; p++) {
		// A SetBias (an F8 turn with the part focused, a KEYBOARD fader, RESET PART BIAS) landed on the last decided
		// step, or a transition's start step brought the new roster's.
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			if (generator_->recipe().bias.of(p, macro) != biasShown_.of(p, macro)) {
				biasShown_.set(p, macro, generator_->recipe().bias.of(p, macro));
				TEST_EVENT("gen_bias_landed part=%ld macro=%ld units=%ld step=%ld", static_cast<long>(p),
				           static_cast<long>(m), static_cast<long>(biasShown_.of(p, macro)),
				           static_cast<long>(generator_->nextStep() - 1));
			}
		}
		// A SetOctave landed on the last decided step: the Current Recipe still has the shift before it.
		if (generator_->octave(p) != currentRecipe.recipe.octave[p]) {
			const int32_t landed = generator_->nextStep() - 1;
			TEST_EVENT("gen_octave_landed part=%ld shift=%ld step=%ld bar_line=%s", static_cast<long>(p),
			           static_cast<long>(generator_->octave(p)), static_cast<long>(landed),
			           landed % kStepsPerBar == 0 ? "yes" : "no");
		}
	}
#endif
	steering_.recordLanded(*generator_);
	if (driver_.jumps() != jumps) {
		TEST_EVENT("gen_jump clock delta=%ld", static_cast<long>(delta));
	}
	if (driver_.ended()) {
		pieceEnded_ = true;
		// The Outro is over: the transport stops on its last line (spec §13.2). As stock's arrangement end, from
		// inside the tick.
		sink_.allNotesOff();
		TEST_EVENT("gen_piece_ended");
		playbackHandler.endPlayback();
		return INT32_MAX;
	}
	if (playbackHandler.isExternalClockActive() && generator_->nextStep() >= nextTempoSample_) {
		sampleExternalTempo();
	}
#if ENABLE_SYSEX_TEST
	if (TestPort::rampsStressed() && generator_->nextStep() != stressStep_) {
		// H2's worst case: every bound param rewritten on every step, alternating up and down.
		stressStep_ = generator_->nextStep();
		controlTiming.begin();
		soundControls.stress((stressStep_ & 1) != 0, sink_.rampTicks);
		controlTiming.end(stepTick(stressStep_));
	}
	// The playing Generator's genre, whose phrase it plays: around a transition the Current Recipe's can differ.
	if (const Genre* genre = generator_->recipe().genre;
	    genre != nullptr && generator_->nextStep() > nextPhraseReport_) {
		// The Generator decided a phrase line's step: a script waits on the music, not the host's clock, e.g. for End
		// to have missed a phrase line (spec §13.5). The line's Section event reaches the sink as it plays, just after.
		const int32_t line = (generator_->nextStep() - 1) / genre->phraseSteps() * genre->phraseSteps();
		if (line > 0) {
			TEST_EVENT("gen_phrase bar=%ld", static_cast<long>(line / kStepsPerBar));
			reportLog(false);
		}
		nextPhraseReport_ = line + genre->phraseSteps();
	}
	if (generator_->nextStep() >= nextH2Report_) {
		nextH2Report_ += 4 * kStepsPerBar;
		controlTiming.report(TestPort::rampsStressed() ? "stress" : TestPort::rampsHeld() ? "off" : "on");
	}
#endif
	return wait;
}

void PerformAdapter::sampleExternalTempo() {
	nextTempoSample_ = (generator_->nextStep() / kStepsPerBar + 1) * kStepsPerBar;
	if (playbackHandler.tempoMagnitudeMatchingActiveNow) {
		return; // not settled yet
	}
	const int32_t bpm = static_cast<int32_t>(playbackHandler.calculateBPMForDisplay() + 0.5f);
	const int32_t tempo = std::clamp(bpm * 100, kMinTempo, kMaxTempo);
	if (tempo == currentRecipe.recipe.tempo) {
		return;
	}
	// Logged only when the rounded value changes (spec §13.7): through the queue, as driver-side steering.
	currentRecipe.recipe.tempo = tempo;
	steering_.queue().push({CommandKind::SetTempo, 0, currentRecipe.recipe.tempo});
	// The driver times a sample Riser at the tempo it knows (spec §9.5); the sink writes nothing under the master.
	// setTempo() also ends a running tempo glide and records `tempo` as written, though the sink wrote nothing.
	// Harmless today: the master sets the tempo, PerformSteering::setTempo() starts no glide under it, and the next
	// piece starts with setTempo(), which always writes.
	driver_.setTempo(tempo, sink_);
	TEST_EVENT("gen_ext_tempo bpm=%ld", static_cast<long>(tempo / 100));
}

void PerformAdapter::playbackEnded() {
	if (active() && currentSong == song_) {
		stopPiece();
		soundSwaps.playbackEnded();
		transitions.playbackEnded(); // a stop commits a transition (spec §12.9)
	}
	startInGroove_ = false;
	pieceEnded_ = false; // the next start is a fresh piece
}

void PerformAdapter::pressPlay(bool groove) {
	const bool restartShortcut = FlashStorage::accessibilityShortcuts
	                                 ? Buttons::isButtonPressed(deluge::hid::button::CROSS_SCREEN_EDIT)
	                                 : Buttons::isButtonPressed(deluge::hid::button::X_ENC);
	if (restartShortcut) {
		pressRestart();
		return;
	}
	playOrStop(groove);
}

void PerformAdapter::playOrStop(bool groove) {
	if (playbackHandler.playbackState) {
		playbackHandler.playButtonPressed(kInternalButtonPressLatency); // a stop
		return;
	}
	if (active() && !leaving() && transitions.startWhenLoaded(groove)) {
		return; // it starts when the incoming roster has loaded
	}
	startPlayback(groove);
}

void PerformAdapter::pressRestart() {
	using perform::PressOutcome;
	if (!active() || leaving()) {
		return;
	}
	if (!playbackHandler.playbackState) {
		// Stopped, AS IT IS is PLAY: the Current Recipe from bar 1 (spec §13.2). A PLAY already waiting for a
		// transition's preload is cancelled by this one, as PLAY's would be: no RESTART popup then.
		const bool cancelsWait = transitions.playWaits();
		TEST_EVENT("gen_restart_press %s", cancelsWait ? "stopped_cancels_wait" : "stopped");
		playOrStop(false);
		if (!cancelsWait) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_RESTART));
		}
		return;
	}
	const bool cancels = steering_.restartAhead();
	const PressOutcome outcome = steering_.pressRestart();
	TEST_EVENT("gen_restart_press %s", perform::refused(outcome) ? "refused" : cancels ? "cancelled" : "queued");
	if (gui::refusedWithPopup(outcome) || outcome != PressOutcome::Accepted) {
		return;
	}
	display->displayPopup(
	    l10n::get(cancels ? l10n::String::STRING_FOR_RESTART_CANCELLED : l10n::String::STRING_FOR_RESTART));
}

void PerformAdapter::startPlayback(bool groove) {
	setStartInGroove(groove);
	playbackHandler.playButtonPressed(kInternalButtonPressLatency);
	if (!playbackHandler.playbackState) {
		setStartInGroove(false); // nothing started
	}
}

void PerformAdapter::songPositionPointer(int32_t songTick) {
	if (active() && !leaving() && currentSong == song_ && perform::pointerJumps(driver_.running())) {
		positionPending_ = true;
		positionTick_ = songTick;
	}
}

void PerformAdapter::playerEdited(const ModelStackWithAutoParam& stack, int32_t value) {
	if (!active() || leaving() || !driver_.running()) {
		return; // stopped, the edit is on the Output: the next piece reads it as home (spec §11.7)
	}
	if (const auto edit = soundControls.playerEdited(stack, value)) {
		// Acts at once; the core only logs it, at the next undecided step (spec §11.6). Continuous, so it coalesces
		// per param and a full queue only loses the log entry.
		steering_.queue().push({CommandKind::SetHome, edit->part, edit->home, edit->param, edit->lane});
	}
	else if (const auto edit = paramEdit(stack, value, sink_.roster, *currentRecipe.recipe.genre, *song_)) {
		// Any other param edit is logged the same way, for Capture's automation (spec §17.2). A full queue loses the
		// entry while the edit is still heard: Capture's song then misses it.
		steering_.queue().push(*edit);
	}
}

void PerformAdapter::takeRoster(Roster& incoming) {
	sink_.roster = incoming;
	incoming = Roster{};
}

void PerformAdapter::incomingStarts(Roster& incoming, const perform::PendingRecipe& pending) {
	// The incoming roster is cold in SDRAM. Its copy prefetches only when the two rosters differ in their address
	// modulo 8, as with the sound picks (perform/pending_recipe.h); both objects are aligned to 8, so the
	// offsets decide. If a new member breaks this, move `incoming_` in Transitions. The steering's size moves `sink_`:
	// MacroSteering's Part bias holds keep their part in 16 bits so they grow it by 16 B; with 8 bits, 12 B broke this
	// (#303).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
	static_assert((Transitions::incomingOffset() - (offsetof(PerformAdapter, sink_) + offsetof(OutputSink, roster))) % 8
	                  != 0,
	              "the start step's copy of the roster must not have the same address modulo 8 at both ends");
#pragma GCC diagnostic pop
	sink_.outgoing = sink_.roster;
	takeRoster(incoming);
	generator_ = idleBlock(); // the incoming instance started there
	steering_.handOver(pending);
	instanceStarts(); // as at a start: a FILL still held reaches the new instance (spec §8.7)
}

void PerformAdapter::incomingCommits(Roster& incoming, const perform::PendingRecipe& pending, uint32_t capSamples) {
	retirements.retireRoster(sink_.roster, Retirements::When::AfterTail, Retirements::Owner::Transition, capSamples);
	takeRoster(incoming);
	// This can run in the tick, when the piece ended by itself with the transition Queued (the lifecycle's
	// playbackEnded() says when). The transport has stopped by then (endPlayback() clears playbackState before the
	// view's playbackEnded()), so the swing interval's write (Steering::swingIntervalChanged()) only sets the
	// interval: nothing is rescheduled, which the tick must not do.
	steering_.takeOverStopped(pending);
}

void PerformAdapter::pendingCommitsOnLeave(const perform::PendingRecipe& pending) {
	steering_.handOver(pending);
}

void PerformAdapter::retireOutgoing(uint32_t capSamples) {
	retirements.retireRoster(sink_.outgoing, Retirements::When::AfterTail, Retirements::Owner::Transition, capSamples);
}

void PerformAdapter::instanceStarts() {
	restartStepCounters();
	steering_.instanceStarts(currentSong->isFillModeActive());
#if ENABLE_SYSEX_TEST
	fillShown_ = -1;
	sink_.fillFrom = sink_.fillTo = INT32_MAX;
	controlTiming.reset();
	decisionTiming.reset(); // H1's windows start with the instance: the first carries no older steps
#endif
}

void PerformAdapter::restartStepCounters() {
	// The external clock's tempo is first sampled on the first bar line after step 0: at the start the firmware's
	// measure of it hasn't settled.
	nextTempoSample_ = kStepsPerBar;
#if ENABLE_SYSEX_TEST
	nextH2Report_ = 4 * kStepsPerBar;
	nextPhraseReport_ = 0;
	stressStep_ = -1;
#endif
}

// ---- The steering's firmware adapter ----

void Steering::glideStarted() {
	// The glide starts at the hook's next call: ask for it on the next swung tick, so it starts at the press, and an
	// Instant swing lands on the next step line even when nothing else would run the hook there.
	playbackHandler.expectEvent();
}

void Steering::swingIntervalChanged(int32_t interval) {
	currentSong->changeSwingInterval(swingSyncLevel(interval));
}

void Steering::keyChanged(Key key) {
	writeSongKey(currentSong, key);
}

void Steering::macroCentreMoved(Macro macro) {
	sendMacroFeedback(macro);
}

#if ENABLE_SYSEX_TEST
void Steering::takeSetStopped(int32_t part, int32_t take) {
	TEST_EVENT("gen_take part=%u take=%ld stopped", static_cast<unsigned>(part), static_cast<long>(take));
}

void Steering::keySet(Key key, perform::KeySet how) {
	TEST_EVENT("gen_key_set root=%u scale=%u %s", static_cast<unsigned>(key.root), static_cast<unsigned>(key.scale),
	           perform::keySetName(how));
}

void Steering::fillPressed(int32_t value) {
	TEST_EVENT("gen_fill_press value=%ld", static_cast<long>(value));
}
#endif

} // namespace deluge::generative::firmware
