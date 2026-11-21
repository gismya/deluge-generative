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

#include "generative/core/generator.h"
#include <algorithm>
#include <iterator>
#ifdef GENERATOR_TEST_SEAMS
#include <cassert>
#endif

namespace deluge::generative {

GEN_STEP_CODE DrawKey onPick(DrawKey key, const PatternState& state, uint16_t index, uint32_t step) {
	key.position[0] = state.pick.groove;
	key.position[1] = static_cast<uint32_t>(state.pick.phrase) << 16 | index;
	key.position[2] = static_cast<uint32_t>(static_cast<uint8_t>(state.entry)) << 24 | (step & 0xFFFFFFU);
	return key;
}

void Generator::start(const Recipe& recipe, uint32_t seed, const Takes& takes, bool skipIntro) {
	begin(recipe, seed, takes, skipIntro ? SectionKind::Groove : SectionKind::Intro, false);
}

void Generator::begin(const Recipe& recipe, uint32_t seed, const Takes& takes, SectionKind first, bool onDrop) {
	recipe_ = recipe;
	// A shift the part's octave range doesn't take (a Recipe from elsewhere) plays at the nearest it does (spec §7.3).
	for (int32_t p = 0; p < kMaxParts; p++) {
		recipe_.octave[p] = static_cast<int8_t>(
		    p < partCount(*recipe.genre) ? octaveRange(recipe.genre->parts[p]).clamp(recipe_.octave[p]) : 0);
	}
	seed_ = seed;
	takes_ = takes;
	genre_ = recipe.genre;
	nextStep_ = 0;
	stopAt_ = kNever;
	restartLanded_ = false;
	arc_.start(*genre_, first, onDrop);
	// A stop cancels the pending list: every start is fresh (spec §2.6, §13.2).
	pending_.clear();
	mutes_ = {}; // mutes and solos are performance state: lost on stop (spec §13.1)
	frozen_ = 0;
	silent_ = 0;
	std::fill(std::begin(landedSound_), std::end(landedSound_), static_cast<uint8_t>(kNoSound));
	autoFill_ = -1;
	heldFill_ = -1; // the driver sends FILL again if it is still held
	rested_ = 0;
	busyHalves_[0] = {};
	busyHalves_[1] = {};
	filled_ = false;
	// A stop clears a drop-out playing or waiting: the next Start begins with none (spec §8.8).
	dropOut_ = -1;
	sirenSounding_ = false;
	hasSiren_ = genreHasSiren(*genre_);
	dropOutFrom_ = 0;
	dropOutTo_ = 0;
	dropOutAfter_ = 0;
	for (PartState& part : parts_) {
		part = PartState{};
	}
	// Every control starts at home, as the driver has the sounds when a piece starts (spec §11.6).
	controlCount_ = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const PartSpec& part = genre_->parts[p];
		for (int32_t set = 0; set < controlSetsOf(part) && controlCount_ < kMaxControlSets; set++) {
			ControlSet& control = controls_[controlCount_++];
			control = ControlSet{};
			control.part = static_cast<uint8_t>(p);
			control.lane = controlSetLane(part, set);
			control.kind = controlSetKind(part, set);
			control.delaySetup = throwsDelay(part, control.lane);
		}
	}
	closeWashFrom_ = kNever;
	previousChord_ = -1;
	chordCount_ = 0;
	// Home, at the Extension level this Recipe's Complexity gives, so chord() reads a voiced chord before step 0. Step
	// 0's bar line voices it again, from the macros then: a transition's carryOver() replaces them after begin().
	voiceChordAt(kHomeChord);
	driftPoint_ = 0;
	driftTowards_ = 0;
	driftLastMotion_ = 0;
	std::fill(std::begin(lastSpaceMacro_), std::end(lastSpaceMacro_), int16_t{0});
	// A part's tables hold what the event buffer counts for its role, so a fuller one evicts (spec §2.7).
	int32_t slots[kMaxParts] = {};
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		slots[p] = maxSounding(genre_->parts[p].role);
	}
	sounding_.layout(slots);
	held_.layout(slots);
	events_.clear();
	dropped_ = 0;
	refused_ = 0;
	evicted_ = 0;
}

void Generator::startIncoming(const Recipe& recipe, uint32_t seed, const Takes& takes, TransitionStyle style) {
	begin(recipe, seed, takes, style == TransitionStyle::Fade ? SectionKind::Intro : SectionKind::Groove,
	      style == TransitionStyle::BuildIntoDrop);
}

