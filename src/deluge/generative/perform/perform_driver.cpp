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
#include "generative/perform/riser_timing.h"
#include <algorithm>

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
	incoming_ = nullptr;
	outgoing_ = nullptr;
	transitionFrom_ = -1;
	transitionGlide_ = false;
	tempoWindow_ = false;
	riser_.held = false;
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
	riser_.held = false; // its note-off ended it there, but a jump never leaves one
	flushOutgoing(Generator::FlushFor::Jump, sink);
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
	drainOutgoing(sink);
	decideDue(queue, log, sink);
	playDue(sink, clock_);
	drainOutgoing(sink);
	glideIntoTransition(sink);
	releaseRiser(sink, clock_);
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
		if (generator_->stoppedDeciding()) {
			// The transition's start step: the incoming instance decides it (spec §12.9).
			if (incoming_ == nullptr) {
				break;
			}
			switchToIncoming(sink);
			continue;
		}
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
		if (incoming_ != nullptr && transitionFrom_ < 0 && generator_->transitionStart() >= 0) {
			transitionFrom_ = generator_->stoppedDeciding() ? generator_->nextStep() : generator_->nextStep() - 1;
		}
		if (generator_->stoppedDeciding()) {
			continue; // a cut landed: nothing was decided
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
	if (outgoing_ != nullptr && outgoing_->nextEventTick() != INT32_MAX) {
		nextDue = std::min(nextDue, outgoing_->nextEventTick() - outgoingBase_);
	}
	if (riser_.held) {
		nextDue = std::min(nextDue, riser_.startTick);
	}
	if (outgoing_ != nullptr && outgoing_->throwsFalling()) {
		nextDue = std::min(nextDue, decisionTick(outgoing_->nextStep()) - outgoingBase_);
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
		releaseRiser(sink, event.tick);
		if (riser_.held && event.kind == EventKind::NoteOff && event.part == riser_.noteOn.part
		    && event.lane == Lane::Riser) {
			// Ended before it started, by a forced Section or a jump: it never sounds (spec §9.5).
			riser_.held = false;
			continue;
		}
		if (event.kind == EventKind::NoteOn && event.lane == Lane::Riser && holdRiser(event, sink)) {
			continue;
		}
		history_.played(event);
		sink.play(event);
	}
	releaseRiser(sink, upTo);
}

bool PerformDriver::holdRiser(const Event& noteOn, NoteSink& sink) {
	// The note the core placed runs from the Build's first step to its Drop, so a synth Riser shapes itself to the
	// Build. A sample has its own length: it starts late, so its file ends on the Drop.
	if (generator_->arc().section() != SectionKind::Build) {
		return false;
	}
	const int32_t micros = sink.sampleMicros(noteOn);
	if (micros <= 0) {
		return false;
	}
	riser_ = {noteOn, stepTick(generator_->nextStep() - 1 + generator_->arc().stepsToDrop()), micros, noteOn.tick,
	          true};
	timeRiser();
	if (riser_.startTick > noteOn.tick) {
		return true;
	}
	riser_.held = false; // longer than the Build: it starts with it
	return false;
}

void PerformDriver::timeRiser() {
	if (!riser_.held) {
		return;
	}
	riser_.startTick = riserStart(riser_.noteOn.tick, riser_.dropTick, riser_.micros,
	                              [this](int32_t tick) { return plannedTempo(tick); });
}

void PerformDriver::releaseRiser(NoteSink& sink, int32_t upTo) {
	if (!riser_.held || riser_.startTick > upTo) {
		return;
	}
	riser_.held = false;
	riser_.noteOn.tick = riser_.startTick;
	history_.played(riser_.noteOn);
	sink.play(riser_.noteOn);
}

int32_t PerformDriver::plannedTempo(int32_t tick) const {
	// A transition's glide across its Build starts from wherever the tempo is when the Build starts (spec §9.7).
	if (tempoWindow_ && !transitionGlide_ && tick >= tempoFrom_) {
		const int32_t from = tempo_.valueIn(tempoFrom_ - clock_);
		if (tick >= tempoTo_) {
			return tempoTarget_;
		}
		return from + (tempoTarget_ - from) * (tick - tempoFrom_) / (tempoTo_ - tempoFrom_);
	}
	return tempo_.valueIn(tick - clock_);
}

void PerformDriver::stop(NoteSink& sink) {
	if (generator_ == nullptr) {
		return;
	}
	generator_->flush(clock_, Generator::FlushFor::Stop);
	playDue(sink, clock_);
	flushOutgoing(Generator::FlushFor::Stop, sink);
	incoming_ = nullptr; // the caller commits the transition (spec §12.9)
	transitionFrom_ = -1;
	transitionGlide_ = false;
	tempoWindow_ = false;
	riser_.held = false;
	finish(sink);
}

