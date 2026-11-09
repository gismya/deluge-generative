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

#include "generative/firmware/transitions.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS Transitions transitions{};

namespace {

/// The pending Recipe (spec §12.9), with its seed, takes and sound picks: in SDRAM, beside the Current Recipe. The
/// Current Recipe stays the playing one until the start step.
struct PendingRecipe {
	Recipe recipe;
	SoundPicks sounds;
	uint32_t seed = 0;
	Takes takes;
};
PLACE_SDRAM_BSS PendingRecipe pendingRecipe;
/// The incoming roster a preload builds. One is enough: a cancelled preload frees its own before the next one starts.
PLACE_SDRAM_BSS Roster building;

const char* phaseName(Transitions::Phase phase) {
	switch (phase) {
	case Transitions::Phase::Idle:
		return "idle";
	case Transitions::Phase::Preloading:
		return "preloading";
	case Transitions::Phase::Queued:
		return "queued";
	case Transitions::Phase::Committed:
		return "committed";
	case Transitions::Phase::Retiring:
		return "retiring";
	}
	return "?";
}

} // namespace

/// A transition's preload (spec §12.9, §12.10): live, so the playing roster plays on and the Perform page works under
/// the busy rules. It rolls the pending Recipe, then builds each incoming part's Output into Perform's song, linking
/// each in one chunk. A cancel or a card error frees what it built, unless Perform is being left: then it goes with
/// Perform's song.
class TransitionJob final : public Job {
public:
	TransitionJob() : Job(Kind::Live) {}

	void prepare(uint32_t request) {
		request_ = request;
		error_ = Error::NONE;
		missing_ = 0;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		building = Roster{}; // the last preload's went to the transition, or was freed
		const Genre* genre = currentRecipe.recipe.genre;
		Song* song = performAdapter.song_;
		if (genre == nullptr || song == nullptr) {
			return JobOutcome::Failed;
		}
#if ENABLE_SYSEX_TEST
		if (TestPort::preloadHeld()) {
			// Test hook: wait here, yielding, so a script's BACK lands while the job runs.
			TEST_EVENT("gen_transition_preload_held");
			while (TestPort::preloadHeld() && !chunks.cancelled()) {
				chunks.boundary();
			}
			TestPort::releasePreload();
			if (chunks.cancelled()) {
				return JobOutcome::Cancelled;
			}
		}
#endif
		// Regenerate (spec §9.7): rolled facts leaning towards the key that plays, a new seed, every take at 0, and
		// every sound pick rolled away from the one playing. The macros are the live ones, copied at the start step.
		pendingRecipe.recipe =
		    newRecipe(*genre, freshEntropy(), Key{currentRecipe.recipe.keyRoot, currentRecipe.recipe.scale});
		pendingRecipe.seed = freshEntropy();
		pendingRecipe.takes = Takes{};
		pendingRecipe.sounds = currentRecipe.sounds;
		CardFolders folders(&chunks);
		if (!rerollSounds(*genre, currentRecipe.palette(), pendingRecipe.sounds, freshEntropy(), folders)) {
			if (folders.cancelled()) {
				return JobOutcome::Cancelled;
			}
			error_ = Error::SD_CARD;
			return JobOutcome::Failed;
		}
		transitions.rolled_ = transitions.request_ == request_;
		for (int32_t p = 0; p < partCount(*genre); p++) {
			if (chunks.boundary()) {
				return unwind(song, Error::ABORTED_BY_USER);
			}
			int32_t missing = 0;
			// Each Output links in one chunk at its end, with no yield inside (spec §12.9).
			error_ = buildPartOutput(song, *genre, p, pendingRecipe.sounds, currentRecipe.palette(), &building.parts[p],
			                         chunks, &missing);
			missing_ += missing;
			if (error_ != Error::NONE) {
				return unwind(song, error_);
			}
		}
		return chunks.boundary() ? unwind(song, Error::ABORTED_BY_USER) : JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
		TEST_EVENT("gen_transition_preload %s error=%d parts=%ld ms=%lu",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<int>(error_), static_cast<long>(builtParts()), msSince(submitted_));
		if (outcome == JobOutcome::Failed) {
			// The stock card error (spec §10.7); a job whose task couldn't start has no error of its own.
			display->displayError(error_ != Error::NONE ? error_ : Error::UNSPECIFIED);
		}
		if (outcome == JobOutcome::Done) {
			if (missing_ > 0) {
				display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
				TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(missing_));
			}
			transitions.preloaded(building, request_);
		}
		else {
			transitions.preloadEnded(request_);
		}
	}

