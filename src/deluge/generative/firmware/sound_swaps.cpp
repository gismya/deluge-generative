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

#include "generative/firmware/sound_swaps.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/sound_controls.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include <climits>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SoundSwaps soundSwaps{};

namespace {

/// Where a preload rolls its picks, the picks of each part's sound waiting for its line, and each part's previous
/// picks (spec §13.4: one previous pick per part). Parts never share a lane, so one SoundPicks holds every part's.
PLACE_SDRAM_BSS SoundPicks stagedPicks;
PLACE_SDRAM_BSS SoundPicks incomingPicks;
PLACE_SDRAM_BSS SoundPicks previousPicks;

/// Copies `part`'s picks, its preset and its lanes' samples, from `from` to `to`.
void copyPartPicks(const Genre& genre, int32_t part, const SoundPicks& from, SoundPicks& to) {
	to.part[part] = from.part[part];
	for (const LaneSpec& lane : genre.parts[part].lanes) {
		const auto index = static_cast<int32_t>(lane.lane);
		to.lane[index] = from.lane[index];
	}
}

#if ENABLE_SYSEX_TEST
unsigned long msSince(uint32_t start) {
	return static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000));
}
#endif

} // namespace

/// A sound re-roll's preload (spec §10.7): live, so Perform plays on and works as usual meanwhile (spec §12.10).
class SoundJob final : public Job {
public:
	SoundJob() : Job(Kind::Live) {}

	/// Before each submit.
	void prepare(int32_t part, bool previous, uint8_t token) {
		part_ = part;
		previous_ = previous;
		token_ = token;
		error_ = Error::NONE;
		missing_ = 0;
		built_ = PartOutput{};
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		const Genre* genre = currentRecipe.recipe.genre;
		Song* song = performAdapter.song_;
		if (genre == nullptr || song == nullptr) {
			return JobOutcome::Failed;
		}
		stagedPicks = currentRecipe.sounds;
		if (previous_) {
			copyPartPicks(*genre, part_, previousPicks, stagedPicks);
		}
		else {
			// Fresh entropy, never the seed (spec §6.4). A Kit part re-rolls every lane it plays.
			CardFolders folders(&chunks);
			if (!rollPartSounds(*genre, currentRecipe.palette(), part_, stagedPicks, freshEntropy(), folders)) {
				if (folders.cancelled()) {
					return JobOutcome::Cancelled;
				}
				error_ = Error::SD_CARD;
				return JobOutcome::Failed;
			}
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}
		error_ = buildPartOutput(song, *genre, part_, stagedPicks, currentRecipe.palette(), &built_, chunks, &missing_);
		if (error_ == Error::ABORTED_BY_USER) {
			error_ = Error::NONE;
			return JobOutcome::Cancelled; // buildPartOutput() freed what it built
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed; // a card error: the old sound plays on (spec §10.7)
		}
		chunks.commit(); // linked into the song: it swaps in at its line, or is freed if that's cancelled
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
		display->removeWorkingAnimation();
		TEST_EVENT("gen_sound_preload %s part=%ld previous=%d error=%d token=%u ms=%lu",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<long>(part_), previous_ ? 1 : 0, static_cast<int>(error_), static_cast<unsigned>(token_),
		           msSince(submitted_));
		if (outcome == JobOutcome::Failed) {
			// The stock card error (spec §10.7); a job whose task couldn't start has no error of its own.
			display->displayError(error_ != Error::NONE ? error_ : Error::UNSPECIFIED);
			return;
		}
		if (outcome != JobOutcome::Done) {
			return;
		}
		if (missing_ > 0) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
			TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(missing_));
		}
		soundSwaps.preloaded(part_, token_, built_, stagedPicks);
	}

	[[nodiscard]] int32_t part() const { return part_; }

private:
	int32_t part_ = 0;
	bool previous_ = false;
	uint8_t token_ = 0;
	Error error_ = Error::NONE;
	int32_t missing_ = 0;
	PartOutput built_;
	uint32_t submitted_ = 0;
};

/// Frees retired and cancelled Outputs (spec §12.9): live, and never stopped once it starts. Leaving waits for it.
class FreeJob final : public Job {
public:
	FreeJob() : Job(Kind::Live) {}

