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

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

} // namespace

void ComposeDriver::begin(Generator& generator, const Recipe& recipe, uint32_t seed, const Takes& takes,
                          int32_t targetSteps, SongWriter& writer, bool skipIntro) {
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
	replaying_ = false;
	tempoPlan_.set(recipe.tempo);
	tempoEndDue_ = false;
	tempoMoved_ = false;
	swingPlan_.set(recipe.swing);
	interval_ = intervalBefore_ = songInterval_ = recipe.swingInterval;
	intervalTick_ = 0;
	songSwing_ = recipe.swing;
	keyRoot_ = recipe.keyRoot;
	scale_ = recipe.scale;
	songStart_ = 0;
	songExtra_ = 0;
	songs_ = 1;
	std::fill(std::begin(partCutTick_), std::end(partCutTick_), 0);
	generator.start(recipe, seed, takes, skipIntro);
	writer.begin(recipe);
	clips_.begin(writer);
}

void ComposeDriver::beginReplay(Generator& generator, const SteeringLog::Piece& piece, SongWriter& writer,
                                int64_t splitBudget) {
	const PieceStart& start = piece.start();
	begin(generator, start.recipe, start.seed, start.takes, 0, writer);
	replaying_ = true;
	splitBudget_ = splitBudget;
	generator.startPiece(start);
	reader_ = piece.reader();
	// The span: up to where the piece ended, or where the log was cut (spec §17.2). A piece still playing (none is
	// once Perform has stopped) runs to its last record.
	spanEnd_ = piece.spanEnd();
	if (spanEnd_ < 0) {
		LogRecord record;
		for (SteeringLog::Piece::Reader all = piece.reader(); all.next(record);) {
			spanEnd_ = record.step + 1;
		}
	}
	endsAtTransition_ = piece.ended() == PieceEnd::Transition && spanEnd_ == piece.endStep();
	dropped_ = 0;
}

bool ComposeDriver::run(int32_t steps) {
	if (replaying_) {
		for (int32_t i = 0; i < steps && !spanEnds(); i++) {
			writeDue(decisionTick(generator_->nextStep()) - 1);
			decideReplayed();
		}
		if (!done_ && spanEnds()) {
			if (endsAtTransition_ && !generator_->stoppedDeciding()) {
				// A hard cut's Transition sits on the start step itself: it lands there and ends every note, deciding
				// nothing (spec §17.2).
				writeDue(decisionTick(generator_->nextStep()) - 1);
				decideReplayed();
			}
			finish();
		}
		return done_;
	}
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

bool ComposeDriver::spanEnds() const {
	return generator_->nextStep() >= spanEnd_;
}

void ComposeDriver::decideReplayed() {
	const int32_t step = generator_->nextStep();
	generator_->decideReplayed([&](LogRecord& out) {
		if (!reader_.peek(out) || out.step > step) {
			return false;
		}
		reader_.next(out);
		replayed(out);
		return true;
	});
}

void ComposeDriver::replayed(const LogRecord& record) {
	const int32_t tick = stepTick(record.step);
	const Command& command = record.command;
	switch (record.kind) {
	case RecordKind::Command:
		// Driver-side steering (spec §2.6): the core keeps it in its Recipe or nothing; the song takes it here.
		switch (command.kind) {
		case CommandKind::SetTempo:
		case CommandKind::TransitionTempo: // it moves the song's tempo, not the outgoing Recipe's (spec §17.2)
			glideTempo(tick, command.value, command.length);
			break;
		case CommandKind::SetSwing:
			swingPlan_.glide(tick, command.value, command.length);
			break;
		case CommandKind::SetSwingInterval:
			intervalBefore_ = interval_;
			interval_ = command.value;
			intervalTick_ = tick;
			break;
		case CommandKind::SetHome: {
			// The part's lanes write their value again from the new home, from `tick` (ClipBuilder::endPart()).
			const auto centre = [this](const Automated& automated) { return centreOf(automated); };
			clips_.endPart(command.target, tick, centre, true);
			writer_->paramEdit(command, tick);
			clips_.resumePart(command.target, tick, true);
			break;
		}
		case CommandKind::SetParam:
			writer_->paramEdit(command, tick);
			break;
		case CommandKind::SetSetting:
			cutForSetting(command.target, tick);
			writer_->settingEdit(command, tick);
			break;
		case CommandKind::SetStutter:
			writer_->stutter(command.value != 0, tick);
			break;
		default:
			break;
		}
		break;
	case RecordKind::Waiting:
	case RecordKind::NotWaiting:
	case RecordKind::Reached: // readers never hand one over
		break;
	case RecordKind::SoundPath:
		writer_->soundPick(static_cast<uint8_t>(record.command.target), static_cast<uint8_t>(record.command.value),
		                   record.slot, record.path, record.pathLength);
		break;
	case RecordKind::SoundDropped:
		dropped_ |= partBit(record.command.target);
		break;
	case RecordKind::SettingPath:
		cutForSetting(command.target, tick);
		writer_->settingPath(command.target, command.lane, command.length, record.path, record.pathLength, tick);
		break;
	}
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
		// A Groove ending on its own off a phrase line goes into a Build shorter than a phrase: only such a Build
		// starts off a phrase line (Arc::endsBefore(), spec §9.3: a longer one starts on one). It plays on, counted.
		if (section == SectionKind::Groove && ahead.start == 1) {
			shortBuildGrooves_++;
		}
		return false;
	}
	if (section == SectionKind::Build) {
		// Only a Groove past the cap gets here: a Build on a phrase line with End not yet queued follows a Groove that
		// saw no phrase line of its own (one phrase long, into a short Build), and below the cap a Groove that ends
		// into a Build on a phrase line queues End on that line instead (today's Arc never puts a Build after any
		// other Section). It played its short Build: the Outro takes the place of the Groove the Build drops into.
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
		if (event.kind == EventKind::NoteOn && !event.muted && event.lane == Lane::Riser && !timeRiser(event)) {
			continue; // no Riser fits this Build: its note-off finds no note to end
		}
		if (replaying_) {
			shape(event);
			if (event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) {
				swingDifference(event);
			}
		}
		clips_.take(event);
	}
}

