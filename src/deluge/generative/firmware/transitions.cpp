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
#include "processing/engines/audio_engine.h"
#include <cstddef>

namespace deluge::generative::firmware {

alignas(8) PLACE_SDRAM_BSS Transitions transitions{}; // 8: see rosterStarts()

namespace {

/// The pending Recipe (spec §12.9), with its seed, takes and sound picks: in SDRAM, beside the Current Recipe. The
/// Current Recipe stays the playing one until the start step.
///
/// `sounds` comes first, and both Recipes are aligned to 8, for the start step's cost in the tick: commitRecipe()
/// copies the sound picks (2.7 KB, cold, SDRAM to SDRAM), and newlib's memcpy prefetches (`pld`) only when source and
/// destination differ in their address modulo 8. With the same remainder it waits for each cache line, and the start
/// step costs about 6 µs more on the Deluge. Unpinned, the link order decides which copy runs.
struct PendingRecipe {
	SoundPicks sounds;
	Recipe recipe;
	uint32_t seed = 0;
	Takes takes;
};
// The Current Recipe's sound picks follow its Recipe, the pending one's start the struct: 4 apart modulo 8. If a new
// Recipe field breaks this, move `sounds` so that the two offsets differ modulo 8 again.
static_assert(sizeof(Recipe) % 8 == 4 && alignof(SoundPicks) <= 4 && offsetof(PendingRecipe, sounds) == 0);
alignas(8) PLACE_SDRAM_BSS PendingRecipe pendingRecipe;
/// The incoming roster a preload builds. One is enough: a cancelled preload frees its own before the next one starts.
PLACE_SDRAM_BSS Roster building;
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
		transitions.rolled_ = transitions.newest(request_);
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
			transitions.preloaded(request_);
		}
		else {
			transitions.loadEnded(request_);
		}
	}