	JobOutcome run(Chunks& chunks) override {
		for (int32_t i = 0; i < count; i++) {
			freePartOutput(performAdapter.song_, outputs[i]);
			chunks.boundary();
		}
		// Their samples too: memory comes back once the old sound is freed (spec §20).
		deleteUnusedAudioFiles();
		return JobOutcome::Done;
	}
	[[nodiscard]] bool cancellable() const override { return false; }
	void finished(JobOutcome outcome) override {
		TEST_EVENT("gen_sound_freed outputs=%ld", static_cast<long>(outcome == JobOutcome::Done ? count : 0));
#if ENABLE_SYSEX_TEST
		if (outcome == JobOutcome::Done) {
			reportFreeMemory("after_sound_free");
		}
#endif
		count = 0;
	}
	/// Dropped from the next slot (leaving): the Outputs stay linked, and go with Perform's song.
	void dropped() override { count = 0; }

	PartOutput outputs[SoundSwaps::kMaxRetiring];
	int32_t count = 0;
};

namespace {
SoundJob soundJob;
FreeJob freeJob;
} // namespace

uint8_t SoundSwaps::nextToken() {
	lastToken_ = lastToken_ >= kMaxSoundToken ? 1 : lastToken_ + 1;
	return lastToken_;
}

bool SoundSwaps::preloading(int32_t part) const {
	return runner.active() == &soundJob && soundJob.part() == part;
}

SoundSwaps::Press SoundSwaps::press(int32_t part, bool previous) {
	const Genre* genre = currentRecipe.recipe.genre;
	if (!performAdapter.active() || performAdapter.leaving_ || genre == nullptr || part < 0
	    || part >= partCount(*genre)) {
		return Press::Ignored;
	}
	// Pressed again while its sound preloads or waits for the line: that one is cancelled (spec §13.5).
	if (preloading(part)) {
		runner.cancel();
		TEST_EVENT("gen_sound_cancel part=%ld preload", static_cast<long>(part));
		return Press::Cancelled;
	}
	Incoming& in = incoming_[part];
	if (in.token != 0) {
		performAdapter.queue_.push({CommandKind::SetSound, static_cast<uint8_t>(part), kNoSound});
		retire(in.output, INT_MIN);
		in = Incoming{};
		TEST_EVENT("gen_sound_cancel part=%ld waiting", static_cast<long>(part));
		return Press::Cancelled;
	}
	if (previous && !hasPrevious_[part]) {
		return Press::NoPrevious;
	}
	// Only one card load at a time: another job running, or one waiting, refuses it (spec §12.10).
	if (runner.busy()) {
		return Press::Busy;
	}
	soundJob.prepare(part, previous, nextToken());
	if (!runner.submit(soundJob)) {
		return Press::Busy;
	}
	TEST_EVENT("gen_sound_press part=%ld previous=%d token=%u", static_cast<long>(part), previous ? 1 : 0,
	           static_cast<unsigned>(lastToken_));
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_sound");
#endif
	return Press::Preloading;
}

bool SoundSwaps::cancelPreload() {
	if (runner.active() != &soundJob || runner.cancelling()) {
		return false;
	}
	return runner.cancel();
}

void SoundSwaps::preloaded(int32_t part, uint8_t token, const PartOutput& output, const SoundPicks& picks) {
	const Genre* genre = currentRecipe.recipe.genre;
	if (!performAdapter.active() || performAdapter.leaving_ || genre == nullptr) {
		return; // the Output goes with Perform's song
	}
	copyPartPicks(*genre, part, picks, incomingPicks);
	const Generator* piece = performAdapter.playingGenerator();
	if (piece == nullptr) {
		// Stopped: no line to wait for, so it swaps in at once, and the next start plays it.
		TEST_EVENT("gen_sound_swap part=%ld stopped token=%u", static_cast<long>(part), static_cast<unsigned>(token));
		swapIn(part, output, performAdapter.sink_.roster, INT_MIN);
		return;
	}
	// The swap waits for the next phrase line in the core (spec §13.4).
	if (!performAdapter.queue_.push({CommandKind::SetSound, static_cast<uint8_t>(part), token})) {
		retire(output, INT_MIN);
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
		return;
	}
	incoming_[part] = Incoming{.output = output, .token = token, .pushedAt = piece->nextStep()};
}

