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

#include "generative/perform/perform_driver.h"

namespace deluge::generative::perform {

namespace {

/// Song ticks per 16th are 6 << magnitude and the Generator's are 6 << kGeneratorTickMagnitude, so converting is a
/// shift by their difference: never a divide (spec §12.3). Below this many bits, a song tick is a fraction of a
/// Generator tick, carried in the remainder.
constexpr int32_t fractionBits(int32_t magnitude) {
	return magnitude > kGeneratorTickMagnitude ? magnitude - kGeneratorTickMagnitude : 0;
}

/// Song ticks in a bar at `magnitude`.
constexpr int64_t barSongTicks(int32_t magnitude) {
	return magnitude >= kGeneratorTickMagnitude
	           ? int64_t{kStepsPerBar * kTicksPerStep} << (magnitude - kGeneratorTickMagnitude)
	           : int64_t{kStepsPerBar * kTicksPerStep} >> (kGeneratorTickMagnitude - magnitude);
}

/// `n` rounded up to a multiple of `unit` (> 0), for negative `n` too.
template <typename T>
constexpr T roundUp(T n, T unit) {
	const T into = ((n % unit) + unit) % unit;
	return into == 0 ? n : n + unit - into;
}

/// Song ticks from `songTick` to the master's next bar line at or after it. A divide, but only on a start or a jump.
int64_t toBarLine(int64_t songTick, int32_t magnitude) {
	return roundUp(songTick, barSongTicks(magnitude)) - songTick;
}

} // namespace

void PerformDriver::begin(Generator& generator, int32_t songTick, int32_t magnitude) {
	generator_ = &generator;
	history_.begin(generator.recipe().genre->phraseSteps());
	ended_ = false;
	drainedEndLine_ = -1;
	magnitude_ = magnitude;
	placeBefore(0, toBarLine(songTick, magnitude));
}

void PerformDriver::placeBefore(int32_t step, int64_t songTicks) {
	const int32_t shift = kGeneratorTickMagnitude - magnitude_;
	if (shift >= 0) {
		clock_ = stepTick(step) - static_cast<int32_t>(songTicks << shift);
		remainder_ = 0;
	}
	else {
		// Exact to the song tick: the remainder holds what's short of a whole Generator tick.
		int64_t total = (int64_t{stepTick(step)} << -shift) - songTicks;
		clock_ = static_cast<int32_t>(total >> -shift);
		remainder_ = static_cast<int32_t>(total & ((1 << -shift) - 1));
	}
}

int64_t PerformDriver::masterTick() const {
	const int32_t shift = kGeneratorTickMagnitude - magnitude_;
	return shift >= 0 ? int64_t{clock_} >> shift : (int64_t{clock_} << -shift) + remainder_;
}

void PerformDriver::jumpTo(int32_t songTick, int32_t magnitude, NoteSink& sink) {
	if (generator_ == nullptr) {
		return;
	}
	setMagnitude(magnitude);
	resumeAt(songTick, sink);
}

void PerformDriver::jump(NoteSink& sink) {
	if (generator_ == nullptr) {
		return;
	}
	resumeAt(masterTick(), sink);
}

void PerformDriver::resumeAt(int64_t songTick, NoteSink& sink) {
	// Note-offs at the jump (spec §12.4, §12.5), then silence: the Generator skips to its next bar line, which lands
	// on the master's.
	generator_->flush(clock_, Generator::FlushFor::Jump);
	playDue(sink, clock_);
	jumps_++;
	sink.jumped();
	const int32_t next = generator_->nextStep();
	const int32_t resume = barLineAtOrAfter(next);
	generator_->skipTo(resume);
	placeBefore(resume, toBarLine(songTick, magnitude_));
	swingLine_ = nextStepLine();
}

int32_t PerformDriver::advance(int32_t songTicks, int32_t magnitude, SteeringQueue& queue, SteeringLog* log,
                               NoteSink& sink) {
	if (generator_ == nullptr) {
		return INT32_MAX;
	}
	setMagnitude(magnitude);
	if (songTicks < 0 || songTicks > barSongTicks(magnitude)) {
		// A jump (spec §12.3): the master is somewhere else now.
		resumeAt(masterTick() + songTicks, sink);
		songTicks = 0;
	}
	const int32_t before = clock_;
	moveClock(songTicks, magnitude);
	moveGlides(clock_ - before, sink);
	decideDue(queue, log, sink);
	playDue(sink, clock_);
	if (generator_->ended() && clock_ >= stepTick(generator_->nextStep())) {
		// The Outro is over: the piece stops on its last line, every note released there (spec §13.2).
		generator_->flush(stepTick(generator_->nextStep()), Generator::FlushFor::Stop);
		playDue(sink, clock_);
		finish(sink);
		ended_ = true;
		return INT32_MAX;
	}
	return nextWait(magnitude);
}

void PerformDriver::setMagnitude(int32_t magnitude) {
	// An external clock can change the magnitude mid-play: the remainder keeps its share of a Generator tick.
	if (magnitude != magnitude_) {
		int32_t from = fractionBits(magnitude_);
		int32_t to = fractionBits(magnitude);
		remainder_ = to >= from ? remainder_ << (to - from) : remainder_ >> (from - to);
		magnitude_ = magnitude;
	}
}

void PerformDriver::moveClock(int32_t songTicks, int32_t magnitude) {
	const int32_t shift = kGeneratorTickMagnitude - magnitude;
	if (shift >= 0) {
		clock_ += songTicks << shift;
	}
	else {
		int32_t total = remainder_ + songTicks;
		clock_ += total >> -shift;
		remainder_ = total & ((1 << -shift) - 1);
	}
}

void PerformDriver::decideDue(SteeringQueue& queue, SteeringLog* log, NoteSink& sink) {
	// Play as you decide: everything due before a decision plays first, so a late call never piles several steps'
	// events into the buffer, and they play in the order they would have on time.
	while (decisionTick(generator_->nextStep()) <= clock_) {
		if (generator_->ended()) {
			// The end line is never decided, so its decision's steering drain happens here, once: a force that
			// lands there cancels the ending (spec §13.2). A later press is too late, as for any line.
			if (drainedEndLine_ == generator_->nextStep()) {
				break;
			}
			drainedEndLine_ = generator_->nextStep();
			generator_->takeQueued(queue);
			if (generator_->ended()) {
				break;
			}
		}
		const int32_t decision = decisionTick(generator_->nextStep());
		playDue(sink, decision - 1);
		if (timer_ != nullptr) {
			timer_->decisionsBegin();
		}
		generator_->decideNextStep(queue, log);
		if (timer_ != nullptr) {
			timer_->decisionsEnd(1);
		}
		history_.decided(generator_->nextStep() - 1,
		                 generator_->effectiveMacro(Macro::Energy) - generator_->recipe().macros[Macro::Energy]);
	}
}

int32_t PerformDriver::nextWait(int32_t magnitude) const {
	const int32_t shift = kGeneratorTickMagnitude - magnitude;
	// Ended, the end line still has its steering drained at its decision tick (decideDue), and only then does the
	// driver wait for the line itself, where the piece stops.
	const int32_t next = generator_->nextStep();
	const bool drained = generator_->ended() && drainedEndLine_ == next;
	int32_t nextDue = drained ? stepTick(next) : decisionTick(next);
	int32_t nextEvent = generator_->nextEventTick();
	if (nextEvent < nextDue) {
		nextDue = nextEvent;
	}
	int32_t wait = nextDue - clock_; // > 0 once everything due has been decided and played
	if (tempo_.running()) {
		wait = 1; // a tempo glide writes every tick
	}
	else if (swingDue_ && swingLine_ - clock_ < wait) {
		wait = swingLine_ - clock_;
	}
	int32_t songWait = shift >= 0 ? (wait + (1 << shift) - 1) >> shift : (wait << -shift) - remainder_;
	return songWait > 0 ? songWait : 1;
}

void PerformDriver::playDue(NoteSink& sink, int32_t upTo) {
	Event event;
	while (generator_->popDue(upTo, &event)) {
		if (event.kind == EventKind::NoteOn && event.muted) {
			continue;
		}
		history_.played(event);
		sink.play(event);
	}
}

void PerformDriver::stop(NoteSink& sink) {
	if (generator_ == nullptr) {
		return;
	}
	generator_->flush(clock_, Generator::FlushFor::Stop);
	playDue(sink, clock_);
	finish(sink);
}

void PerformDriver::finish(NoteSink& sink) {
	generator_ = nullptr;
	if (tempo_.running()) {
		tempo_.finish();
		writeTempo(tempo_.value(), sink);
	}
	if (swingDue_) {
		swing_.finish();
		sink.setSwing(swing_.value());
		swingDue_ = false;
	}
}

void PerformDriver::setTempo(int32_t tempo, NoteSink& sink) {
	tempo_.set(tempo);
	writtenTempo_ = tempo;
	sink.setTempo(tempo);
}

void PerformDriver::writeTempo(int32_t tempo, NoteSink& sink) {
	// Each write retimes the firmware's ticks: skip the ones that change nothing.
	if (tempo != writtenTempo_) {
		writtenTempo_ = tempo;
		sink.setTempo(tempo);
	}
}

void PerformDriver::setSwing(int32_t amount, NoteSink& sink) {
	swing_.set(amount);
	swingDue_ = false;
	sink.setSwing(amount);
}

void PerformDriver::glideTempo(int32_t tempo, int32_t ticks, NoteSink& sink) {
	if (generator_ == nullptr || ticks <= 0) {
		setTempo(tempo, sink);
		return;
	}
	tempo_.start(tempo, ticks);
	tempoStarting_ = true;
}

void PerformDriver::glideSwing(int32_t amount, int32_t ticks, NoteSink& sink) {
	if (generator_ == nullptr) {
		setSwing(amount, sink);
		return;
	}
	swing_.start(amount, ticks);
	swingDue_ = true;
	swingStarting_ = true;
}

int32_t PerformDriver::stepLineAtOrAfter(int32_t tick) {
	return roundUp(tick, kTicksPerStep);
}

int32_t PerformDriver::barLineAtOrAfter(int32_t step) {
	return roundUp(step, kStepsPerBar);
}

int32_t PerformDriver::nextStepLine() const {
	return stepLineAtOrAfter(clock_ + 1);
}

void PerformDriver::moveGlides(int32_t ticks, NoteSink& sink) {
	// A glide started between hook calls counts from the clock at the first call after it: the ticks this call moved
	// went by before it started.
	const int32_t tempoTicks = tempoStarting_ ? 0 : ticks;
	tempoStarting_ = false;
	const int32_t swingTicks = swingStarting_ ? 0 : ticks;
	if (swingStarting_) {
		// The first step line at or after this call: the hook often runs exactly on one, and that's the boundary.
		swingStarting_ = false;
		swingLine_ = stepLineAtOrAfter(clock_);
	}
	if (swingDue_) {
		swing_.advance(swingTicks);
		if (clock_ >= swingLine_) {
			sink.setSwing(swing_.value());
			swingDue_ = swing_.running();
			swingLine_ = nextStepLine();
		}
	}
	if (tempo_.running() && tempoTicks > 0) {
		tempo_.advance(tempoTicks);
		writeTempo(tempo_.value(), sink);
	}
}

} // namespace deluge::generative::perform
