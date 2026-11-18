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

#include "generative/compose/compose_driver.h"
#include "generative/perform/riser_timing.h"

namespace deluge::generative::compose {

void ComposeDriver::begin(Generator& generator, const Recipe& recipe, uint32_t seed, const Takes& takes,
                          int32_t targetSteps, SongWriter& writer) {
	generator_ = &generator;
	writer_ = &writer;
	queue_ = SteeringQueue{};
	target_ = targetSteps;
	tempo_ = recipe.tempo;
	endQueuedAt_ = -1;
	endTick_ = 0;
	shortBuildGrooves_ = 0;
	done_ = false;
	// From Start, with the Intro, as an untouched Perform plays it (spec §15.1).
	generator.start(recipe, seed, takes);
	writer.begin(recipe);
	clips_.begin(writer);
}

bool ComposeDriver::run(int32_t steps) {
	for (int32_t i = 0; i < steps && !done_; i++) {
		if (generator_->ended()) {
			break;
		}
		const int32_t next = generator_->nextStep();
		if (endQueuedAt_ < 0 && endsHere()) {
			queue_.push({CommandKind::End, 0, 1});
			endQueuedAt_ = next;
		}
		// As Perform plays as it decides: what falls due before a decision plays first.
		writeDue(decisionTick(next) - 1);
		generator_->decideNextStep(queue_, nullptr);
	}
	if (!done_ && generator_->ended()) {
		finish();
	}
	return done_;
}

bool ComposeDriver::endsHere() {
	const Arc& arc = generator_->arc();
	const SectionKind section = arc.section();
	if (generator_->nextStep() < target_ || (section != SectionKind::Groove && section != SectionKind::Build)) {
		return false;
	}
	// At the Recipe's Motion, not the effective one, as the core asks the Arc (Generator::walkChord()).
	const Arc::Ahead ahead = arc.ahead(generator_->recipe().macros[Macro::Motion]);
	const bool capped = shortBuildGrooves_ >= kMaxShortBuildGrooves;
	if (arc.nextStepInPhrase() != 0) {
		// A Groove ending on its own off a phrase line goes into a short Build, the only Section that starts there
		// (Arc::endsBefore()): it plays on, counted.
		if (section == SectionKind::Groove && ahead.start == 1) {
			shortBuildGrooves_++;
		}
		return false;
	}
	if (section == SectionKind::Build) {
		// A Groove past the cap that saw no phrase line of its own, one phrase long, played its short Build: the
		// Outro takes the place of the Groove the Build drops into.
		return shortBuildGrooves_ > kMaxShortBuildGrooves && ahead.start == 1;
	}
	// End lands on the line it is queued before, so the Outro takes the place of the Section the Groove ends into
	// there, and nothing is cut short. A Groove that never ends by itself (Motion 0) ends on the first line.
	if (ahead.start == 1 || ahead.start == Arc::kNotAhead) {
		return true;
	}
	// Past the cap, a Groove that ends in this phrase, into a short Build, ends on this line instead.
	return capped && ahead.start <= generator_->recipe().genre->phraseSteps();
}

void ComposeDriver::writeDue(int32_t upTo) {
	Event event;
	while (generator_->popDue(upTo, &event)) {
		if (event.kind == EventKind::NoteOn && !event.muted && event.lane == Lane::Riser) {
			timeRiser(event);
		}
		clips_.take(event);
	}
}

void ComposeDriver::timeRiser(Event& noteOn) {
	// As PerformDriver::holdRiser() does.
	const int32_t drop = perform::riserDropTick(*generator_);
	if (drop < 0) {
		return;
	}
	const int32_t micros = writer_->sampleMicros(noteOn);
	if (micros <= 0) {
		return;
	}
	// The song's tempo is the Recipe's throughout: Compose writes no tempo automation.
	noteOn.tick = perform::riserStart(noteOn.tick, drop, micros, [this](int32_t /*tick*/) { return tempo_; });
}

void ComposeDriver::finish() {
	// The Outro is over: the song ends on its last line, every note released there (spec §13.2), as Perform's does.
	endTick_ = stepTick(generator_->nextStep());
	writeDue(endTick_);
	generator_->flush(endTick_, Generator::FlushFor::Stop);
	writeDue(INT32_MAX);
	clips_.finish(endTick_, [&](const Automated& automated) -> int8_t {
		if (automated.throwDelay) {
			return 0; // the Throw lifts feedback from its home
		}
		return static_cast<int8_t>(generator_->centreLayer(automated.part, automated.lane, automated.control));
	});
	done_ = true;
}

} // namespace deluge::generative::compose