void Generator::carryOver(const Macros& macros, const PartBiases& bias, bool held) {
	recipe_.macros = macros;
	recipe_.bias = bias;
	arc_.carryHold(held);
}

GEN_STEP_CODE Chord Generator::chord() const {
	return genre_ == nullptr ? voiceChord(recipe_.scale, kHomeChord, 0) : voiced_;
}

GEN_STEP_CODE void Generator::voiceChordAt(int32_t root) {
	voiced_ = voiceChord(recipe_.scale, root,
	                     extensionLevel(recipe_.macros[Macro::Complexity], genre_->harmony.extensionFloor,
	                                    genre_->harmony.extensionCeiling),
	                     {}, genre_->harmony.extensionOrder);
}

GEN_STEP_CODE int32_t Generator::stepsToChordLine() const {
	// The harmonic rhythm's grid, counted in the phrase, or for chords longer than a phrase, in phrases from the
	// Section start.
	const int32_t rhythmSteps = genre_->harmony.graph->barsPerChord * kStepsPerBar;
	const int32_t phraseSteps = genre_->phraseSteps();
	if (rhythmSteps <= phraseSteps) {
		return (rhythmSteps - arc_.stepInPhrase() % rhythmSteps) % rhythmSteps;
	}
	const int32_t phrasesPerChord = rhythmSteps / phraseSteps;
	const int32_t toPhraseLine = (phraseSteps - arc_.stepInPhrase()) % phraseSteps;
	const int32_t phrase = arc_.phraseInSection() + (toPhraseLine > 0 ? 1 : 0);
	return toPhraseLine + (phrasesPerChord - phrase % phrasesPerChord) % phrasesPerChord * phraseSteps;
}

GEN_STEP_CODE int32_t Generator::walkChord(int32_t step) {
#ifdef GENERATOR_TEST_SEAMS
	// waitingMove() keeps only the first move, and that holds because every move still waiting on a phrase line is a
	// Build whose Drop is the next phrase line. A move that lands elsewhere breaks it. Checked on every phrase line of
	// every genre, not only where a chord longer than a phrase changes, so every genre's play tests it.
	if (arc_.stepInPhrase() == 0) {
		for (int32_t i = 0; i < pending_.size(); i++) {
			Arc::Move move;
			assert(!moveAt(pending_.at(i), move)
			       || (move.section == SectionKind::Build && !move.stops
			           && move.at + move.buildSteps == genre_->phraseSteps()));
		}
	}
#endif
	const ChordGraph* graph = genre_->harmony.graph;
	const int32_t current = chordRoot();
	if (graph == nullptr) {
		return current;
	}
	// A chord line: a line on the harmonic rhythm's grid, or a Section start, except that a chord longer than a phrase
	// only changes on a phrase line, so it never straddles one. A Section start off it only restarts the cadence count.
	const int32_t phraseSteps = genre_->phraseSteps();
	const int32_t rhythmSteps = graph->barsPerChord * kStepsPerBar;
	const bool spansPhrases = rhythmSteps > phraseSteps;
	const bool sectionStart = arc_.sectionStartsHere();
	const int32_t toGridLine = stepsToChordLine();
	if (toGridLine != 0 && !(sectionStart && (!spansPhrases || arc_.stepInPhrase() == 0))) {
		if (sectionStart) {
			chordCount_ = 0;
		}
		return current;
	}

	// Steps to the next chord line: the grid's, or a Section start known now if one comes first and is a chord line.
	// The Arc's own move as things stand, or a waiting force, End or transition that replaces it.
	const Arc::Ahead ahead = arc_.ahead(recipe_.macros[Macro::Motion], waitingMove());
	int32_t chordSteps = toGridLine != 0 ? toGridLine : rhythmSteps;
	if (ahead.start < chordSteps && (!spansPhrases || (arc_.stepInPhrase() + ahead.start) % phraseSteps == 0)) {
		chordSteps = ahead.start;
	}

	ChordPlace place;
	place.home = step == 0 || arc_.dropStartsHere();
	place.sectionStart = sectionStart;
	// The phrase rules: only when a phrase holds 2 or more chords. Its last is the chord that reaches the phrase line.
	place.phraseFirst = arc_.stepInPhrase() == 0 && chordSteps < phraseSteps;
	place.phraseLast = arc_.stepInPhrase() != 0 && arc_.stepInPhrase() + chordSteps >= phraseSteps;
	const int32_t period = graph->cadencePeriod;
	chordCount_ = static_cast<uint8_t>(sectionStart ? 1 : chordCount_ % std::max<int32_t>(period, 1) + 1);
	place.cadence = period > 0 && chordCount_ == period;
	place.beforeDrop = ahead.drop <= chordSteps;
	place.complexity = recipe_.macros[Macro::Complexity];

	DrawKey key = partKey(seed_, kArcPart, Lane::None, 0, Purpose::ChordWalk);
	key.position[0] = static_cast<uint32_t>(arc_.section()) << 16 | arc_.occurrence();
	key.position[1] = static_cast<uint32_t>(arc_.phraseInSection());
	key.position[2] = static_cast<uint32_t>(arc_.barInPhrase());
	const int32_t next = nextChord(*graph, recipe_.scale, current, previousChord_, place, draw(key),
	                               genre_->harmony.guard, colourMoves(recipe_.scale));
	if (next != current) {
		previousChord_ = static_cast<int8_t>(current);
	}
	return next;
}

