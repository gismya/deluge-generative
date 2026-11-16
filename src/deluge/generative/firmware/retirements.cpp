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

#include "generative/firmware/retirements.h"
#include "generative/core/genre.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS Retirements retirements{};

namespace {

/// One beat at the song's tempo now: in its internal ticks, and in samples.
int32_t beatTicks() {
	return kStepsPerBeat * (6 << currentSong->getInputTickMagnitude());
}
uint32_t beatSamples() {
	return static_cast<uint32_t>(
	    std::min<uint64_t>(uint64_t(beatTicks()) * currentSong->getTimePerTimerTickRounded(), INT32_MAX));
}

/// What the free job frees, in SDRAM: the job object stays in internal RAM (it has a vtable, which `.sdram_bss`
/// never gets).
struct Freeing {
	PartOutput outputs[Retirements::kCapacity];
	Retirements::Owner owners[Retirements::kCapacity];
	int32_t count;
};
PLACE_SDRAM_BSS Freeing freeing;

} // namespace

/// Frees retired and cancelled Outputs (spec §12.9): live, and never stopped once it starts. Leaving waits for it.
class FreeJob final : public Job {
public:
	FreeJob() : Job(Kind::Live) {}

	JobOutcome run(Chunks& chunks) override {
#if ENABLE_SYSEX_TEST
		if (TestPort::freeHeld()) {
			// Test hook: wait here, yielding, so a script's LEAVE lands while the job runs.
			TEST_EVENT("gen_free_held outputs=%ld", static_cast<long>(freeing.count));
			while (TestPort::freeHeld()) {
				chunks.boundary();
			}
		}
#endif
		for (int32_t i = 0; i < freeing.count; i++) {
			freePartOutput(performAdapter.song_, freeing.outputs[i]);
#if ENABLE_SYSEX_TEST
			if (retirements.referenceFound_) {
				// The release failed, and freePartOutput()'s check said so (gen_retire_referenced): nothing more is
				// freed, so a script fails loudly. This Output and the ones after it stay linked, and go with
				// Perform's song.
				return JobOutcome::Done;
			}
#endif
			chunks.boundary();
		}
		// Their samples too: memory comes back once the old sound is freed (spec §20).
		deleteUnusedAudioFiles();
		return JobOutcome::Done;
	}
	[[nodiscard]] bool cancellable() const override { return false; }
	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		if (retirements.referenceFound_) {
			// No gen_outputs_freed after a failed check, not even for the Outputs this job freed before the one that
			// failed: gen_retire_referenced is the batch's only event.
			freeing.count = 0;
			return;
		}
#endif
		TEST_EVENT("gen_outputs_freed outputs=%ld", static_cast<long>(outcome == JobOutcome::Done ? freeing.count : 0));
		if (outcome != JobOutcome::Done) {
			// Its task couldn't start (run() never ran): the Outputs wait for the next service() to try again.
			for (int32_t i = 0; i < freeing.count; i++) {
				retirements.retire(freeing.outputs[i], Retirements::When::Now, 0, freeing.owners[i]);
			}
		}
#if ENABLE_SYSEX_TEST
		if (outcome == JobOutcome::Done) {
			reportFreeMemory("after_free");
		}
#endif
		freeing.count = 0;
	}
	/// Dropped from the next slot (leaving): the Outputs stay linked, and go with Perform's song.
	void dropped() override { freeing.count = 0; }
};

namespace {
FreeJob freeJob;
} // namespace

uint32_t Retirements::phraseSamples(const Genre& genre) {
	const uint64_t ticks = static_cast<uint64_t>(genre.phraseSteps()) * (6U << currentSong->getInputTickMagnitude());
	return static_cast<uint32_t>(std::min<uint64_t>(ticks * currentSong->getTimePerTimerTickRounded(), INT32_MAX));
}

void Retirements::retire(const PartOutput& output, When when, uint32_t capSamples, Owner owner) {
	if (output.output == nullptr) {
		return;
	}
	if (count_ == kCapacity) {
		// Never, by kCapacity's count: the Output would stay linked until Perform is left.
		TEST_EVENT("gen_retire_overflow");
		return;
	}
	const uint32_t until = AudioEngine::audioSampleTimer + (when == When::AfterTail ? capSamples : 0);
	retiring_[count_++] = Retiring{.output = output, .when = when, .owner = owner, .until = until};
}

int32_t Retirements::pending(Owner owner) const {
	int32_t n = 0;
	for (int32_t i = 0; i < count_; i++) {
		n += retiring_[i].owner == owner ? 1 : 0;
	}
	if (runner.active() == &freeJob || runner.next() == &freeJob) {
		for (int32_t i = 0; i < freeing.count; i++) {
			n += freeing.owners[i] == owner ? 1 : 0;
		}
	}
	return n;
}

namespace {
/// Asks the UI to let go of `output`. Whether it did.
bool uiLetGo(ReleaseUi release, const PartOutput& output) {
#if ENABLE_SYSEX_TEST
	if (TestPort::uiReleaseHeld()) {
		return true; // test hook: the UI isn't asked, so freePartOutput()'s check has something to find
	}
#endif
	return release(output) == ReleaseAnswer::Released;
}
} // namespace

void Retirements::service(ReleaseUi release) {
	if (count_ == 0) {
		return;
	}
#if ENABLE_SYSEX_TEST
	if (referenceFound_) {
		return;
	}
#endif
	const bool playing = playbackHandler.isEitherClockActive();
	// At the cap, an Output still sounding fades over a beat (spec §12.9). Its volume ramps with the song's ticks, so
	// only while playing; stopped, its tail is freed at the cap.
	for (int32_t i = 0; i < count_; i++) {
		Retiring& r = retiring_[i];
		const bool due = static_cast<int32_t>(AudioEngine::audioSampleTimer - r.until) >= 0;
		if (r.when == When::AfterTail && r.state == State::Waiting && due && playing && !partSilent(r.output)) {
			fadeOutPart(r.output, beatTicks());
			r.state = State::Fading;
			r.until = AudioEngine::audioSampleTimer + beatSamples();
			TEST_EVENT("gen_output_fade");
		}
	}
	if (runner.busy()) {
		return;
	}
	// Freed once silent, at the cap, or once faded, playing or stopped: a stop leaves tails ringing, as stock does.
	int32_t kept = 0;
	for (int32_t i = 0; i < count_; i++) {
		const Retiring& r = retiring_[i];
		const bool due = static_cast<int32_t>(AudioEngine::audioSampleTimer - r.until) >= 0;
		const bool ready = r.when == When::Now || (r.state == State::Waiting && (due || partSilent(r.output)))
		                   || (r.state == State::Fading && due);
		// The UI lets go of it first, by identity; one it still holds waits, and is asked again next time.
		if (ready && uiLetGo(release, r.output)) {
			freeing.outputs[freeing.count] = r.output;
			freeing.owners[freeing.count++] = r.owner;
		}
		else {
			retiring_[kept++] = r;
		}
	}
	count_ = kept;
	if (freeing.count > 0 && !runner.submit(freeJob)) {
		freeing.count = 0; // never: the runner was free
	}
}

void Retirements::forget() {
	count_ = 0;
#if ENABLE_SYSEX_TEST
	referenceFound_ = false;
#endif
}

} // namespace deluge::generative::firmware