private:
	/// Frees what was built (each Output a chunk), unless Perform is being left.
	JobOutcome unwind(Song* song, Error error) {
		if (!performAdapter.leaving()) {
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

// ---- Perform's side ----

perform::PerformDriver& Transitions::driver() {
	return performAdapter.driver_;
}
SteeringQueue& Transitions::queue() {
	return performAdapter.steering_.queue();
}
perform::NoteSink& Transitions::sink() {
	return performAdapter.sink_;
}
bool Transitions::ready() const {
	return performAdapter.active() && !performAdapter.leaving() && currentRecipe.recipe.genre != nullptr;
}
bool Transitions::leaving() const {
	return performAdapter.leaving();
}
bool Transitions::transportRunning() const {
	return playbackHandler.playbackState != 0;
}
void Transitions::startPlayback(bool groove) {
	performAdapter.startPlayback(groove);
}
int32_t Transitions::cutGlideTicks() const {
	return performAdapter.steering_.glideLengthTicks();
}

// ---- The preload ----

bool Transitions::submitPreload(uint32_t request) {
	rolled_ = false;
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_transition");
#endif
	// The job running (this transition's last preload, a sound's, or a free) is cancelled where it can be, and this
	// one waits in the next slot until it has unwound.
	TransitionJob& job = runner.active() == &jobs[0] ? jobs[1] : jobs[0];
	if (runner.active() == &jobs[0] || runner.active() == &jobs[1]) {
		runner.cancel();
	}
	job.prepare(request);
	// A refused submit only if `job` were already active, which the choice above rules out.
	return runner.next() == &job || runner.submit(job);
}

void Transitions::cancelPreload() {
	if (runner.active() == &jobs[0] || runner.active() == &jobs[1]) {
		runner.cancel();
	}
	if (runner.next() == &jobs[0] || runner.next() == &jobs[1]) {
		runner.clearNext();
	}
	display->removeWorkingAnimation();
}

void Transitions::preloadEnded() {
	display->removeWorkingAnimation();
	if (performAdapter.leaving() && rolled_) {
		commitRecipe(); // leaving commits (spec §12.9): Compose sees the new Recipe; the roster goes with the song
	}
}

void Transitions::dropQueuedSounds() {
	soundSwaps.cancelAll(); // a sound still preloading is cancelled and freed (spec §12.9)
}

void Transitions::preloaded(uint32_t request) {
	// Perform gone or being left: loaded() changes nothing and returns true, and the Outputs go with Perform's song.
	if (!loaded(request)) {
		// Cancelled after its last boundary: its Outputs never played.
		for (const PartOutput& part : building.parts) {
			retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
		}
	}
}

// ---- The incoming roster ----

void Transitions::adoptIncoming() {
	display->removeWorkingAnimation();
	incoming_ = building;
}

Generator& Transitions::startIncoming(TransitionStyle style) {
	Generator* incoming = performAdapter.idleBlock();
	incoming->startIncoming(pendingRecipe.recipe, pendingRecipe.seed, pendingRecipe.takes, style);
	return *incoming;
}

void Transitions::releaseIncoming() {
	for (const PartOutput& part : incoming_.parts) {
		retirements.retire(part, Retirements::When::Now, 0, Retirements::Owner::Transition);
	}
	incoming_ = Roster{};
}

void Transitions::showQueueFull() {
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
}

void Transitions::commitRecipe() {
	const Macros live = currentRecipe.recipe.macros;
	currentRecipe.recipe = pendingRecipe.recipe;
	currentRecipe.recipe.macros = live;
	currentRecipe.sounds = pendingRecipe.sounds;
	currentRecipe.seed = pendingRecipe.seed;
	currentRecipe.takes = pendingRecipe.takes;
	performAdapter.steering_.newRecipe();
	soundSwaps.forgetPrevious(); // the new roster has no previous sound (spec §12.9)
}

// ---- The start, in the tick ----

void Transitions::rosterStarts([[maybe_unused]] TransitionStyle style) {
#if ENABLE_SYSEX_TEST
	const uint32_t started = getSystemTicks();
#endif
	// The incoming roster is cold in SDRAM. Its copy prefetches only when the two rosters differ in their address
	// modulo 8, as with the sound picks (see PendingRecipe); both objects are aligned to 8, so the offsets decide. If a
	// new member breaks this, move `incoming_` in Transitions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
	static_assert(
	    (offsetof(Transitions, incoming_) - offsetof(PerformAdapter, sink_) - offsetof(OutputSink, roster)) % 8 != 0);
#pragma GCC diagnostic pop
	OutputSink& sink = performAdapter.sink_;
	sink.outgoing = sink.roster;
	sink.roster = incoming_;
	incoming_ = Roster{};
	performAdapter.generator_ = performAdapter.idleBlock(); // the incoming instance started there
	commitRecipe();
	performAdapter.instanceStarts();   // as at a start: a FILL still held reaches the new instance (spec §8.7)
	soundControls.keepOutgoingSpace(); // the outgoing parts' Throws fall after the start step (spec §11.4)
	soundControls.begin(sink.roster, *currentRecipe.recipe.genre);
	swingIntervalDue_ = true;
	// us: what the start step costs in the tick, the sound controls' binding included.
	TEST_EVENT("gen_transition start style=%d us=%lu", static_cast<int>(style),
	           static_cast<unsigned long>((getSystemTicks() - started) / (DELUGE_CLOCKS_PER / 1000000)));
}

void Transitions::retireOutgoing(uint32_t cap) {
	OutputSink& sink = performAdapter.sink_;
	for (const PartOutput& part : sink.outgoing.parts) {
		retirements.retire(part, Retirements::When::AfterTail, cap, Retirements::Owner::Transition);
	}
	sink.outgoing = Roster{};
}

// ---- A stop ----

void Transitions::commitStopped(uint32_t cap) {
	OutputSink& sink = performAdapter.sink_;
	for (const PartOutput& part : sink.roster.parts) {
		retirements.retire(part, Retirements::When::AfterTail, cap, Retirements::Owner::Transition);
	}
	sink.roster = incoming_;
	incoming_ = Roster{};
	commitRecipe();
	// The tempo jumps to the new Recipe's (spec §12.9), as do swing and the key.
	sink.setTempo(currentRecipe.recipe.tempo);
	sink.setSwing(currentRecipe.recipe.swing);
	// This can run in the tick, when the piece ended by itself with the transition Queued (the lifecycle's
	// playbackEnded() says when). The transport has stopped by then (endPlayback() clears playbackState before the
	// view's playbackEnded()), so this only sets the interval: nothing is rescheduled, which the tick must not do.
	currentSong->changeSwingInterval(swingSyncLevel(currentRecipe.recipe.swingInterval));
	writeSongKey(currentSong, {currentRecipe.recipe.keyRoot, currentRecipe.recipe.scale});
}

// ---- Time, and the end ----

uint32_t Transitions::now() const {
	return AudioEngine::audioSampleTimer;
}

uint32_t Transitions::phraseLength() const {
	const Genre* genre = currentRecipe.recipe.genre;
	return genre != nullptr ? Retirements::phraseSamples(*genre) : 0;
}

bool Transitions::outgoingFreed() const {
	return retirements.pending(Retirements::Owner::Transition) == 0;
}

#if ENABLE_SYSEX_TEST
void Transitions::phaseChanged(Phase phase) {
	TEST_EVENT("gen_transition phase=%s", perform::phaseName(phase));
}

void Transitions::traced(Trace what, TransitionStyle style) {
	TEST_EVENT("gen_transition %s", perform::traceText(what, style, phase()).text);
}
#endif

void Transitions::service() {
	if (!ready()) {
		return; // follow() asks too: asked once here, the two checks cost no code (sizediff)
	}
	if (swingIntervalDue_) {
		swingIntervalDue_ = false;
		currentSong->changeSwingInterval(swingSyncLevel(currentRecipe.recipe.swingInterval));
	}
	follow();
}

void Transitions::forget() {
	incoming_ = Roster{};
	swingIntervalDue_ = false;
	reset();
}

} // namespace deluge::generative::firmware