GEN_STEP_CODE int32_t Generator::effectiveMacro(Macro macro) const {
	return effectiveOn(*genre_, arc_, recipe_.macros, macro);
}

GEN_STEP_ENTRY void Generator::decideNextStep(SteeringQueue& queue, SteeringLog* log) {
	const int32_t n = nextStep_;
	if (stoppedDeciding()) {
		return; // a transition's start step: the new roster decides from here
	}
	events_.beginStep();
	cutParts_ = 0; // set again below by a SetSound or SetKey landing on this step

	// Steering lands here, on the next undecided step, and is logged where it lands (ADR 0005). Bar- and
	// phrase-quantised commands wait for their line in the pending list, and land before the Arc moves, so a forced
	// Section starts on this step.
	Command command;
	while (queue.pop(&command)) {
		if (command.kind == CommandKind::CancelNewest) {
			pending_.cancelNewest(); // a cancelled action never reaches the log
		}
		else if (landsNow(command)) {
			// A hold press, or a SHIFT mute or solo press, lands here as the other state, unless it cancelled a press
			// waiting for its line.
			if (resolvePress(command) && apply(command) && log != nullptr) {
				log->append(n, command);
			}
		}
		else if (isQuantised(command.kind)) {
			refused_ += wait(command) ? 0 : 1;
		}
		else if (apply(command) && log != nullptr) {
			log->append(n, command);
		}
	}
	landPending(log);
	if (stoppedDeciding()) {
		endAtStop(); // a cut landed: its line is the start step
		return;
	}

	Step step{};
	step.index = n;
	step.tick = stepTick(n);
	if (arc_.enterStep(*genre_, seed_, n, recipe_.macros)) {
		push(Event::sectionStart(step.tick, arc_.section(), arc_.occurrence()));
	}
	if (n == 0) {
		push(Event::keyScale(step.tick, recipe_.keyRoot, recipe_.scale));
	}
	if (arc_.stepInBar() == 0) {
		// Chords and their Extensions change on bar lines: a Complexity turn mid-bar waits for the next (spec §7.4).
		voiceChordAt(walkChord(n));
	}
	step.energy = effectiveMacro(Macro::Energy);
	step.density = effectiveMacro(Macro::Density);
	step.sharedDensity = step.density;
	step.complexity = recipe_.macros[Macro::Complexity];
	step.motion = recipe_.macros[Macro::Motion];
	// The Intro and the first Groove share the first pick; Breaks and Builds keep what was playing.
	if (arc_.section() == SectionKind::Groove && arc_.stepInPhrase() == 0) {
		step.drop = arc_.dropStartsHere();
		// A Groove's first step re-picks by chance too when no Drop opens it (one forced from a Break, say), except the
		// first Groove, which shares the Intro's pick.
		step.repickLine = !arc_.sectionStartsHere() || (!step.drop && arc_.occurrence() > 0);
		step.groove = arc_.occurrence();
		step.phrase = static_cast<uint16_t>(arc_.phraseInSection());
	}
	step.sectionStart = arc_.sectionStartsHere();
	if (arc_.stepInBar() == 0) {
		updateGates(step);
	}
	if (arc_.sectionStartsHere()) {
		cutSectionLine(step);
	}
	sectionRules(step);
	dropOutRules(step);
	fillRules(step);
	updateSilent(step);

	// Parts in the genre's roster order, which is role order unless the genre says otherwise (spec §5.5).
	uint8_t avoidedParts = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		// Each part at its own Part bias (spec §3.3); the shared step above stays the Arc's, the fills' and the
		// sound controls' drift.
		avoidedParts |= decidePart(p, recipe_.bias.any(p) ? partStep(p, step) : step) ? partBit(p) : 0;
	}
	if (arc_.stepInBar() == 0) {
		lateEntries(step, avoidedParts);
	}
