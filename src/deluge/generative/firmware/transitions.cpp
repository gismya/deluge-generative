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
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/recipe_files.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/working_animation.h"
#include "generative/perform/regenerate_page.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace deluge::generative::firmware {

alignas(8) PLACE_SDRAM_BSS Transitions transitions{}; // 8: see PerformAdapter::incomingStarts()

namespace {

/// The pending Recipe (perform/pending_recipe.h) is in SDRAM, beside the Current Recipe.
///
/// `sounds` comes first, and both Recipes are aligned to 8, for the start step's cost in the tick: commitPending()
/// copies the sound picks (2.7 KB, cold, SDRAM to SDRAM), and newlib's memcpy prefetches (`pld`) only when source and
/// destination differ in their address modulo 8. With the same remainder it waits for each cache line, and the start
/// step costs about 6 µs more on the Deluge. Unpinned, the link order decides which copy runs.
using perform::PendingRecipe;
// Both objects are aligned to 8, so the two copies of the sound picks differ in their address modulo 8 when their
// offsets do: the Current Recipe's follow its Recipe, the pending one's start the struct. If a new field breaks this,
// move `sounds` so that the two offsets differ modulo 8 again.
static_assert((offsetof(CurrentRecipe, sounds) - offsetof(PendingRecipe, sounds)) % 8 != 0,
              "commitPending()'s copy of the sound picks must not have the same address modulo 8 at both ends");
alignas(8) PLACE_SDRAM_BSS PendingRecipe pendingRecipe;
/// The incoming roster a preload builds. One is enough: a cancelled preload frees its own before the next one starts.
PLACE_SDRAM_BSS Roster building;
/// The name a LOAD's file has, which the Current Recipe takes with the pending Recipe (spec §16.3); empty for a genre
/// change or Regenerate. Set by the preload, with the pending Recipe.
PLACE_SDRAM_BSS perform::RecipeName pendingName;
/// A LOAD's file and its name, one per TransitionJob: submitPreload() copies them in when the request goes ahead, so a
/// refused request, or one for a job still waiting in the runner's next slot, never changes what another job reads.
struct LoadSlot {
	RecipeFile file;
	perform::RecipeName name;
};
PLACE_SDRAM_BSS LoadSlot loadSlots[2];
} // namespace

/// A transition's preload (spec §12.9, §12.10): live, so the playing roster plays on and the Perform page works under
/// the busy rules. It rolls the pending Recipe, then builds each incoming part's Output into Perform's song, linking
/// each in one chunk. A cancel or a card error frees what it built, unless Perform is being left: then it goes with
/// Perform's song.
class TransitionJob final : public Job {
public:
	TransitionJob() : Job(Kind::Live) {}