void ComposeDriver::shape(const Event& event) {
	tempoReaches(event.tick);
	switch (event.kind) {
	case EventKind::KeyScale: {
		const bool changed = event.pitch != keyRoot_ || event.scale != scale_;
		keyRoot_ = event.pitch;
		scale_ = event.scale;
		if (!changed || event.tick <= 0) {
			break;
		}
		// Key and scale are song-wide: each pitched part's clip is cut where the change lands (spec §17.6).
		const Genre& genre = *generator_->recipe().genre;
		for (int32_t p = 0; p < partCount(genre); p++) {
			if (genre.parts[p].synth()) {
				clips_.cutPart(static_cast<uint8_t>(p), event.tick,
				               [this](const Automated& automated) { return centreOf(automated); });
				partCutTick_[p] = std::max(partCutTick_[p], event.tick);
				writer_->cutClip(static_cast<uint8_t>(p), event.tick);
				songExtra_ += kSongCosts.synthClip;
			}
		}
		break;
	}
	case EventKind::Sound: {
		const uint8_t bit = partBit(event.part);
		if ((dropped_ & bit) != 0) {
			dropped_ &= static_cast<uint8_t>(~bit); // a confirm dropped it: the part keeps its old track (spec §17.2)
			break;
		}
		// Every track has one sound for its whole life (spec §17.6).
		clips_.cutPart(event.part, event.tick, [this](const Automated& automated) { return centreOf(automated); });
		partCutTick_[event.part] = std::max(partCutTick_[event.part], event.tick);
		writer_->newTrack(event.part, event.pitch, event.tick);
		const PartSpec& part = generator_->recipe().genre->parts[event.part];
		songExtra_ += part.synth() ? kSongCosts.synthClip : int64_t{laneCount(part)} * kSongCosts.kitRow;
		break;
	}
	case EventKind::Section: {
		// A song that would be too big to load back ends at the next Section line (spec §17.5).
		// Not on the song's first line, nor on the span's last step, which a stop can leave decided and never heard
		// (spec §17.3): a song a step long would follow.
		if (event.tick <= songStart_ || event.tick >= stepTick(spanEnd_ - 1)) {
			break;
		}
		const int32_t bars = (event.tick - songStart_ + kBarTicks - 1) / kBarTicks;
		if (estimateBarsBytes(*generator_->recipe().genre, bars) + songExtra_ > splitBudget_) {
			split(event.tick);
		}
		break;
	}
	default:
		break;
	}
}

void ComposeDriver::split(int32_t tick) {
	const bool tempoMoving = tempoPlan_.moving(tick);
	if (tempoMoving) {
		writer_->tempo({tick - 1, tempoPlan_.at(tick - 1), true});
	}
	clips_.split(tick, [this](const Automated& automated) { return centreOf(automated); });
	songStart_ = tick;
	songExtra_ = 0;
	songs_++;
	std::fill(std::begin(partCutTick_), std::end(partCutTick_), tick);
	// The next song opens at the tempo that plays there.
	if (tempoMoved_) {
		writer_->tempo({tick, tempoPlan_.at(tick), false});
	}
}