#ifdef GENERATOR_TEST_SEAMS
	if (testDecided_ != nullptr) {
		testDecided_(*this, n, step.density);
	}
#endif
	updateControls(step);

	queueNoteOffsBefore(decisionTick(n + 1));
	nextStep_ = n + 1;
	if (stoppedDeciding()) {
		endAtStop(); // the Build's or the Outro's last step
	}
}

GEN_STEP_CODE void Generator::endAtStop() {
	const int32_t stopTick = stepTick(stopAt_);
	sounding_.takeEndingBy(INT32_MAX, [this, stopTick](int32_t part, SoundingNote& note) {
		if (note.onTick < stopTick) {
			note.offTick = std::min(note.offTick, stopTick);
		}
		return push(note.noteOff(static_cast<uint8_t>(part)));
	});
	held_.clear(); // nothing comes back late: the new roster plays from here
}

void Generator::takeQueued(SteeringQueue& queue) {
	Command command;
	for (int32_t n = queue.size(); n > 0 && queue.pop(&command); n--) {
		if (command.kind == CommandKind::CancelNewest) {
			pending_.cancelNewest();
		}
		else if (isQuantised(command.kind) && !landsNow(command)) {
			refused_ += wait(command) ? 0 : 1;
		}
		else {
			queue.push(command); // back at the end: the next decision, if any, drains it
		}
	}
}

void Generator::skipTo(int32_t step) {
	while (nextStep_ < step) {
		arc_.enterStep(*genre_, seed_, nextStep_, recipe_.macros);
#ifdef GENERATOR_TEST_SEAMS
		// Callers stop at the next bar line: crossing one would skip the chord walk and the voicing done there.
		assert(arc_.stepInBar() != 0);
#endif
		nextStep_++;
	}
}

GEN_STEP_CODE Generator::Step Generator::partStep(int32_t partIndex, const Step& step) const {
	Step own = step;
	own.energy = biasedOn(partIndex, Macro::Energy, step.energy);
	own.density = biasedOn(partIndex, Macro::Density, step.density);
	own.complexity = biasedOn(partIndex, Macro::Complexity, step.complexity);
	own.motion = biasedOn(partIndex, Macro::Motion, step.motion);
	return own;
}

GEN_STEP_CODE bool Generator::decidePart(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	// Respond's halves, counted before the part decides. A Kit part lists no rules (genreValid()): decideKit() counts
	// each lane's.
	if (spec.lanes.empty()) {
		countHalves(partIndex, -1, step);
	}
	if (!spec.lanes.empty()) {
		decideKit(partIndex, step);
	}
	else if (spec.flavour == Flavour::Loop) {
		decideLoop(partIndex, step);
	}
	else if (spec.flavour == Flavour::Sustain) {
		decideSustain(partIndex, step);
	}
	else if (spec.flavour == Flavour::Stab) {
		decideStab(partIndex, step);
	}
	else if (spec.flavour == Flavour::Arp) {
		decideArp(partIndex, step);
	}
	else if (spec.flavour == Flavour::Motif) {
		decideMotif(partIndex, step);
	}
#ifdef GENERATOR_TEST_SEAMS
	else if (testFlavour_ != nullptr) {
		testFlavour_(*this, partIndex, step);
	}
#endif
	parts_[partIndex].newTake = false;
	// Read after the part's pick, which a phrase line may change.
	return endAvoided(partIndex, step);
}