private:
	/// Frees what was built (each Output a chunk), unless Perform is being left.
	JobOutcome unwind(Song* song, Error error) {
		if (!performAdapter.leaving_) {
			for (PartOutput& part : building.parts) {
				freePartOutput(song, part);
			}
		}
		building = Roster{};
		if (error == Error::ABORTED_BY_USER) {
			error_ = Error::NONE;
			return JobOutcome::Cancelled;
		}
		error_ = error;
		return JobOutcome::Failed;
	}
	[[nodiscard]] int32_t builtParts() const {
		int32_t n = 0;
		for (const PartOutput& part : building.parts) {
			n += part.output != nullptr ? 1 : 0;
		}
		return n;
	}

	uint32_t request_ = 0;
	Error error_ = Error::NONE;
	int32_t missing_ = 0;
	uint32_t submitted_ = 0;
};

namespace {
/// Two, alternating: a request cancels the one running and waits in the runner's next slot while it unwinds.
TransitionJob jobs[2];
} // namespace

void Transitions::setPhase(Phase phase) {
	if (phase != phase_) {
		phase_ = phase;
		TEST_EVENT("gen_transition phase=%s", phaseName(phase));
	}
}

bool Transitions::cancellable() const {
	if (phase_ == Phase::Preloading) {
		return true;
	}
	const Generator* piece = performAdapter.playingGenerator();
	return phase_ == Phase::Queued && piece != nullptr && piece->transitionStart() < 0;
}

Transitions::Request Transitions::regenerate(TransitionStyle style) {
	if (!performAdapter.active() || performAdapter.leaving_ || currentRecipe.recipe.genre == nullptr) {
		return Request::Ignored;
	}
	if (phase_ == Phase::Committed || phase_ == Phase::Retiring || (phase_ == Phase::Queued && !cancellable())) {
		TEST_EVENT("gen_transition request style=%d refused phase=%s", static_cast<int>(style), phaseName(phase_));
		return Request::Refused;
	}
	// A second request during Preloading or Queued replaces the first (spec §12.9).
	if (phase_ == Phase::Queued) {
		cancelQueued("replaced");
	}
	dropTakesAndSounds();
	request_++;
	rolled_ = false;
	commitOnLoad_ = false;
	startWhenLoaded_ = false;
	style_ = style;
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_transition");
#endif
	// The job running (this transition's last preload, a sound's, or a free) is cancelled where it can be, and this
	// one waits in the next slot until it has unwound.
	TransitionJob& job = runner.active() == &jobs[0] ? jobs[1] : jobs[0];
	if (runner.active() == &jobs[0] || runner.active() == &jobs[1]) {
		runner.cancel();
	}
	job.prepare(request_);
	if (runner.next() != &job && !runner.submit(job)) {
		// Only if `job` were already active, which the choice above rules out.
		setPhase(Phase::Idle);
		return Request::Ignored;
	}
	setPhase(Phase::Preloading);
	TEST_EVENT("gen_transition request style=%d started", static_cast<int>(style));
	return Request::Started;
}

void Transitions::dropTakesAndSounds() {
	// At the confirm, takes and sounds still queued are dropped, and a sound still preloading is cancelled and freed
	// (spec §12.9). Each goes the way a press of it again would.
	const Generator* piece = performAdapter.playingGenerator();
	for (int32_t p = 0; piece != nullptr && p < kMaxParts; p++) {
		if (piece->takeWaiting(p) >= 0) {
			performAdapter.queue_.push({CommandKind::SetTake, static_cast<uint8_t>(p), piece->take(p)});
		}
	}
	soundSwaps.cancelAll();
}