	/// `load`: a LOAD's file and name (this job's own copy), or nullptr for a genre change or Regenerate.
	void prepare(uint32_t request, const Genre* genre, const VariantPicks& narrowed, const LoadSlot* load) {
		request_ = request;
		genre_ = genre;
		narrowed_ = narrowed;
		load_ = load;
		error_ = Error::NONE;
		missing_ = 0;
		replaced_ = 0;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		building = Roster{}; // the last preload's went to the transition, or was freed
		const Genre* playing = currentRecipe.recipe.genre;
		const Genre* genre = genre_;
		Song* song = performAdapter.song_;
		if (playing == nullptr || genre == nullptr || song == nullptr) {
			return JobOutcome::Failed;
		}
#if ENABLE_SYSEX_TEST
		if (TestPort::preloadHeld()) {
			// Test hook: wait here, yielding, so a script's BACK lands while the job runs.
			TEST_EVENT("gen_transition_preload_held");
			TestPort::waitWhilePreloadHeld(chunks);
			if (chunks.cancelled()) {
				return JobOutcome::Cancelled;
			}
		}
#endif
		if (load_ != nullptr) {
			// A LOAD (spec §16.5, §16.6): the file's facts, seed, takes and macros, with its genre's palette read as a
			// genre change reads it (so a LOAD of the playing genre reads its override again), and each missing sound
			// re-rolled from that palette. Its macros replace the live ones at the start step.
			error_ = currentRecipe.stagePalette(*genre);
			if (error_ != Error::NONE) {
				return JobOutcome::Failed;
			}
			pendingRecipe.recipe = load_->file.recipe;
			pendingRecipe.stagedPalette = true;
			pendingRecipe.macros = perform::HandoverMacros::Pending;
			pendingRecipe.seed = load_->file.seed;
			pendingRecipe.takes = load_->file.takes;
			pendingRecipe.sounds = load_->file.sounds;
			pendingName = load_->name;
			error_ = replaceMissingCardSounds(*genre, currentRecipe.stagedPalette(), pendingRecipe.sounds, replaced_,
			                                  &chunks);
			if (error_ == Error::ABORTED_BY_USER) {
				error_ = Error::NONE;
				return JobOutcome::Cancelled;
			}
			if (error_ != Error::NONE) {
				return JobOutcome::Failed;
			}
		}
		else if (!rollPending(chunks, *genre, *playing)) {
			return error_ == Error::NONE ? JobOutcome::Cancelled : JobOutcome::Failed;
		}
		const Palette& palette = pendingRecipe.stagedPalette ? currentRecipe.stagedPalette() : currentRecipe.palette();
		transitions.rolled_ = transitions.newest(request_);
		for (int32_t p = 0; p < partCount(*genre); p++) {
			if (chunks.boundary()) {
				return unwind(song, Error::ABORTED_BY_USER);
			}
			int32_t missing = 0;
			// Each Output links in one chunk at its end, with no yield inside (spec §12.9).
			error_ =
			    buildPartOutput(song, *genre, p, pendingRecipe.sounds, palette, &building.parts[p], chunks, &missing);
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
			if (transitions.newest(request_)) {
				// The pending Recipe's song values, as the song takes them (swing interval as a sync level, the scale
				// as modeNotes bits: the rolled root alone often matches the playing one's), once its roster has
				// loaded: a stop's commit writes them (gen_commit_song), and the next start plays at its tempo
				// (gen_start). A cancelled preload, or one an older request ran, shows none.
				TEST_EVENT("gen_pending_recipe tempo=%ld swing=%d interval=%d key=%d mode_notes=%u",
				           static_cast<long>(pendingRecipe.recipe.tempo), static_cast<int>(pendingRecipe.recipe.swing),
				           static_cast<int>(swingSyncLevel(pendingRecipe.recipe.swingInterval)),
				           static_cast<int>(pendingRecipe.recipe.keyRoot),
				           static_cast<unsigned>(scaleNotes(pendingRecipe.recipe.scale)));
				char picks[64];
				perform::picksText(*pendingRecipe.recipe.genre, pendingRecipe.recipe.variants, picks);
				TEST_EVENT("gen_pending_variants %s", picks);
			}
			if (replaced_ > 0 && transitions.newest(request_)) {
				showSoundsReplaced(replaced_); // spec §16.6
			}
			else if (missing_ > 0) {
				display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
			}
			if (missing_ > 0) {
				TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(missing_));
			}
			transitions.preloaded(request_);
		}
		else {
			transitions.loadEnded(request_);
		}
	}

private:
	/// A genre change or Regenerate (spec §9.7): facts rolled from the incoming genre, leaning towards the key that
	/// plays, each variant set drawn within the page's narrowed masks (spec §13.6), a new seed and every take at 0. A
	/// genre change rolls the new roster's sound picks from its palette, with its card override read into the Current
	/// Recipe's spare one; a Regenerate rolls every pick away from the one playing. The macros are the live ones,
	/// copied at the start step. False on a cancel, or on a card error (then error_ is set).
	bool rollPending(Chunks& chunks, const Genre& genre, const Genre& playing) {
		const bool change = &genre != &playing;
		if (change) {
			error_ = currentRecipe.stagePalette(genre);
			if (error_ != Error::NONE) {
				return false;
			}
		}
		const Palette& palette = change ? currentRecipe.stagedPalette() : currentRecipe.palette();
		pendingRecipe.recipe =
		    newRecipe(genre, freshEntropy(), Key{currentRecipe.recipe.keyRoot, currentRecipe.recipe.scale}, &narrowed_);
		pendingRecipe.stagedPalette = change;
		pendingRecipe.macros = perform::HandoverMacros::Live;
		pendingRecipe.seed = freshEntropy();
		pendingRecipe.takes = Takes{};
		pendingRecipe.sounds = currentRecipe.sounds;
		pendingName.clear();
		CardFolders folders(&chunks);
		if (!rollIncomingSounds(genre, playing, palette, pendingRecipe.sounds, freshEntropy(), folders)) {
			error_ = folders.cancelled() ? Error::NONE : Error::SD_CARD;
			return false;
		}
		return true;
	}

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
	const Genre* genre_ = nullptr; ///< The incoming genre
	VariantPicks narrowed_;        ///< A genre change's or Regenerate's: what each set draws within
	const LoadSlot* load_ = nullptr;
	Error error_ = Error::NONE;
	int32_t missing_ = 0;
	int32_t replaced_ = 0; ///< A LOAD's missing sounds, re-rolled (spec §16.6)
	uint32_t submitted_ = 0;
};