void SoundSwaps::swap(const Event& event, Roster& roster) {
	const Genre* genre = currentRecipe.recipe.genre;
	if (event.part >= kMaxParts || genre == nullptr) {
		return;
	}
	Incoming& in = incoming_[event.part];
	if (in.token == 0 || in.token != event.soundToken()) {
		return; // cancelled since it landed: the part plays on with its old sound
	}
	const int32_t phraseTicks = genre->phraseSteps() * kTicksPerStep;
	TEST_EVENT("gen_sound_swap part=%u line=%d token=%u tick=%ld", static_cast<unsigned>(event.part),
	           event.tick % phraseTicks == 0 ? 1 : 0, static_cast<unsigned>(event.soundToken()),
	           static_cast<long>(event.tick));
	// The old Output had its note-offs on this line. It rings out for at most a phrase (spec §12.9).
	swapIn(event.part, in.output, roster, event.tick + phraseTicks);
	in = Incoming{};
}

void SoundSwaps::swapIn(int32_t part, const PartOutput& output, Roster& roster, int32_t retireBy) {
	const Genre& genre = *currentRecipe.recipe.genre;
	retire(roster.parts[part], retireBy);
	roster.parts[part] = output;
	// The Current Recipe keeps the new picks, and the old ones are the part's previous sound (spec §13.4).
	copyPartPicks(genre, part, currentRecipe.sounds, previousPicks);
	copyPartPicks(genre, part, incomingPicks, currentRecipe.sounds);
	hasPrevious_[part] = true;
	soundControls.rebindPart(roster, genre, part);
	swaps_++;
}

void SoundSwaps::retire(const PartOutput& output, int32_t by) {
	if (output.output == nullptr) {
		return;
	}
	if (retiringCount_ == kMaxRetiring) {
		return; // never with one swap a part at a time; if it did, the Output stays linked until Perform's song goes
	}
	retiring_[retiringCount_++] = Retiring{.output = output, .by = by};
}

void SoundSwaps::service() {
	if (!performAdapter.active() || performAdapter.leaving_) {
		return;
	}
	const Generator* piece = performAdapter.playingGenerator();
	// A sound waiting for its line that the core no longer has, and never landed, was dropped there: a BACK tap, or
	// a full pending list. Its Output goes.
	for (int32_t p = 0; p < kMaxParts && piece != nullptr; p++) {
		Incoming& in = incoming_[p];
		if (in.token == 0 || piece->nextStep() <= in.pushedAt || piece->soundWaiting(p) == in.token
		    || piece->soundLanded(p) == in.token) {
			continue;
		}
		TEST_EVENT("gen_sound_cancel part=%ld dropped", static_cast<long>(p));
		retire(in.output, INT_MIN);
		in = Incoming{};
	}
	if (retiringCount_ == 0 || runner.busy()) {
		return;
	}
	const int32_t clock = performAdapter.clock();
	int32_t kept = 0;
	for (int32_t i = 0; i < retiringCount_; i++) {
		const Retiring& r = retiring_[i];
		if (r.by == INT_MIN || piece == nullptr || clock >= r.by || partSilent(r.output)) {
			freeJob.outputs[freeJob.count++] = r.output;
		}
		else {
			retiring_[kept++] = r;
		}
	}
	retiringCount_ = kept;
	if (freeJob.count > 0 && !runner.submit(freeJob)) {
		freeJob.count = 0; // never: the runner was free
	}
}

void SoundSwaps::playbackEnded() {
	// The stop cancelled the core's pending list, and any Sound event still buffered (spec §2.6).
	for (int32_t p = 0; p < kMaxParts; p++) {
		Incoming& in = incoming_[p];
		if (in.token != 0) {
			TEST_EVENT("gen_sound_cancel part=%ld stopped", static_cast<long>(p));
			retire(in.output, INT_MIN);
			in = Incoming{};
		}
	}
}

void SoundSwaps::forget() {
	for (Incoming& in : incoming_) {
		in = Incoming{};
	}
	retiringCount_ = 0;
	for (bool& previous : hasPrevious_) {
		previous = false;
	}
}

} // namespace deluge::generative::firmware