GEN_STEP_CODE bool Generator::endAvoided(int32_t partIndex, const Step& step) {
	// A note from an earlier step never sounds into a step Avoid leaves free of the part (spec §5.5).
	if (!avoided(partIndex, step)) {
		return false;
	}
	const PartSpec& spec = genre_->parts[partIndex];
	if (spec.flavour != Flavour::Loop) {
		sounding_.endPitchedBy(partIndex, step.tick);
		return true;
	}
	// A Loop note plays plain: the note that started at the part's last onset (PartState::loopOn: a tie's last
	// segment, since extend() keeps the first segment's onTick) ends at its gate or the line, whichever comes first.
	// That is the note that can sound into this step (§8.6's overlap rule); one the Section line has already cut there
	// counts too. It can't end before this step's decision tick: a note-off due earlier has already gone out. Notes
	// that start after loopOn, a ratchet's later hits and a late entry's re-strike, end on the line at most, so a
	// ratchet stays inside its 16th. No cut ever ends a note before it starts.
	const int32_t loopOn = parts_[partIndex].loopOn;
	const int32_t plain = std::clamp<int32_t>(loopOn + spec.loop.gateTicks, decisionTick(step.index), step.tick);
	sounding_.forEach(partIndex, [&](SoundingNote& note) {
		if (note.offTick >= step.tick) {
			const int32_t end = note.onTick > loopOn ? step.tick : plain;
			SoundingNotes::cutAt(note, std::max<int32_t>(end, note.onTick + 1));
		}
	});
	return true;
}

// ---- Note bookkeeping (spec §2.7, §12.5) ----

GEN_STEP_CODE void Generator::noteOn(int32_t partIndex, const Note& note) {
	const uint8_t part = static_cast<uint8_t>(partIndex);
	const Lane lane = note.lane;
	const uint8_t pitch = note.pitch;
	const int32_t onTick = note.onTick;
	Event on = Event::noteOn(onTick, part, lane, pitch, note.velocity);
	on.ornament = note.ornament;
	on.muted = !mutes_.audible(partIndex);

	// A note held back on this key for late entry won't need it: this one takes its place.
	held_.release(on);

	// Under overflow (bad data only: the buffer is sized for the worst case, generator.h), this note-on can be pushed
	// out below, by its own push or by making room for an earlier note's note-off. The side effects already done for
	// it stay: the held-back note released above, and the earlier notes on its key ended at its tick. Those notes end
	// a little early with nothing starting there. That's accepted: a dropped event is counted, and a harness spec
	// fails on any.

	PushResult result = events_.pushNoteOn(on, genre_->parts[partIndex].role);
	if (result.dropped) {
		dropped_++;
		if (!result.stored) {
			return; // nothing else changes for a note that won't play
		}
		forgetUnplayed(result.droppedEvent);
	}

	// Stores a note-off the new note needs room for. @return Whether it was stored: then its note has ended and
	// leaves the table. Making room for it may push out the new note-on itself, which sets `onDropped`.
	bool onDropped = false;
	auto pushNoteOff = [&] GEN_STEP_CODE(const Event& off) {
		PushResult pushed = events_.push(off);
		if (!pushed.dropped) {
			return true;
		}
		dropped_++;
		if (!pushed.stored) {
			return false;
		}
		if (pushed.droppedEvent == on) {
			onDropped = true;
		}
		else {
			forgetUnplayed(pushed.droppedEvent);
		}
		return true;
	};

	// The same key sounding again ends the earlier note no later than this one starts, and that note-off goes out
	// now, freeing its slot: a Kit lane retriggered every step never needs a second slot (spec §2.7).
	sounding_.endKeyBy(on);
	for (int32_t i = 0; i < events_.size(); i++) {
		Event& queued = events_.at(i);
		if (queued.kind == EventKind::NoteOff && queued.sameKey(on) && queued.tick > onTick) {
			queued.tick = onTick;
		}
	}
	bool keyFree = true; // every earlier note on the key has its note-off stored
	sounding_.takeKeyEndingBy(on, [&](const SoundingNote& note) {
		keyFree = keyFree && pushNoteOff(note.noteOff(part));
		return keyFree;
	});

	SoundingNote* slot = keyFree && !onDropped ? sounding_.freeSlot(partIndex) : nullptr;
	if (keyFree && !onDropped && slot == nullptr) {
		// The part's notes that end by the time this one starts leave now, not when the step's note-offs go out
		// (queueNoteOffsBefore): the table holds what sounds at once, not what is decided 12 ticks ahead. A slid note
		// leaned late still sounds when the onset after next is decided (spec §2.7).
		sounding_.takeEndingBy(partIndex, onTick,
		                       [&](const SoundingNote& note) { return pushNoteOff(note.noteOff(part)); });
		slot = onDropped ? nullptr : sounding_.freeSlot(partIndex);
	}
	if (keyFree && !onDropped && slot == nullptr) {
		// A full table: the part's oldest note gets a note-off before the new note-on, but never before its own
		// note-on. Only bad data gets here, so it is counted. A roster past the pool (past kLargestRoster, which
		// rosterFits() rejects) can leave a part no slots at all: its notes don't play.
		evicted_++;
		SoundingNote* oldest = sounding_.oldest(partIndex);
		if (oldest != nullptr) {
			oldest->offTick = onTick > oldest->onTick ? onTick : oldest->onTick + 1;
			if (pushNoteOff(oldest->noteOff(part))) {
				oldest->used = false;
				slot = onDropped ? nullptr : oldest;
			}
		}
	}
	if (onDropped) {
		return; // pushed out to store a note-off, and already counted
	}
	if (slot == nullptr) {
		// The new note can't be tracked, so it doesn't play.
		for (int32_t i = events_.size() - 1; i >= 0; i--) {
			if (events_.at(i) == on) {
				events_.removeAt(i);
				dropped_++;
				break;
			}
		}
		return;
	}
	*slot = SoundingNote{.onTick = onTick,
	                     .offTick = note.offTick,
	                     .baseVelocity = note.baseVelocity,
	                     .lane = lane,
	                     .pitch = pitch,
	                     .used = true};
}