namespace {
/// Two, alternating: a request cancels the one running and waits in the runner's next slot while it unwinds.
TransitionJob jobs[2];
} // namespace

// ---- Perform's side ----

perform::PerformDriver& Transitions::driver() {
	return performAdapter.driver();
}
Steering& Transitions::steering() {
	return performAdapter.steering();
}
perform::NoteSink& Transitions::sink() {
	return performAdapter.noteSink();
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
	return performAdapter.steering().glideLengthTicks();
}

Transitions::Request Transitions::change(const Genre& genre, TransitionStyle style, const VariantPicks& narrowed) {
#if ENABLE_SYSEX_TEST
	if (TestPort::queueFullHeld()) {
		// Test hook: as if the steering queue had no room for the confirm's presses, which request() refuses first.
		traced(Trace::RequestQueueFull, style);
		return Request::QueueFull;
	}
#endif
	// A refused request leaves the preload running, and what it rolls from, alone.
	return request(style, Incoming{.genre = &genre, .narrowed = narrowed});
}

Transitions::Request Transitions::load(const RecipeFile& file, const perform::RecipeName& name, TransitionStyle style) {
#if ENABLE_SYSEX_TEST
	if (TestPort::queueFullHeld()) {
		traced(Trace::RequestQueueFull, style);
		return Request::QueueFull;
	}
#endif
	return request(style, Incoming{.genre = file.recipe.genre, .file = &file, .name = &name});
}

// ---- The preload ----

bool Transitions::submitPreload(uint32_t request, const Incoming& incoming) {
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_transition");
#endif
	// This one waits in the next slot while a job runs (this transition's last preload, a sound's, or a free), and
	// the transition's last preload, the other job, is cancelled. A refused submit only if `job` were already active,
	// which the choice below rules out; it would change nothing, as the lifecycle needs. A job whose task can't start
	// ends inside the submit, which the lifecycle notices.
	const bool first = runner.active() != &jobs[0];
	TransitionJob& job = first ? jobs[0] : jobs[1];
	// A LOAD's file goes to this job's own slot: the other job may still run, reading its own.
	LoadSlot* load = nullptr;
	if (incoming.file != nullptr) {
		load = &loadSlots[first ? 0 : 1];
		load->file = *incoming.file;
		load->name = *incoming.name;
	}
	job.prepare(request, incoming.genre, incoming.narrowed, load);
	if (runner.next() != &job && !runner.submitReplacing(job, first ? jobs[1] : jobs[0])) {
		return false;
	}
	rolled_ = false;
	return true;
}

void Transitions::cancelPreload() {
	if (runner.active() == &jobs[0] || runner.active() == &jobs[1]) {
		runner.cancel();
	}
	if (runner.next() == &jobs[0] || runner.next() == &jobs[1]) {
		runner.clearNext();
	}
	transitionPreloadEnded();
}

void Transitions::preloadEnded() {
	transitionPreloadEnded();
	if (performAdapter.leaving() && rolled_) {
		// Leaving commits (spec §12.9): Compose sees the new Recipe; the roster goes with the song.
		commitPending([] { performAdapter.pendingCommitsOnLeave(pendingRecipe); });
	}
}

int32_t Transitions::waitingSounds() const {
	return soundSwaps.waitingSounds();
}

void Transitions::dropWaitingSounds() {
	soundSwaps.cancelAll(); // a sound still preloading is cancelled and freed (spec §12.9)
}

void Transitions::preloaded(uint32_t request) {
	// Perform gone or being left: loaded() returns true and the Outputs go with Perform's song. Being left, the newest
	// preload's end reaches preloadEnded(), which commits a Recipe it rolled.
	if (!loaded(request)) {
		// Cancelled after its last boundary: its Outputs never played.
		retirements.retireRoster(building, Retirements::When::Now, Retirements::Owner::Transition, Retirements::kNoCap);
	}
}

// ---- The incoming roster ----

void Transitions::adoptIncoming() {
	transitionPreloadEnded();
	incoming_ = building;
}