bool Transitions::cancel() {
	if (!cancellable()) {
		return false;
	}
	if (phase_ == Phase::Preloading) {
		// The UI reacts at once; the job unwinds and frees what it built (spec §12.10).
		request_++;
		if (runner.active() == &jobs[0] || runner.active() == &jobs[1]) {
			runner.cancel();
		}
		if (runner.next() == &jobs[0] || runner.next() == &jobs[1]) {
			runner.clearNext();
		}
		commitOnLoad_ = false;
		startWhenLoaded_ = false;
		display->removeWorkingAnimation();
		TEST_EVENT("gen_transition cancel preload");
		setPhase(Phase::Idle);
		return true;
	}
	cancelQueued("queued");
	setPhase(Phase::Idle);
	return true;
}

void Transitions::cancelQueued(const char* why) {
	// Its command goes before the line's decision can land it: the cancel is drained first (spec §2.6).
	performAdapter.queue_.push({CommandKind::Transition, 0, kCancelTransition});
	performAdapter.driver_.cancelIncoming(performAdapter.sink_);
	for (const PartOutput& part : incoming_.parts) {
		retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
	}
	incoming_ = Roster{};
	TEST_EVENT("gen_transition cancel %s", why);
}

void Transitions::preloaded(const Roster& roster, uint32_t request) {
	if (performAdapter.leaving_ || !performAdapter.active()) {
		return; // the Outputs go with Perform's song
	}
	if (request != request_) {
		// Cancelled after its last boundary: its Outputs never played.
		for (const PartOutput& part : roster.parts) {
			retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
		}
		return;
	}
	display->removeWorkingAnimation();
	incoming_ = roster;
	Generator* playing = performAdapter.generator_;
	if (commitOnLoad_ || !performAdapter.playing()) {
		// Stopped (spec §12.9): it commits at once, and the next start plays the new roster.
		commitStopped();
		if (startWhenLoaded_) {
			startWhenLoaded_ = false;
			performAdapter.setStartInGroove(startInGroove_);
			playbackHandler.playButtonPressed(kInternalButtonPressLatency);
			if (!playbackHandler.playbackState) {
				performAdapter.setStartInGroove(false);
			}
		}
		return;
	}
	// The incoming block starts in task context with the pending Recipe; its first decision waits for the start step.
	Generator* incoming = performAdapter.idleBlock();
	incoming->startIncoming(pendingRecipe.recipe, pendingRecipe.seed, pendingRecipe.takes, style_);
	if (!performAdapter.queue_.push({CommandKind::Transition, static_cast<uint8_t>(style_), 0})) {
		for (const PartOutput& part : incoming_.parts) {
			retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
		}
		incoming_ = Roster{};
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
		setPhase(Phase::Idle);
		return;
	}
	performAdapter.driver_.queueIncoming(*incoming, performAdapter.glideLengthTicks());
	pushedAt_ = playing->nextStep();
	setPhase(Phase::Queued);
}

void Transitions::preloadEnded(uint32_t request) {
	if (request != request_) {
		return; // a newer request or a cancel has moved on
	}
	display->removeWorkingAnimation();
	if (performAdapter.leaving_ && rolled_) {
		commitRecipe(); // leaving commits (spec §12.9): Compose sees the new Recipe; the roster goes with the song
	}
	commitOnLoad_ = false;
	startWhenLoaded_ = false;
	setPhase(Phase::Idle);
}

void Transitions::commitRecipe() {
	const Macros live = currentRecipe.recipe.macros;
	currentRecipe.recipe = pendingRecipe.recipe;
	currentRecipe.recipe.macros = live;
	currentRecipe.sounds = pendingRecipe.sounds;
	currentRecipe.seed = pendingRecipe.seed;
	currentRecipe.takes = pendingRecipe.takes;
	performAdapter.newRecipeTakes();
	soundSwaps.forgetPrevious(); // the new roster has no previous sound (spec §12.9)
}