GEN_STEP_CODE bool Generator::push(const Event& event) {
	PushResult result = events_.push(event);
	if (result.dropped) {
		dropped_++;
		if (result.stored) {
			forgetUnplayed(result.droppedEvent);
		}
	}
	return result.stored;
}

GEN_STEP_CODE void Generator::forgetUnplayed(const Event& noteOn) {
	if (sounding_.forget(noteOn)) {
		return;
	}
	// Its note-off is already queued: the first one on its key after it starts.
	int32_t off = -1;
	for (int32_t i = 0; i < events_.size(); i++) {
		const Event& e = events_.at(i);
		if (e.kind == EventKind::NoteOff && e.sameKey(noteOn) && e.tick > noteOn.tick
		    && (off < 0 || e.tick < events_.at(off).tick)) {
			off = i;
		}
	}
	if (off >= 0) {
		events_.removeAt(off);
	}
}

void Generator::queueNoteOffsBefore(int32_t tick) {
	sounding_.takeEndingBy(
	    tick, [this](int32_t part, SoundingNote& note) { return push(note.noteOff(static_cast<uint8_t>(part))); });
}

bool Generator::popDue(int32_t now, Event* out) {
	return events_.popDue(now, out);
}

void Generator::flush(int32_t now, FlushFor why) {
	// Everything still buffered is due after `now`, so it hasn't played. Forget each unplayed note-on's note,
	// earliest first.
	while (true) {
		int32_t earliest = -1;
		for (int32_t i = 0; i < events_.size(); i++) {
			const Event& e = events_.at(i);
			if (e.kind == EventKind::NoteOn && (earliest < 0 || e.tick < events_.at(earliest).tick)) {
				earliest = i;
			}
		}
		if (earliest < 0) {
			break;
		}
		Event on = events_.at(earliest);
		events_.removeAt(earliest);
		forgetUnplayed(on);
	}

	// What's left: note-offs for notes that started, which now end at `now`. On a jump, key changes, Section starts
	// and control targets the instance has already made play at `now` too, as the piece goes on in them (the driver
	// mirrors the key, spec §12.8, the display shows the Section, and controls are sent only when they change), and so
	// does a sound swap. On a stop, nobody heard them: they go, and the driver puts every sound back at home and frees
	// a sound that never swapped in.
	for (int32_t i = 0; i < events_.size();) {
		Event& e = events_.at(i);
		const bool heardLater = why == FlushFor::Jump
		                        && (e.kind == EventKind::KeyScale || e.kind == EventKind::Section
		                            || e.kind == EventKind::Control || e.kind == EventKind::Sound);
		if (e.kind == EventKind::NoteOff || heardLater) {
			e.tick = now;
			i++;
		}
		else {
			events_.removeAt(i);
		}
	}
	events_.beginStep();
	sounding_.takeEndingBy(INT32_MAX, [this, now](int32_t part, SoundingNote& note) {
		note.offTick = now;
		return push(note.noteOff(static_cast<uint8_t>(part)));
	});
}

} // namespace deluge::generative