Generator& Transitions::startIncoming(TransitionStyle style) {
	Generator& incoming = performAdapter.idleGenerator();
	incoming.startIncoming(pendingRecipe.recipe, pendingRecipe.seed, pendingRecipe.takes, style);
	return incoming;
}

perform::HandoverMacros Transitions::incomingMacros() const {
	return pendingRecipe.macros;
}

void Transitions::releaseIncoming() {
	retirements.retireRoster(incoming_, Retirements::When::Now, Retirements::Owner::Transition, Retirements::kNoCap);
}

void Transitions::showQueueFull() {
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
}

template <typename HandOver>
void Transitions::commitPending(HandOver handOver) {
	if (pendingRecipe.stagedPalette) {
		currentRecipe.adoptStagedPalette(*pendingRecipe.recipe.genre); // its palette came with it
	}
	handOver();
	adoptPending();
}

void Transitions::adoptPending() {
	currentRecipe.sounds = pendingRecipe.sounds;
	currentRecipe.replaced(); // the Compose screen's previous picks were the old Recipe's (spec §14.4)
	if (!pendingName.empty()) {
		currentRecipe.name = pendingName; // a LOAD's file's name (spec §16.3)
	}
	soundSwaps.forgetPrevious(); // the new roster has no previous sound (spec §12.9)
}

// ---- The start, in the tick ----

void Transitions::rosterStarts([[maybe_unused]] TransitionStyle style) {
#if ENABLE_SYSEX_TEST
	const uint32_t started = getSystemTicks();
#endif
	// The incoming roster's copy: its address rule is in incomingStarts()
	commitPending([this] { performAdapter.incomingStarts(incoming_, pendingRecipe); });
	soundControls.keepOutgoingSpace(); // the outgoing parts' Throws fall after the start step (spec §11.4)
	soundControls.begin(performAdapter.output().roster, *currentRecipe.recipe.genre);
	swingIntervalDue_ = true;
	// us: what the start step costs in the tick, the sound controls' binding included. tempo_off: how far the song's
	// tempo is from the incoming Recipe's, in hundredths of a BPM as the display shows it. The glide arrives where the
	// new roster starts (spec §9.7), but this runs at the start step's decision tick, 12 ticks before it plays: a
	// glide too short to finish before this tick (a cut's, Dub's 1-bar Build's) is a few BPM short here, one that
	// never ran the whole change.
	TEST_EVENT("gen_transition start style=%d us=%lu tempo_off=%ld", static_cast<int>(style),
	           static_cast<unsigned long>((getSystemTicks() - started) / (DELUGE_CLOCKS_PER / 1000000)),
	           std::labs(lroundf(playbackHandler.calculateBPMForDisplay() * 100.f)
	                     - static_cast<long>(currentRecipe.recipe.tempo)));
}

void Transitions::retireOutgoing(uint32_t cap) {
	performAdapter.retireOutgoing(cap);
}

// ---- A stop ----

void Transitions::commitStopped(uint32_t cap) {
	commitPending([this, cap] { performAdapter.incomingCommits(incoming_, pendingRecipe, cap); });
	// What the song has after the commit, in gen_pending_recipe's terms (the tempo in hundredths of a BPM, as the
	// display shows it: under an external clock the sink's setTempo() writes nothing, so this shows that clock's).
	TEST_EVENT("gen_commit_song tempo=%ld swing=%d interval=%d key=%d mode_notes=%u",
	           static_cast<long>(lroundf(playbackHandler.calculateBPMForDisplay() * 100.f)),
	           static_cast<int>(currentSong->swingAmount), static_cast<int>(currentSong->swingInterval),
	           static_cast<int>(currentSong->key.rootNote), static_cast<unsigned>(currentSong->key.modeNotes.toBits()));
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

int32_t Transitions::retirementRoom() const {
	// The job running may be past its last boundary: if it ends for an older request, preloaded() retires its roster.
	// One waiting in the next slot has built nothing, and a request re-prepares or replaces it.
	const bool running = runner.active() == &jobs[0] || runner.active() == &jobs[1];
	return retirements.room(Retirements::Owner::Transition) - (running ? kMaxParts : 0);
}

#if ENABLE_SYSEX_TEST
void Transitions::phaseChanged(Phase phase) {
	TEST_EVENT("gen_transition phase=%s working=%d", perform::phaseName(phase), workingAnimationShown() ? 1 : 0);
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