void ComposeDriver::cutForSetting(uint8_t target, int32_t tick) {
	const Genre& genre = *generator_->recipe().genre;
	if (target >= partCount(genre) || tick <= partCutTick_[target]) {
		return;
	}
	clips_.cutPart(target, tick, [this](const Automated& automated) { return centreOf(automated); });
	partCutTick_[target] = tick;
	const PartSpec& part = genre.parts[target];
	songExtra_ += part.synth() ? kSongCosts.synthClip : int64_t{laneCount(part)} * kSongCosts.kitRow;
}

void ComposeDriver::swingDifference(Event& note) const {
	const int32_t tick = note.tick;
	// Perform changes the swing on step lines (spec §12.7): a note plays at its step's.
	const int32_t line = stepTick(tick >= 0 ? tick / kTicksPerStep : 0);
	const int32_t interval = tick >= intervalTick_ ? interval_ : intervalBefore_;
	note.tick = swingShifted(tick, swingPlan_.at(line), interval, songSwing_, songInterval_);
}

void ComposeDriver::glideTempo(int32_t tick, int32_t tempo, int32_t length) {
	tempoReaches(tick);
	const bool moving = tempoPlan_.moving(tick);
	const int32_t from = tempoPlan_.at(tick);
	if (!moving && tempo == from) {
		tempoPlan_.glide(tick, tempo, 0);
		return; // the external clock's per-bar tempo, unchanged
	}
	if (!tempoMoved_ && tick > songStart_) {
		// The song's first tempo node holds its own tempo from its start, as a control lane's does.
		writer_->tempo({songStart_, tempoPlan_.at(songStart_), false});
	}
	if (moving) {
		// A glide cut short holds what it reached: its ramp ends here, or a tick before a jump.
		writer_->tempo(length > 0 ? TempoNode{tick, from, true} : TempoNode{tick - 1, tempoPlan_.at(tick - 1), true});
	}
	else if (length > 0) {
		writer_->tempo({tick, from, false});
	}
	tempoPlan_.glide(tick, tempo, length);
	tempoMoved_ = true;
	tempoEndDue_ = length > 0;
	if (length == 0) {
		writer_->tempo({tick, tempo, false});
	}
}

void ComposeDriver::tempoReaches(int32_t tick) {
	if (tempoEndDue_ && tempoPlan_.endTick() <= tick) {
		writer_->tempo({tempoPlan_.endTick(), tempoPlan_.target(), true});
		tempoEndDue_ = false;
	}
}

int8_t ComposeDriver::centreOf(const Automated& automated) const {
	if (automated.throwDelay) {
		return 0; // the Throw lifts feedback from its home
	}
	return static_cast<int8_t>(generator_->centreLayer(automated.part, automated.lane, automated.control));
}

bool ComposeDriver::timeRiser(Event& noteOn) {
	// As PerformDriver::holdRiser() does.
	const int32_t drop = generator_->riserDropTick();
	if (drop < 0) {
		return true;
	}
	int32_t micros = writer_->sampleMicros(noteOn);
	if (micros <= 0) {
		return true;
	}
	// Compose's song plays the Recipe's tempo throughout; a replay's, the tempo the log's glides plan (spec §17.6).
	auto tempoAt = [this](int32_t tick) { return replaying_ ? tempoPlan_.at(tick) : tempo_; };
	const int32_t fallback = writer_->fallbackMicros(noteOn);
	switch (perform::pickRiser(noteOn.tick, drop, micros, fallback, tempoAt)) {
	case perform::RiserPick::Drawn:
		break;
	case perform::RiserPick::Fallback:
		noteOn.pitch = perform::kRiserFallbackPitch; // the note goes on the fallback's row
		micros = fallback;
		break;
	case perform::RiserPick::None:
		return false;
	}
	noteOn.tick = perform::riserStart(noteOn.tick, drop, micros, tempoAt);
	return true;
}

void ComposeDriver::finish() {
	// The Outro is over: the song ends on its last line, every note released there (spec §13.2), as Perform's does.
	// A replay ends at its span's end: where the piece ended, or where the log was cut (spec §17.2).
	endTick_ = stepTick(replaying_ ? spanEnd_ : generator_->nextStep());
	writeDue(endTick_);
	generator_->flush(endTick_, Generator::FlushFor::Stop);
	writeDue(INT32_MAX);
	if (replaying_) {
		tempoReaches(endTick_ - 1);
		if (tempoEndDue_) {
			writer_->tempo({endTick_ - 1, tempoPlan_.at(endTick_ - 1), true}); // a glide the song's end cuts short
			tempoEndDue_ = false;
		}
	}
	clips_.finish(endTick_, [this](const Automated& automated) { return centreOf(automated); });
	done_ = true;
}

} // namespace deluge::generative::compose