void PerformDriver::queueIncoming(Generator& incoming, int32_t cutGlideTicks) {
	incoming_ = &incoming;
	cutGlideTicks_ = cutGlideTicks > 0 ? cutGlideTicks : 1;
	transitionFrom_ = -1;
	transitionGlide_ = false;
	tempoWindow_ = false;
}

void PerformDriver::cancelIncoming(NoteSink& sink) {
	if (incoming_ == nullptr) {
		return;
	}
	incoming_ = nullptr;
	if (transitionGlide_ && generator_ != nullptr) {
		glideTempo(generator_->recipe().tempo, cutGlideTicks_, sink);
	}
	transitionGlide_ = false;
	tempoWindow_ = false;
	transitionFrom_ = -1;
	timeRiser();
}

void PerformDriver::glideIntoTransition(NoteSink& sink) {
	if (incoming_ != nullptr && !transitionGlide_) {
		const bool window = tempoWindow_;
		const int32_t from = tempoFrom_;
		const int32_t to = tempoTo_;
		// Across the Build or the Outro once it has landed; a cut's ends on the cut line, over the Glide setting.
		tempoWindow_ = false;
		if (transitionFrom_ >= 0) {
			tempoWindow_ = true;
			tempoFrom_ = stepTick(transitionFrom_);
			tempoTo_ = stepTick(generator_->transitionStart());
		}
		else if (const Command* waiting = generator_->waitingTransition();
		         waiting != nullptr && static_cast<TransitionStyle>(waiting->target) == TransitionStyle::Cut) {
			tempoWindow_ = true;
			tempoTo_ = stepTick(generator_->landsAt(*waiting));
			tempoFrom_ = tempoTo_ - cutGlideTicks_;
		}
		tempoTarget_ = incoming_->recipe().tempo;
		if (window != tempoWindow_ || from != tempoFrom_ || to != tempoTo_) {
			timeRiser(); // the Build's glide is planned now
		}
	}
	// A short cut glide may start after the switch, 12 ticks before the cut line.
	if (!tempoWindow_ || transitionGlide_ || clock_ < tempoFrom_) {
		return;
	}
	transitionGlide_ = true;
	tempoWindow_ = false;
	glideTempo(tempoTarget_, std::max<int32_t>(tempoTo_ - clock_, 0), sink);
	tempoStarting_ = false; // this call's ticks have gone by already: it counts from the clock now
	timeRiser();
}

void PerformDriver::switchToIncoming(NoteSink& sink) {
	const int32_t startTick = stepTick(generator_->transitionStart());
	flushOutgoing(Generator::FlushFor::Stop, sink); // never one left: a third roster is refused (spec §12.9)
	outgoing_ = generator_;
	outgoingBase_ = startTick;
	// The new roster plays the macros as they are now (spec §12.9), a turn since the confirm included, and keeps a
	// hold (spec §9.7).
	incoming_->carryOver(generator_->recipe().macros, generator_->arc().held());
	// A Riser still held this close to the Drop is shorter than the lookahead: it never sounds.
	riser_.held = false;
	generator_ = incoming_;
	incoming_ = nullptr;
	transitionFrom_ = -1;
	clock_ -= startTick;
	swingLine_ -= startTick;
	drainedEndLine_ = -1;
	history_.begin(generator_->recipe().genre->phraseSteps(), outgoing_->arc().section());
	if (tempoWindow_) {
		tempoFrom_ -= startTick;
		tempoTo_ -= startTick;
	}
	sink.transitionStarts(startTick);
	// Swing switches on the start step, in one step: never a glide under a Build roll (spec §9.7).
	glideSwing(generator_->recipe().swing, 0, sink);
}

void PerformDriver::drainOutgoing(NoteSink& sink) {
	if (outgoing_ == nullptr) {
		return;
	}
	// Its parts' Throws hold and fall on past the start step (spec §11.4): one step at each step's decision tick.
	while (outgoing_->throwsFalling() && decisionTick(outgoing_->nextStep()) <= clock_ + outgoingBase_) {
		outgoing_->fallThrows();
	}
	Event event;
	while (outgoing_->popDue(clock_ + outgoingBase_, &event)) {
		if (event.kind == EventKind::NoteOn && event.muted) {
			continue;
		}
		sink.playOutgoing(event);
	}
	if (outgoing_->isDrained()) {
		outgoing_ = nullptr;
		sink.outgoingDrained();
	}
}

void PerformDriver::flushOutgoing(Generator::FlushFor why, NoteSink& sink) {
	if (outgoing_ == nullptr) {
		return;
	}
	outgoing_->flush(clock_ + outgoingBase_, why);
	drainOutgoing(sink);
	if (outgoing_ != nullptr) {
		outgoing_ = nullptr; // a flush leaves only events due now, so never: but it goes either way
		sink.outgoingDrained();
	}
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
	timeRiser();
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
	timeRiser();
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
