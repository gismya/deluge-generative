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

#include "generative/firmware/preset_preview.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/core/lane_names.h"
#include "generative/core/palette.h"
#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/runner.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "processing/engines/audio_engine.h"

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS PresetPreview presetPreview{};

namespace {

/// The audition note: middle C on a synth part, the lane's drum on a Kit, at a firm velocity.
constexpr uint8_t kNotePitch = 60;
constexpr uint8_t kNoteVelocity = 100;

/// The picks a preview loads: the Current Recipe's, with the previewed preset in its part's or lane's slot.
PLACE_SDRAM_BSS SoundPicks previewPicks;

} // namespace

/// Builds the previewed preset's Output, as a sound re-roll's preload does (SoundJob), but for no roster: live, so
/// Perform works as usual meanwhile (spec §12.10), and cancellable until it ends.
class PresetPreviewJob final : public Job {
public:
	PresetPreviewJob() : Job(Kind::Live) {}

	void prepare(int32_t part, const SoundChoice& choice, uint8_t token) {
		part_ = part;
		choice_ = choice;
		token_ = token;
		error_ = Error::NONE;
		built_ = PartOutput{};
		ended_ = false;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		const Genre* genre = currentRecipe.recipe.genre;
		Song* song = performAdapter.song_;
		if (genre == nullptr || song == nullptr) {
			return JobOutcome::Failed;
		}
#if ENABLE_SYSEX_TEST
		if (TestPort::presetPreviewHeld()) {
			TEST_EVENT("gen_preset_preview held part=%ld", static_cast<long>(part_));
			while (TestPort::presetPreviewHeld() && !chunks.cancelled()) {
				chunks.boundary();
			}
			if (chunks.cancelled()) {
				return JobOutcome::Cancelled;
			}
		}
#endif
		previewPicks = currentRecipe.sounds;
		if (!chooseSound(*genre, part_, choice_, previewPicks)) {
			return JobOutcome::Failed;
		}
#if ENABLE_SYSEX_TEST
		setPreviewBuild(true);
#endif
		error_ = buildPartOutput(song, *genre, part_, previewPicks, currentRecipe.palette(), &built_, chunks);
#if ENABLE_SYSEX_TEST
		setPreviewBuild(false);
#endif
		if (error_ == Error::ABORTED_BY_USER) {
			error_ = Error::NONE;
			return JobOutcome::Cancelled; // buildPartOutput() freed what it built
		}
		return error_ == Error::NONE ? JobOutcome::Done : JobOutcome::Failed;
	}

	void finished(JobOutcome outcome) override {
		ended_ = true;
		TEST_EVENT("gen_preset_preview %s part=%ld error=%d ms=%lu path=%s",
		           outcome == JobOutcome::Done        ? "built"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<long>(part_), static_cast<int>(error_), msSince(submitted_), choice_.path.c_str());
		if (outcome == JobOutcome::Failed && error_ != Error::NONE) {
			display->displayError(error_); // a card error, as stock's preset browser shows one
		}
		if (outcome != JobOutcome::Done) {
			built_ = PartOutput{};
		}
		presetPreview.buildEnded(token_, built_);
	}

	[[nodiscard]] bool ended() const { return ended_; }

private:
	int32_t part_ = 0;
	SoundChoice choice_;
	uint8_t token_ = 0;
	Error error_ = Error::NONE;
	PartOutput built_;
	bool ended_ = false;
	uint32_t submitted_ = 0;
};

namespace {
PresetPreviewJob previewJob;
} // namespace

void PresetPreview::freePreviewOutput(PartOutput& output) {
	freePartOutput(performAdapter.song_, output);
	if (!runner.busy() || runner.active() == &previewJob) {
		deleteUnusedAudioFiles();
	}
}

void PresetPreview::show(int32_t part, const SoundChoice& choice) {
	wanted_ = choice;
	wantedPart_ = part;
	lifecycle_.want();
	service();
}

void PresetPreview::stop() {
	lifecycle_.stop();
}

void PresetPreview::service() {
	lifecycle_.service(AudioEngine::audioSampleTimer);
}

void PresetPreview::buildEnded(uint8_t token, PartOutput& output) {
	if (output.output == nullptr) {
		lifecycle_.buildEnded();
		return;
	}
	// Kept before the note plays: built() plays it on shown_.
	const PartOutput previous = shown_;
	shown_ = output;
	shownPart_ = wantedPart_;
	shownLane_ = wanted_.lane;
	if (!lifecycle_.built(token, AudioEngine::audioSampleTimer)) {
		shown_ = previous;
		TEST_EVENT("gen_preset_preview dropped part=%ld", static_cast<long>(wantedPart_));
		freePreviewOutput(output);
		return;
	}
#if ENABLE_SYSEX_TEST
	reportFreeMemory("preset_preview");
#endif
}

// ---- The adapter ----

bool PresetPreview::runnerBusy() const {
	return runner.busy();
}

bool PresetPreview::submitBuild(uint8_t token) {
	if (!performAdapter.active() || performAdapter.leaving()) {
		return false;
	}
	previewJob.prepare(wantedPart_, wanted_, token);
	// A job whose task couldn't start has ended inside submit(): nothing builds.
	return runner.submit(previewJob) && !previewJob.ended();
}

void PresetPreview::cancelBuild() {
	if (runner.active() == &previewJob) {
		runner.cancel();
	}
	else if (runner.next() == &previewJob) {
		runner.clearNext();
	}
}

void PresetPreview::freeShown() {
	TEST_EVENT("gen_preset_preview freed part=%ld", static_cast<long>(shownPart_));
	freePreviewOutput(shown_);
	shown_ = PartOutput{};
	shownPart_ = -1;
	shownLane_ = Lane::None;
}

void PresetPreview::noteOn() {
	OutputSink::playNote(shown_, noteEvent(), true);
	TEST_EVENT("gen_preset_preview note part=%ld lane=%s", static_cast<long>(shownPart_),
	           shownLane_ == Lane::None ? "-" : lanePaletteName(shownLane_));
}

void PresetPreview::noteOff() {
	OutputSink::playNote(shown_, noteEvent(), false);
	TEST_EVENT("gen_preset_preview note_off part=%ld", static_cast<long>(shownPart_));
}

Event PresetPreview::noteEvent() const {
	Event event;
	event.kind = EventKind::NoteOn;
	event.part = static_cast<uint8_t>(shownPart_);
	event.lane = shownLane_;
	event.pitch = kNotePitch;
	event.velocity = kNoteVelocity;
	return event;
}

} // namespace deluge::generative::firmware
