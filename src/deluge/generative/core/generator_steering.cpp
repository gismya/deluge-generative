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

// The Generator's steering: how a command lands, waits and moves the Arc (spec §2.8, §9.6, §9.7), and the Arc plan
// the ARC page draws from the waiting commands (spec §13.5).

#include "generative/core/generator.h"
#include <algorithm>

namespace deluge::generative {

namespace {

/// Whether a SetKey names a pitch class and a scale.
constexpr bool validKeyChange(const Command& command) {
	return validKey(command.target, command.value);
}

/// A SetKey's key and scale; valid once validKeyChange() holds.
constexpr Key keyOf(const Command& command) {
	return {command.target, static_cast<Scale>(command.value)};
}

/// Whether a SetSound names a part of the roster and a token.
bool validSound(const Command& command, const Genre& genre) {
	return command.target < partCount(genre) && command.value > kNoSound && command.value <= kMaxSoundToken;
}

/// Whether a SetTake names a part of the roster and a take.
constexpr bool validTake(const Command& command, const Genre& genre) {
	return command.target < partCount(genre) && command.value >= 0 && command.value < kMaxTakes;
}

/// Whether a Section kind can be forced: Groove, Break or Build (spec §9.6, §13.5). The Outro comes with End.
constexpr bool forceable(SectionKind kind) {
	return kind == SectionKind::Groove || kind == SectionKind::Break || kind == SectionKind::Build;
}

/// The Section `command` starts in the Arc where it lands, if it moves the Arc (spec §9.6, §9.7): a force its own, End
/// and a Fade the Outro, a Drop transition its Build. A hard cut stops the Arc there. `out.at` is left alone.
GEN_STEP_CODE bool sectionMoveOf(const Command& command, Arc::Move& out) {
	switch (command.kind) {
	case CommandKind::ForceSection:
		out.section = static_cast<SectionKind>(command.target);
		out.buildSteps = command.value;
		return true;
	case CommandKind::End:
		out.section = SectionKind::Outro;
		return true;
	case CommandKind::Transition:
		switch (static_cast<TransitionStyle>(command.target)) {
		case TransitionStyle::BuildIntoDrop:
			out.section = SectionKind::Build;
			out.buildSteps = command.value;
			break;
		case TransitionStyle::Fade:
			out.section = SectionKind::Outro;
			break;
		case TransitionStyle::Cut:
			out.stops = true;
			break;
		}
		return true;
	default:
		return false;
	}
}

} // namespace

int32_t Generator::transitionSteps(const Command& command) const {
	switch (static_cast<TransitionStyle>(command.target)) {
	case TransitionStyle::BuildIntoDrop:
		return command.value;
	case TransitionStyle::Fade:
		return genre_->arc.outroPhrases * genre_->phraseSteps();
	case TransitionStyle::Cut:
		break;
	}
	return 0;
}

GEN_STEP_CODE bool Generator::apply(const Command& command) {
	switch (command.kind) {
	case CommandKind::SetMacro:
		if (command.target >= kNumMacros) {
			return false;
		}
		recipe_.macros.set(static_cast<Macro>(command.target), command.value);
		return true;
	case CommandKind::SetKey: {
		if (!validKeyChange(command)) {
			return false;
		}
		const Key before = key();
		recipe_.keyRoot = keyOf(command).root;
		recipe_.scale = keyOf(command).scale;
		if (key() != before) {
			// Every pitched note ends here, and nothing held back at the old key comes in late: the new notes start
			// fresh. Drum and FX tails ring out (spec §7.6).
			const int32_t tick = stepTick(nextStep_);
			sounding_.endPitchedBy(tick);
			held_.releasePitched();
			for (int32_t p = 0; p < partCount(*genre_); p++) {
				cutParts_ |= genre_->parts[p].lanes.empty() ? partBit(p) : 0;
			}
			if (nextStep_ > 0) { // step 0 sends the starting key anyway
				push(Event::keyScale(tick, recipe_.keyRoot, recipe_.scale));
			}
		}
		return true;
	}
	case CommandKind::SetTake: {
		// A new take or a step back: every pattern of the part re-picks with it (spec §6.6).
		if (!validTake(command, *genre_)) {
			return false;
		}
		PartState& part = parts_[command.target];
		part.newTake = takes_.take[command.target] != command.value;
		takes_.take[command.target] = static_cast<uint8_t>(command.value);
		return true;
	}
	case CommandKind::ForceSection: {
		auto kind = static_cast<SectionKind>(command.target);
		if (!forceable(kind) || (kind == SectionKind::Build && !arc_.buildFits(command.value))) {
			return false;
		}
		arc_.force(kind, command.value);
		return true;
	}
	case CommandKind::SetHold:
		if (command.value != 0 && command.value != 1) {
			return false;
		}
		arc_.hold(command.value != 0);
		return true;
	case CommandKind::CancelNewest:
		return false; // the pending list's, never applied or logged
	case CommandKind::End:
		if (command.value == 0) {
			return false; // a cancel never lands
		}
		arc_.force(SectionKind::Outro, 0);
		return true;
	case CommandKind::SetTempo:
		if (command.value <= 0) {
			return false;
		}
		recipe_.tempo = std::clamp<int32_t>(command.value, kMinTempo, kMaxTempo);
		return true;
	case CommandKind::SetSwing:
		if (command.value < -kMaxSwing || command.value > kMaxSwing) {
			return false;
		}
		recipe_.swing = static_cast<int8_t>(command.value);
		return true;
	case CommandKind::SetSwingInterval:
		if (command.value < 0 || command.value > kMaxSwingInterval) {
			return false;
		}
		recipe_.swingInterval = static_cast<uint8_t>(command.value);
		return true;
	case CommandKind::SetHome:
		return command.target < partCount(*genre_); // the driver moved the home: logged, nothing to apply

	case CommandKind::SetFreeze: {
		if (command.target >= partCount(*genre_) || (command.value != 0 && command.value != 1)) {
			return false;
		}
		const bool freeze = command.value != 0;
		if (freeze && !partFrozen(command.target)) {
			// It replays the phrase of the last decided step (spec §6.7). On a phrase line that's the phrase just
			// heard: the line's re-pick or Drop waits for the thaw, so the new phrase's draws would never match.
			const int32_t last = nextStep_ > 0 ? nextStep_ - 1 : 0;
			parts_[command.target].frozenPhraseStart = last - last % genre_->phraseSteps();
		}
		else if (!freeze && partFrozen(command.target)) {
			thaw(command.target);
		}
		const auto bit = static_cast<uint8_t>(1U << command.target);
		frozen_ = freeze ? frozen_ | bit : frozen_ & ~bit;
		return true;
	}
	case CommandKind::SetSound: {
		if (!validSound(command, *genre_)) {
			return false;
		}
		// The part's notes end on the line, on its old sound, and its new one plays the line's notes (spec §10.7).
		const int32_t tick = stepTick(nextStep_);
		sounding_.endPartBy(command.target, tick);
		cutParts_ |= partBit(command.target);
		landedSound_[command.target] = static_cast<uint8_t>(command.value);
		push(Event::sound(tick, command.target, static_cast<uint8_t>(command.value)));
		return true;
	}
	case CommandKind::SetMute:
	case CommandKind::SetSolo: {
		if (command.target >= partCount(*genre_) || (command.value != 0 && command.value != 1)) {
			return false;
		}
		mutes_.land(command);
		return true;
	}
	case CommandKind::Transition: {
		// It plays its Build (placed like a forced one) or Outro from here, and the new roster starts where that ends,
		// or here for a cut (spec §9.7).
		const auto style = static_cast<TransitionStyle>(command.target);
		if (command.target >= kNumTransitionStyles || stopAt_ != kNever
		    || (style == TransitionStyle::BuildIntoDrop && !arc_.buildFits(command.value))) {
			return false;
		}
		if (style == TransitionStyle::BuildIntoDrop) {
			arc_.force(SectionKind::Build, command.value);
		}
		else if (style == TransitionStyle::Fade) {
			arc_.force(SectionKind::Outro, 0);
		}
		stopAt_ = nextStep_ + transitionSteps(command);
		return true;
	}
	case CommandKind::SetFill: {
		const std::span<const FillEntry> fills = genre_->fills;
		if (command.value == kFillReleased) {
			const bool held = heldFill_ >= 0;
			heldFill_ = -1;
			return held;
		}
		// A genre with no fill entries has no held-fill entry: FILL does nothing (spec §8.7).
		if (fills.empty() || (command.value != kFillHeldEntry && command.value != kFillFreshPick)) {
			return false;
		}
		// The one place a held Roll's count restarts: every held fill starts with this press, the driver's re-send
		// after begin() included, so a new piece's held Roll starts its first beat again (spec §8.7).
		heldSteps_ = 0;
		if (command.value == kFillHeldEntry) {
			heldFill_ = static_cast<int8_t>(genre_->heldFill < fills.size() ? genre_->heldFill : 0);
			return true;
		}
		// SHIFT + FILL: a weighted draw, keyed on the step the press lands on.
		DrawKey key = partKey(seed_, kArcPart, Lane::None, 0, Purpose::HeldFill);
		key.position[0] = static_cast<uint32_t>(nextStep_);
		heldFill_ = static_cast<int8_t>(drawWeighted(key, fills));
		return true;
	}
	}
	return false;
}

GEN_STEP_CODE bool Generator::resolvePress(Command& command) {
	if (command.kind == CommandKind::SetHold) {
		command.value = arc_.held() ? 0 : 1;
		return true;
	}
	if (command.kind == CommandKind::SetFreeze) {
		command.value = command.target < kMaxParts && partFrozen(command.target) ? 0 : 1;
		return true;
	}
	// Pressing again while a press waits for its line cancels it (spec §13.5).
	if (pending_.waiting(command) != nullptr) {
		pending_.cancel(command);
		return false;
	}
	const uint8_t parts = mutes_.of(command.kind);
	command.value = command.target < kMaxParts && (parts & (1U << command.target)) != 0 ? 0 : 1;
	return true;
}

GEN_STEP_CODE bool Generator::wait(Command command) {
	// An invalid command is dropped here, so it never replaces a valid one or gets logged.
	switch (command.kind) {
	case CommandKind::SetKey:
		if (!validKeyChange(command)) {
			return true;
		}
		// Choosing the key that plays again cancels the change waiting, if any (spec §13.5).
		if (keyOf(command) == key()) {
			pending_.cancel(command);
			return true;
		}
		break;
	case CommandKind::SetTake: {
		if (!validTake(command, *genre_)) {
			return true;
		}
		// Pressing the waiting take again, or the take that plays, cancels the one waiting (spec §13.5).
		const Command* waiting = pending_.waiting(command);
		if ((waiting != nullptr && waiting->value == command.value) || takes_.take[command.target] == command.value) {
			pending_.cancel(command);
			return true;
		}
		break;
	}
	case CommandKind::SetSound:
		if (command.target < partCount(*genre_) && command.value == kNoSound) {
			pending_.cancel(command); // the driver frees the sound it preloaded
			return true;
		}
		if (!validSound(command, *genre_)) {
			return true;
		}
		break;
	case CommandKind::ForceSection: {
		auto kind = static_cast<SectionKind>(command.target);
		if (!forceable(kind)) {
			return true;
		}
		// Pressing the same force again cancels it while it waits (spec §9.6). Another kind replaces it.
		const Command* waiting = pending_.waiting(command);
		if (waiting != nullptr && waiting->kind == CommandKind::ForceSection && waiting->target == command.target) {
			pending_.cancel(command);
			return true;
		}
		// A forced Build draws its length when forced, so it knows where it starts (spec §9.6).
		command.value = kind == SectionKind::Build ? arc_.nextBuildSteps(*genre_, seed_) : 0;
		break;
	}
	case CommandKind::End: {
		// Once the Outro plays, End has nothing left to do: dropped, so it can't start the Outro over.
		if (arc_.section() == SectionKind::Outro) {
			return true;
		}
		// A tap cancels End while it waits; it leaves a force in the shared slot alone. End again keeps it.
		const Command* waiting = pending_.waiting(command);
		bool waits = waiting != nullptr && waiting->kind == CommandKind::End;
		if (command.value == 0) {
			if (waits) {
				pending_.cancel(command);
			}
			return true;
		}
		if (waits) {
			return true;
		}
		command.value = 1;
		break;
	}
	case CommandKind::SetMute:
	case CommandKind::SetSolo:
		if (command.target >= partCount(*genre_) || !resolvePress(command)) {
			return true;
		}
		break;
	case CommandKind::Transition:
		if (command.value == kCancelTransition) {
			pending_.cancel(command); // only while it waits: once landed it plays on (spec §9.7)
			return true;
		}
		if (command.target >= kNumTransitionStyles || stopAt_ != kNever) {
			return true;
		}
		// Its Build draws its length as a forced one does, so it knows where it starts (spec §9.6, §9.7).
		command.value = static_cast<TransitionStyle>(command.target) == TransitionStyle::BuildIntoDrop
		                    ? arc_.nextBuildSteps(*genre_, seed_)
		                    : 0;
		break;
	default:
		break;
	}
	return pending_.add(command);
}

GEN_STEP_CODE const Command* Generator::waitingForce() const {
	const Command* waiting = pending_.waiting({CommandKind::ForceSection});
	return waiting != nullptr && waiting->kind == CommandKind::ForceSection ? waiting : nullptr;
}

bool Generator::outroEndsAfter(const Arc& arc, int32_t step) const {
	if (!arc.outroEndsHere()) {
		return false;
	}
	const Command* force = waitingForce();
	return force == nullptr || landsAt(*force) != step + 1;
}

GEN_STEP_CODE int32_t Generator::stepsToLine(const Command& command) const {
	switch (command.kind) {
	case CommandKind::ForceSection:
		return arc_.stepsToStart(static_cast<SectionKind>(command.target), command.value);
	case CommandKind::SetMute:
	case CommandKind::SetSolo:
		return arc_.stepsToBarLine(); // spec §13.4
	case CommandKind::Transition:
		// A Build placed like a forced one; an Outro or a cut on the next phrase line (spec §9.7).
		return static_cast<TransitionStyle>(command.target) == TransitionStyle::BuildIntoDrop
		           ? arc_.stepsToStart(SectionKind::Build, command.value)
		           : arc_.stepsToPhraseLine();
	default:
		// Key changes, takes and End land on the next phrase line, which every Arc Drop is (spec §7.6, §9.3, §6.6).
		return arc_.stepsToPhraseLine();
	}
}

void Generator::landPending(SteeringLog* log) {
	const int32_t n = nextStep_;
	pending_.land([&](const Command& command) {
		if (stepsToLine(command) != 0) {
			return false;
		}
		if (apply(command) && log != nullptr) {
			log->append(n, command);
		}
		return true;
	});
}

int32_t Generator::landsAt(const Command& command) const {
	return nextStep_ + stepsToLine(command);
}

GEN_STEP_CODE bool Generator::moveAt(const Command& command, Arc::Move& out) const {
	if (!sectionMoveOf(command, out)) {
		return false;
	}
	out.at = stepsToLine(command) + 1;
	return true;
}

GEN_STEP_CODE Arc::Move Generator::waitingMove() const {
	// A force or End, and a transition, each wait in a slot. They land oldest first, so where two land on one step the
	// newer's Section starts, except that a cut stops the Arc there whatever else lands. Only the first move is kept,
	// and a later one can't change the chord being drawn. On a harmonic rhythm up to a phrase long, the first move's
	// Section start starts a chord, so the walk's look ahead (Arc::ahead()) ends there. On a longer one, chords start
	// on phrase lines, where every move pressed before has landed, except a Build shorter than the phrase, so each move
	// still waiting starts a Build that lands its Drop on the next phrase line: the first's Drop is every one's.
	// walkChord() asserts that in the harness, on every phrase line.
	Arc::Move first;
	for (int32_t i = 0; i < pending_.size(); i++) {
		const Command& command = pending_.at(i);
		Arc::Move move;
		if (!moveAt(command, move)) {
			continue;
		}
		if (move.at < first.at || (move.at == first.at && !first.stops)) {
			first = move;
		}
	}
	return first;
}

void Generator::planArc(ArcPlan& out) const {
	out = ArcPlan{};
	const Genre& genre = *genre_;
	const int32_t phraseSteps = genre.phraseSteps();
	const Macros& centre = recipe_.macros;
	const int32_t parts = partCount(genre);

	// What lands on its line: each waiting command and the step it lands on, by the one line rule.
	struct Landing {
		Command command;
		int32_t step;
	};
	Landing landings[PendingList::kCapacity];
	const int32_t waiting = pending_.size();
	for (int32_t i = 0; i < waiting; i++) {
		landings[i] = {pending_.at(i), landsAt(pending_.at(i))};
	}

	// A copy of the counters the plan moves on: the Arc, the gates, mutes and solos. Nothing else of the piece.
	Arc arc = arc_;
	MuteSolo mutes = mutes_;
	Gates gates[kMaxParts] = {};
	for (int32_t p = 0; p < parts; p++) {
		gates[p] = parts_[p].gates;
	}
	// The plan reads gates, keeps, the roll and mutes. The cut and the Gap never cover a whole phrase or a whole Build
	// (cutAndGapUnderBuild()), so it leaves them out, and so does a drop-out, which never outlasts the phrase it starts
	// in (dropOutsValid(), spec §8.8). The dim counts all three, so a part they silence on a phrase's last step shows
	// dim there while the plan shows it playing; the cut never stops the roll lane (rollUncut()).
	const auto lets = [&](int32_t p, const Keeps& keeps, RollStage roll) {
		return mayPlay(genre.parts[p], gates[p], genre.arc.roll.lane, roll,
		               [&](Lane lane) { return keeps.lets(p, lane); });
	};

	int32_t energySum[ArcPlan::kMaxPhrases] = {};
	int32_t steps[ArcPlan::kMaxPhrases] = {};
	const auto addStep = [&](int32_t phrase, const Arc& at, uint8_t sounds, int32_t energy) {
		ArcPhrase& entry = out.phrase[phrase];
		entry.kind = at.section();
		entry.kindFrom = static_cast<int16_t>(std::max<int32_t>(at.stepInPhrase() - at.stepInSection(), 0));
		entry.drop = entry.drop || (at.stepInPhrase() == 0 && at.dropStartsHere());
		entry.sounds |= sounds;
		entry.muted = mutes.muted;
		energySum[phrase] += energy - centre[Macro::Energy];
		steps[phrase]++;
	};

	int32_t step = nextStep_;
	out.first = (step > 0 ? step - 1 : 0) / phraseSteps;
	out.count = ArcPlan::kMaxPhrases;
	if (step > 0) {
		// The last decided step, as it played: the first of the plan's first phrase.
		uint8_t sounds = 0;
		for (int32_t p = 0; p < parts; p++) {
			sounds |= !partSilenced(p) && mutes.audible(p) ? static_cast<uint8_t>(1U << p) : 0;
		}
		addStep(0, arc_, sounds, effectiveMacro(Macro::Energy));
	}
	bool ended = this->ended();
	// Where a transition's new roster starts: the landed one's start step, or where the waiting one's would be.
	int32_t newRoster = stopAt_;
	if (const Command* transition = waitingTransition(); transition != nullptr) {
		newRoster = landsAt(*transition) + transitionSteps(*transition);
	}
	for (; step / phraseSteps - out.first < ArcPlan::kMaxPhrases; step++) {
		const int32_t phrase = step / phraseSteps - out.first;
		if (step >= newRoster) {
			out.phrase[phrase].next = steps[phrase] == 0;
			continue;
		}
		if (ended) {
			out.phrase[phrase].ended = steps[phrase] == 0;
			continue;
		}
		// Steering lands before the Arc moves, as in decideNextStep().
		for (int32_t i = 0; i < waiting; i++) {
			const Command& command = landings[i].command;
			if (landings[i].step != step) {
				continue;
			}
			if (Arc::Move move; sectionMoveOf(command, move)) {
				if (!move.stops) {
					arc.force(move.section, move.buildSteps);
				}
				continue;
			}
			if (command.kind == CommandKind::SetMute || command.kind == CommandKind::SetSolo) {
				mutes.land(command);
			}
			// A key change or a take moves nothing the plan shows.
		}
		arc.enterStep(genre, seed_, step, centre);
		const int32_t energy = effectiveOn(genre, arc, centre, Macro::Energy);
		if (arc.stepInBar() == 0) {
			const int32_t density = effectiveOn(genre, arc, centre, Macro::Density);
			for (int32_t p = 0; p < parts; p++) {
				gates[p].crossBar(genre.parts[p], density);
			}
		}
		const Keeps keeps = genre.arc.keepsIn(arc.section());
		const RollStage roll = rollStageOn(genre.arc, arc, centre[Macro::Complexity]);
		uint8_t sounds = 0;
		for (int32_t p = 0; p < parts; p++) {
			sounds |= lets(p, keeps, roll) && mutes.audible(p) ? static_cast<uint8_t>(1U << p) : 0;
		}
		addStep(phrase, arc, sounds, energy);
		// The piece stops on the line where the Outro ends, unless a force lands there (spec §13.2).
		ended = outroEndsAfter(arc, step);
	}
	for (int32_t i = 0; i < ArcPlan::kMaxPhrases; i++) {
		out.phrase[i].energy = static_cast<int16_t>(steps[i] > 0 ? energySum[i] / steps[i] : 0);
	}
}

Key Generator::keyAhead() const {
	const Command* waiting = pending_.waiting({CommandKind::SetKey});
	return waiting != nullptr ? keyOf(*waiting) : key();
}

} // namespace deluge::generative