void Transitions::started() {
	// The new roster plays from the start step (spec §12.9): it starts clean, with the pending Recipe.
	OutputSink& sink = performAdapter.sink_;
	sink.outgoing = sink.roster;
	sink.roster = incoming_;
	incoming_ = Roster{};
	performAdapter.generator_ = const_cast<Generator*>(performAdapter.driver_.playing());
	commitRecipe();
	performAdapter.restartStepCounters();
	soundControls.begin(sink.roster, *currentRecipe.recipe.genre);
	swingIntervalDue_ = true;
	starts_++;
	setPhase(Phase::Retiring);
	TEST_EVENT("gen_transition start style=%d", static_cast<int>(style_));
}

void Transitions::outgoingDrained() {
	OutputSink& sink = performAdapter.sink_;
	const Genre* genre = currentRecipe.recipe.genre;
	const uint32_t cap = genre != nullptr ? Retirements::phraseSamples(*genre) : 0;
	for (const PartOutput& part : sink.outgoing.parts) {
		retirements.retire(part, Retirements::When::AfterTail, cap, Retirements::Owner::Transition);
	}
	sink.outgoing = Roster{};
}

void Transitions::commitStopped() {
	OutputSink& sink = performAdapter.sink_;
	const Genre* genre = currentRecipe.recipe.genre;
	const uint32_t cap = genre != nullptr ? Retirements::phraseSamples(*genre) : 0;
	for (const PartOutput& part : sink.roster.parts) {
		retirements.retire(part, Retirements::When::AfterTail, cap, Retirements::Owner::Transition);
	}
	sink.roster = incoming_;
	incoming_ = Roster{};
	commitRecipe();
	// The tempo jumps to the new Recipe's (spec §12.9), as do swing and the key.
	sink.setTempo(currentRecipe.recipe.tempo);
	sink.setSwing(currentRecipe.recipe.swing);
	currentSong->changeSwingInterval(swingSyncLevel(currentRecipe.recipe.swingInterval));
	writeSongKey(currentSong, {currentRecipe.recipe.keyRoot, currentRecipe.recipe.scale});
	commitOnLoad_ = false;
	starts_++;
	setPhase(Phase::Retiring);
	TEST_EVENT("gen_transition start stopped");
}

void Transitions::playbackEnded() {
	switch (phase_) {
	case Phase::Preloading:
		commitOnLoad_ = true; // Stop commits in any phase (spec §12.9): this one when its preload ends
		TEST_EVENT("gen_transition commit_on_load");
		break;
	case Phase::Queued:
	case Phase::Committed:
		commitStopped(); // the driver has dropped the incoming block
		break;
	default:
		break;
	}
}

bool Transitions::startWhenLoaded(bool groove) {
	if (!holdsStart()) {
		return false;
	}
	startWhenLoaded_ = true;
	startInGroove_ = groove;
	TEST_EVENT("gen_transition start_when_loaded");
	return true;
}

void Transitions::service() {
	if (!performAdapter.active() || performAdapter.leaving_) {
		return;
	}
	if (swingIntervalDue_) {
		swingIntervalDue_ = false;
		currentSong->changeSwingInterval(swingSyncLevel(currentRecipe.recipe.swingInterval));
	}
	const Generator* piece = performAdapter.playingGenerator();
	if (phase_ == Phase::Queued && piece != nullptr) {
		if (piece->transitionStart() >= 0) {
			setPhase(Phase::Committed);
		}
		else if (!piece->transitionWaiting() && piece->nextStep() > pushedAt_ + 1) {
			// The core dropped it: a BACK tap cancelled the newest action (spec §13.5).
			performAdapter.driver_.cancelIncoming(performAdapter.sink_);
			for (const PartOutput& part : incoming_.parts) {
				retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
			}
			incoming_ = Roster{};
			TEST_EVENT("gen_transition cancel dropped");
			setPhase(Phase::Idle);
		}
	}
	if (phase_ == Phase::Retiring && performAdapter.driver_.outgoing() == nullptr
	    && retirements.pending(Retirements::Owner::Transition) == 0) {
		setPhase(Phase::Idle);
	}
}

void Transitions::forget() {
	incoming_ = Roster{};
	commitOnLoad_ = false;
	startWhenLoaded_ = false;
	swingIntervalDue_ = false;
	phase_ = Phase::Idle;
}

} // namespace deluge::generative::firmware
