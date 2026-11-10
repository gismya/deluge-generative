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
#include "generative/core/draw.h"
#include "generative/core/motion_curve.h"
#include "generative/core/tiers.h"
#include <algorithm>
#include <iterator>

namespace deluge::generative {

namespace {

/// The chord root while the Harmony slot is empty: the home chord, on the tonic (spec §7.4).
constexpr int32_t kHomeChordRoot = 0;

/// A draw key for a part, or one of its lanes (Lane::None for a part-wide draw), before its position words.
DrawKey partKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose) {
	DrawKey key;
	key.seed = seed;
	key.part = part;
	key.lane = static_cast<uint8_t>(lane);
	key.take = take;
	key.purpose = purpose;
	return key;
}

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

/// The Build roll's hits on `step` at `stage` (spec §8.3): none off the stage's grid (beats, 8ths, then every 16th),
/// otherwise 1, or a ratchet of 2 for 32nds and 4 for 64ths.
constexpr int32_t rollGrid(RollStage stage) {
	return stage == RollStage::Beat ? kStepsPerBeat : stage == RollStage::Eighth ? 2 : 1;
}
constexpr int32_t rollHits(RollStage stage, int32_t step) {
	if (stage == RollStage::None || step % rollGrid(stage) != 0) {
		return 0;
	}
	return stage == RollStage::SixtyFourth ? 4 : stage == RollStage::ThirtySecond ? 2 : 1;
}

/// The stage a roll has reached with `toDrop` of a Build's `length` steps left: one stage further for each of the
/// genre's fractions of the Build left before the Drop, up to its top stage (spec §8.3).
constexpr RollStage rollStageAt(const BuildRoll& roll, int32_t toDrop, int32_t length) {
	int32_t stage = 0;
	for (uint8_t fraction : roll.stageFraction) {
		if (stage >= static_cast<int32_t>(roll.top) || fraction == 0 || toDrop * fraction > length) {
			break;
		}
		stage++;
	}
	return static_cast<RollStage>(stage);
}

/// What still plays in a Section of `kind` by its keeps (spec §9.5): Break or Build keeps, or nullptr (everything).
const PartsAndLanes* keepsFor(const ArcSpec& rules, SectionKind kind) {
	if (kind == SectionKind::Break) {
		return &rules.breakKeeps;
	}
	if (kind == SectionKind::Build) {
		return rules.buildKeeps == kSameAsBreakKeeps ? &rules.breakKeeps : &rules.buildKeeps;
	}
	return nullptr;
}

/// Whether a Section's keeps let a part's notes on `lane` (Lane::None for a pitched part) play. FX lanes are the
/// section rules' own, and the roll lane always plays in a Build.
bool kept(const Genre& genre, const PartsAndLanes* keeps, SectionKind kind, int32_t part, Lane lane) {
	if (keeps == nullptr || isFxLane(lane)
	    || (lane != Lane::None && lane == genre.arc.roll.lane && kind == SectionKind::Build)) {
		return true;
	}
	return keeps->has(part, lane);
}

/// `key` positioned on a Groove and a phrase within it: word 0 and word 1. Word 2 is the purpose's.
DrawKey onPhrase(DrawKey key, uint16_t groove, uint16_t phrase, uint32_t word2 = 0) {
	key.position[0] = groove;
	key.position[1] = phrase;
	key.position[2] = word2;
	return key;
}

/// `key` positioned on a pick: its Groove in word 0, its phrase in word 1's top half and its pool entry in word 2's
/// top byte. The purpose fills the rest: an `index` in word 1's low half (passes since a roll, a rule) and a `step`
/// in word 2's low 24 bits (a step within the pattern or line, with any field in its low byte).
DrawKey onPick(DrawKey key, const PatternState& state, uint16_t index, uint32_t step) {
	key.position[0] = state.pick.groove;
	key.position[1] = static_cast<uint32_t>(state.pick.phrase) << 16 | index;
	key.position[2] = static_cast<uint32_t>(static_cast<uint8_t>(state.entry)) << 24 | (step & 0xFFFFFFU);
	return key;
}

/// A position word holding `value` with a draw's field in its low byte, for purposes that draw several fields.
constexpr uint32_t fieldWord(uint32_t value, int32_t field) {
	return value << 8 | static_cast<uint32_t>(field);
}

/// `key` for another field of the same draw: the field is position[2]'s low byte.
DrawKey withField(DrawKey key, int32_t field) {
	key.position[2] = fieldWord(key.position[2] >> 8, field);
	return key;
}

/// Fields of an automatic fill's draws: whether one plays, and which entry.
enum FillField : int32_t { kFillWhether, kFillWhich };

} // namespace

void Generator::start(const Recipe& recipe, uint32_t seed, const Takes& takes, bool skipIntro) {
	begin(recipe, seed, takes, skipIntro ? SectionKind::Groove : SectionKind::Intro, false);
}

void Generator::begin(const Recipe& recipe, uint32_t seed, const Takes& takes, SectionKind first, bool onDrop) {
	recipe_ = recipe;
	seed_ = seed;
	takes_ = takes;
	genre_ = recipe.genre;
	nextStep_ = 0;
	stopAt_ = kNever;
	arc_.start(*genre_, first, onDrop);
	// A stop cancels the pending list: every start is fresh (spec §2.6, §13.2).
	pending_.clear();
	muted_ = 0; // mutes and solos are performance state: lost on stop (spec §13.1)
	soloed_ = 0;
	frozen_ = 0;
	silent_ = 0;
	std::fill(std::begin(landedSound_), std::end(landedSound_), static_cast<uint8_t>(kNoSound));
	autoFill_ = -1;
	heldFill_ = -1; // the driver sends FILL again if it is still held
	rested_ = 0;
	filled_ = false;
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
		}
	}
	shadedFrom_ = kNever;
	driftPoint_ = 0;
	driftTowards_ = 0;
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
	      style == TransitionStyle::Drop);
}

void Generator::carryOver(const Macros& macros, bool held) {
	recipe_.macros = macros;
	arc_.carryHold(held);
}

int32_t Generator::transitionSteps(const Command& command) const {
	switch (static_cast<TransitionStyle>(command.target)) {
	case TransitionStyle::Drop:
		return command.value;
	case TransitionStyle::Fade:
		return genre_->arc.outroPhrases * genre_->phraseSteps();
	case TransitionStyle::Cut:
		break;
	}
	return 0;
}

bool Generator::apply(const Command& command) {
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
		landedSound_[command.target] = static_cast<uint8_t>(command.value);
		push(Event::sound(tick, command.target, static_cast<uint8_t>(command.value)));
		return true;
	}
	case CommandKind::SetMute:
	case CommandKind::SetSolo: {
		if (command.target >= partCount(*genre_) || (command.value != 0 && command.value != 1)) {
			return false;
		}
		uint8_t& parts = command.kind == CommandKind::SetMute ? muted_ : soloed_;
		const auto bit = static_cast<uint8_t>(1U << command.target);
		parts = command.value != 0 ? parts | bit : parts & ~bit;
		return true;
	}
	case CommandKind::Transition: {
		// It plays its Build (placed like a forced one) or Outro from here, and the new roster starts where that ends,
		// or here for a cut (spec §9.7).
		const auto style = static_cast<TransitionStyle>(command.target);
		if (command.target >= kNumTransitionStyles || stopAt_ != kNever
		    || (style == TransitionStyle::Drop && !arc_.buildFits(command.value))) {
			return false;
		}
		if (style == TransitionStyle::Drop) {
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

bool Generator::resolvePress(Command& command) {
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
	const uint8_t parts = command.kind == CommandKind::SetMute ? muted_ : soloed_;
	command.value = command.target < kMaxParts && (parts & (1U << command.target)) != 0 ? 0 : 1;
	return true;
}

bool Generator::wait(Command command) {
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
		command.value = static_cast<TransitionStyle>(command.target) == TransitionStyle::Drop
		                    ? arc_.nextBuildSteps(*genre_, seed_)
		                    : 0;
		break;
	default:
		break;
	}
	return pending_.add(command);
}

const Command* Generator::waitingForce() const {
	const Command* waiting = pending_.waiting({CommandKind::ForceSection});
	return waiting != nullptr && waiting->kind == CommandKind::ForceSection ? waiting : nullptr;
}

bool Generator::forceLandsNext() const {
	const Command* force = waitingForce();
	return force != nullptr && arc_.forceLandsNext(static_cast<SectionKind>(force->target), force->value);
}

int32_t Generator::stepsToLine(const Command& command) const {
	switch (command.kind) {
	case CommandKind::ForceSection:
		return arc_.stepsToStart(static_cast<SectionKind>(command.target), command.value);
	case CommandKind::SetMute:
	case CommandKind::SetSolo:
		return arc_.stepsToBarLine(); // spec §13.4
	case CommandKind::Transition:
		// A Build placed like a forced one; an Outro or a cut on the next phrase line (spec §9.7).
		return static_cast<TransitionStyle>(command.target) == TransitionStyle::Drop
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
	uint8_t muted = muted_;
	uint8_t soloed = soloed_;
	bool gate[kMaxParts] = {};
	bool laneGate[kMaxParts][kMaxLanesPerPart] = {};
	for (int32_t p = 0; p < parts; p++) {
		gate[p] = parts_[p].gateOpen;
		for (int32_t l = 0; l < kMaxLanesPerPart; l++) {
			laneGate[p][l] = parts_[p].laneGateOpen[l];
		}
	}
	const auto audible = [&](int32_t p) {
		const uint32_t bit = 1U << p;
		return soloed != 0 ? (soloed & bit) != 0 : (muted & bit) == 0;
	};
	const auto lets = [&](int32_t p, const PartsAndLanes* keeps, SectionKind kind) {
		const PartSpec& spec = genre.parts[p];
		if (!gate[p]) {
			return false;
		}
		if (spec.lanes.empty()) {
			return kept(genre, keeps, kind, p, Lane::None);
		}
		for (int32_t l = 0; l < laneCount(spec); l++) {
			if (laneGate[p][l] && kept(genre, keeps, kind, p, spec.lanes[l].lane)) {
				return true;
			}
		}
		return false;
	};

	int32_t energySum[ArcPlan::kMaxPhrases] = {};
	int32_t steps[ArcPlan::kMaxPhrases] = {};
	const auto addStep = [&](int32_t phrase, const Arc& at, uint8_t sounds, int32_t energy) {
		ArcPhrase& entry = out.phrase[phrase];
		entry.kind = at.section();
		entry.drop = entry.drop || (at.stepInPhrase() == 0 && at.dropStartsHere());
		entry.sounds |= sounds;
		entry.muted = muted;
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
			sounds |= !partSilenced(p) && audible(p) ? static_cast<uint8_t>(1U << p) : 0;
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
			switch (command.kind) {
			case CommandKind::ForceSection:
				arc.force(static_cast<SectionKind>(command.target), command.value);
				break;
			case CommandKind::End:
				arc.force(SectionKind::Outro, 0);
				break;
			case CommandKind::Transition:
				if (static_cast<TransitionStyle>(command.target) == TransitionStyle::Drop) {
					arc.force(SectionKind::Build, command.value);
				}
				else if (static_cast<TransitionStyle>(command.target) == TransitionStyle::Fade) {
					arc.force(SectionKind::Outro, 0);
				}
				break;
			case CommandKind::SetMute:
			case CommandKind::SetSolo: {
				uint8_t& mask = command.kind == CommandKind::SetMute ? muted : soloed;
				const auto bit = static_cast<uint8_t>(1U << command.target);
				mask = command.value != 0 ? mask | bit : mask & ~bit;
				break;
			}
			default:
				break; // a key change or a take moves nothing the plan shows
			}
		}
		arc.enterStep(genre, seed_, step, centre);
		const int32_t energy = std::clamp<int32_t>(
		    centre[Macro::Energy] + arc.offset(genre, Macro::Energy, centre[Macro::Energy]), 0, kMacroFull);
		if (arc.stepInBar() == 0) {
			const int32_t density = std::clamp<int32_t>(
			    centre[Macro::Density] + arc.offset(genre, Macro::Density, centre[Macro::Density]), 0, kMacroFull);
			for (int32_t p = 0; p < parts; p++) {
				const PartSpec& spec = genre.parts[p];
				gate[p] = gateOpens(gate[p], spec.densityGate, density);
				for (int32_t l = 0; l < laneCount(spec); l++) {
					laneGate[p][l] = gateOpens(laneGate[p][l], spec.lanes[l].densityGate, density);
				}
			}
		}
		const PartsAndLanes* keeps = keepsFor(genre.arc, arc.section());
		uint8_t sounds = 0;
		for (int32_t p = 0; p < parts; p++) {
			sounds |= lets(p, keeps, arc.section()) && audible(p) ? static_cast<uint8_t>(1U << p) : 0;
		}
		addStep(phrase, arc, sounds, energy);
		// The piece stops on the line where the Outro ends, unless a force lands there (spec §13.2).
		if (arc.section() == SectionKind::Outro && arc.stepInSection() + 1 >= arc.sectionSteps()) {
			ended = true;
			for (int32_t i = 0; i < waiting; i++) {
				ended =
				    ended && !(landings[i].step == step + 1 && landings[i].command.kind == CommandKind::ForceSection);
			}
		}
	}
	for (int32_t i = 0; i < ArcPlan::kMaxPhrases; i++) {
		out.phrase[i].energy = static_cast<int16_t>(steps[i] > 0 ? energySum[i] / steps[i] : 0);
	}
}

Key Generator::keyAhead() const {
	const Command* waiting = pending_.waiting({CommandKind::SetKey});
	return waiting != nullptr ? keyOf(*waiting) : key();
}

Chord Generator::chord() const {
	return voiceChord(recipe_.scale, kHomeChordRoot,
	                  extensionLevel(recipe_.macros[Macro::Complexity], genre_->harmony.extensionFloor,
	                                 genre_->harmony.extensionCeiling));
}

int32_t Generator::macroLayer(SoundControl control) const {
	const auto c = static_cast<int32_t>(control);
	int32_t offset = 0;
	for (int32_t row = 0; row < kMacroLayerRows; row++) {
		if (kMacroLayerSigns[row][c] == 0) {
			continue;
		}
		const auto macro = static_cast<Macro>(row);
		// Effective, so Section offsets shade the sound with no rules of their own; measured from the genre's default,
		// where its sounds sit at home.
		const int32_t fromDefault = effectiveMacro(macro) - genre_->defaults[macro];
		offset += kMacroLayerSigns[row][c] * genre_->soundWeight[row][c] * fromDefault * kMacroLayerDepth
		          / (1000 * kMacroLayerSpan);
	}
	return offset;
}

Generator::ThrowLine Generator::throwLine() const {
	// The line is near when the step just decided is at most kThrowWindowSteps before it. The Gap's place is fixed by
	// the Build's length; a Break's comes from a waiting force, or from the Groove ending on its own.
	const auto inWindow = [](int32_t steps) { return steps >= 1 && steps <= kThrowWindowSteps; };
	const int32_t gap = gapSteps(genre_->arc.gap);
	if (arc_.section() == SectionKind::Build && gap > 0 && arc_.sectionSteps() > gap) {
		return {inWindow(arc_.sectionSteps() - gap - arc_.stepInSection()), GestureMoment::ThrowGap};
	}
	// Steps from this step to where a waiting force, End or transition lands: whichever comes first starts there.
	int32_t waitingAt = INT32_MAX;
	bool waitingBreak = false;
	for (CommandKind kind : {CommandKind::ForceSection, CommandKind::Transition}) {
		const Command* waiting = pending_.waiting({kind});
		if (waiting != nullptr && stepsToLine(*waiting) + 1 < waitingAt) {
			waitingAt = stepsToLine(*waiting) + 1;
			waitingBreak = waiting->kind == CommandKind::ForceSection
			               && static_cast<SectionKind>(waiting->target) == SectionKind::Break;
		}
	}
	const int32_t ownAt =
	    arc_.breakEndsPhrase(recipe_.macros[Macro::Motion]) ? arc_.stepsToPhraseLine() + 1 : INT32_MAX;
	if (waitingAt <= ownAt) {
		return {waitingBreak && inWindow(waitingAt), GestureMoment::ThrowBreak};
	}
	return {inWindow(ownAt), GestureMoment::ThrowBreak};
}

bool Generator::gestureActsOn(const ControlSet& set, Gesture gesture) const {
	const Lane aim = genre_->parts[set.part].gestures.aim[static_cast<int32_t>(gesture)];
	switch (set.kind) {
	case ControlSetKind::KitWide:
		return aim == Lane::None;
	case ControlSetKind::KitLane:
		return aim == set.lane;
	default:
		return true; // FX lanes always use their own row controls (spec §11.4)
	}
}

bool Generator::silencedAt(const ControlSet& set, ThrowLine line) const {
	if (line.moment == GestureMoment::ThrowGap) {
		return true; // no note starts in the Gap
	}
	const PartsAndLanes* keeps = keepsFor(genre_->arc, SectionKind::Break);
	if (set.kind != ControlSetKind::KitWide) {
		return !kept(*genre_, keeps, SectionKind::Break, set.part, set.lane);
	}
	const PartSpec& part = genre_->parts[set.part];
	for (int32_t l = 0; l < laneCount(part); l++) {
		if (kept(*genre_, keeps, SectionKind::Break, set.part, part.lanes[l].lane)) {
			return false;
		}
	}
	return true;
}

bool Generator::updateThrow(ControlSet& set, ThrowLine line, const PartsAndLanes& hits) {
	if (!gestureActsOn(set, Gesture::Throw)) {
		return false;
	}
	bool hit = hits.has(set.part, set.lane);
	if (set.kind == ControlSetKind::KitWide) {
		// The kit plays when any of its lanes does.
		const PartSpec& part = genre_->parts[set.part];
		for (int32_t l = 0; l < laneCount(part) && !hit; l++) {
			hit = (hits.lanes & laneBit(part.lanes[l].lane)) != 0;
		}
	}
	if (hit && line.near && genre_->parts[set.part].gestures.on(line.moment) && silencedAt(set, line)) {
		set.thrown = 0; // this hit may be the last before the line
		return true;
	}
	if (set.thrown < 0) {
		return false;
	}
	if (hit) {
		set.thrown = -1; // it plays again: back at home
		return true;
	}
	set.thrown = static_cast<int16_t>(set.thrown + 1);
	if (set.thrown > genre_->throwHoldSteps + kThrowDecaySteps) {
		set.thrown = -1;
	}
	return false;
}

int32_t Generator::gestureLayer(const ControlSet& set, SoundControl control, int32_t shade, int32_t rise) const {
	const GestureSpec& gestures = genre_->parts[set.part].gestures;
	const auto scaled = [&](int32_t amount, Gesture gesture, int32_t level) {
		return amount * level * gestures.depth[static_cast<int32_t>(gesture)] / (1000 * 1000);
	};
	const bool outro = shadedBy_ == SectionKind::Outro;
	const bool intro = arc_.section() == SectionKind::Intro;
	int32_t offset = 0;
	if (control == SoundControl::Brightness && shade > 0 && gestureActsOn(set, Gesture::Close)
	    && gestures.on(outro ? GestureMoment::CloseOutro : GestureMoment::CloseBreak)) {
		offset += scaled(kCloseBrightness, Gesture::Close, shade);
	}
	if (control == SoundControl::Space && shade > 0 && gestureActsOn(set, Gesture::Wash)
	    && gestures.on(outro ? GestureMoment::WashOutro : GestureMoment::WashBreak)) {
		offset += scaled(kWashSpace, Gesture::Wash, shade);
	}
	if (rise > 0 && gestureActsOn(set, Gesture::Rise)
	    && gestures.on(intro ? GestureMoment::RiseIntro : GestureMoment::RiseBuild)) {
		int32_t amount = 0;
		if (control == SoundControl::Thin) {
			amount = kRiseThin;
		}
		else if (control == SoundControl::Brightness) {
			amount = kRiseBrightness;
		}
		else if (control == SoundControl::Space && set.lane == Lane::Riser) {
			amount = kRiseRiserSpace;
		}
		offset += scaled(amount, Gesture::Rise, rise);
	}
	if (control == SoundControl::Space && set.thrown >= 0) {
		// Full while the hold lasts, then down over a beat.
		const int32_t hold = genre_->throwHoldSteps;
		const int32_t level =
		    set.thrown <= hold ? 1000 : (hold + kThrowDecaySteps - set.thrown) * 1000 / kThrowDecaySteps;
		offset += scaled(kThrowSpace, Gesture::Throw, level);
	}
	return offset;
}

int32_t Generator::driftLayer(const ControlSet& set, SoundControl control, const Step& step) const {
	const DriftSpec& drift = genre_->parts[set.part].drift;
	if (set.kind == ControlSetKind::KitWide || !drifts(control)) {
		return 0;
	}
	const int32_t depth = step.motion * kDriftDepth * drift.weight / (kMacroFull * 1000);
	if (depth == 0) {
		return 0;
	}
	if (control == SoundControl::Brightness && drift.sweepBars > 0) {
		// A triangle from -1000 up to 1000 and back over the period, from a seeded quarter, counted from step 0 so it
		// keeps to phrase lines.
		const int32_t period = drift.sweepBars * kStepsPerBar;
		const int32_t quarter = drawBelow(partKey(seed_, set.part, Lane::None, 0, Purpose::SweepPhase), 4);
		const int32_t at = (step.index + quarter * period / 4) % period;
		const int32_t triangle = at < period / 2 ? -1000 + 4000 * at / period : 3000 - 4000 * at / period;
		return triangle * depth / 1000;
	}
	// The walk: from one seeded point to the next, eased in and out, each set and control at its own seeded place
	// between them.
	const auto c = static_cast<uint32_t>(control);
	DrawKey key = partKey(seed_, set.part, set.lane, 0, Purpose::DriftPhase);
	key.position[0] = c;
	const int32_t towards = driftTowards_ + drawBelow(key, kDriftApart);
	const uint32_t point = driftPoint_ + static_cast<uint32_t>(towards / kDriftApart);
	const int64_t x = int64_t{towards % kDriftApart} * 1000 / kDriftApart;
	const int64_t eased = x * x * (3000 - 2 * x) / (1000 * 1000);
	key.purpose = Purpose::DriftPoint;
	key.position[0] = point;
	key.position[1] = c;
	const int32_t from = drawBelow(key, 2001) - 1000;
	key.position[0] = point + 1;
	const int32_t to = drawBelow(key, 2001) - 1000;
	return static_cast<int32_t>(from + (to - from) * eased / 1000) * depth / 1000;
}

void Generator::updateControls(const Step& step) {
	// Reset: every Groove's downbeat snaps the gestures back (the Drop, the first Groove after an Intro, a forced one).
	const bool reset = step.index > 0 && arc_.section() == SectionKind::Groove && arc_.sectionStartsHere();
	if (reset) {
		shadedFrom_ = kNever;
		for (int32_t i = 0; i < controlCount_; i++) {
			controls_[i].thrown = -1;
		}
	}
	const SectionKind section = arc_.section();
	if (arc_.sectionStartsHere() && (section == SectionKind::Break || section == SectionKind::Outro)) {
		shadedFrom_ = shadedFrom_ == kNever ? step.index : shadedFrom_;
		shadedBy_ = section;
	}
	const int32_t shade = shadedFrom_ == kNever
	                          ? 0
	                          : std::min(step.index - shadedFrom_ + 1, kGestureEaseSteps) * 1000 / kGestureEaseSteps;
	int32_t rise = 0;
	if ((section == SectionKind::Build || section == SectionKind::Intro) && arc_.sectionSteps() > 0) {
		const int32_t x = std::min(arc_.stepInSection() + 1, arc_.sectionSteps()) * 1000 / arc_.sectionSteps();
		rise = x * x / 1000; // an ease-in, peaking on the Section's last step
	}
	// The notes this step decided, muted ones too, so a mute never changes a Throw.
	PartsAndLanes hits;
	for (int32_t i = events_.stepStart(); i < events_.size(); i++) {
		const Event& e = events_.at(i);
		if (e.kind == EventKind::NoteOn) {
			hits.parts |= static_cast<uint8_t>(e.lane == Lane::None ? 1U << e.part : 0U);
			hits.lanes |= e.lane == Lane::None ? 0 : laneBit(e.lane);
		}
	}
	const ThrowLine line = throwLine();
	int32_t macro[kNumSoundControls];
	for (int32_t c = 0; c < kNumSoundControls; c++) {
		macro[c] = macroLayer(static_cast<SoundControl>(c));
	}

	for (int32_t i = 0; i < controlCount_; i++) {
		ControlSet& set = controls_[i];
		const bool spaceSnaps = updateThrow(set, line, hits);
		for (int32_t c = 0; c < kNumSoundControls; c++) {
			const auto control = static_cast<SoundControl>(c);
			if (!setMoves(set.kind, control)) {
				continue;
			}
			// Macro + gesture + drift, summed and clamped to the control's range (spec §11.3). A Drums lane takes no
			// macro layer: that moves the kit-wide controls.
			const int32_t sum = (set.kind == ControlSetKind::KitLane ? 0 : macro[c])
			                    + gestureLayer(set, control, shade, rise) + driftLayer(set, control, step);
			const int32_t target = std::clamp<int32_t>(sum, -kControlFull, kControlFull);
			if (target == set.sent[c]) {
				continue;
			}
			set.sent[c] = static_cast<int8_t>(target);
			// The piece's first step jumps there, as does a Reset; a Throw jumps on its hit and back on the next.
			const bool snap = step.index == 0 || reset || (spaceSnaps && control == SoundControl::Space);
			push(Event::controlChange(step.tick, set.part, set.lane, control, set.sent[c], snap));
		}
	}

	// The walk moves on by Motion: a point a phrase at Motion 25 (spec §11.5).
	driftTowards_ += step.motion;
	driftPoint_ += static_cast<uint32_t>(driftTowards_ / kDriftApart);
	driftTowards_ %= kDriftApart;
}

int32_t Generator::effectiveMacro(Macro macro) const {
	const int32_t centre = recipe_.macros[macro];
	return std::clamp<int32_t>(centre + arc_.offset(*genre_, macro, centre), 0, kMacroFull);
}

void Generator::decideNextStep(SteeringQueue& queue, SteeringLog* log) {
	const int32_t n = nextStep_;
	if (stoppedDeciding()) {
		return; // a transition's start step: the new roster decides from here
	}
	events_.beginStep();

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
	step.energy = effectiveMacro(Macro::Energy);
	step.density = effectiveMacro(Macro::Density);
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
	fillRules(step);
	updateSilent(step);

	// Parts in the genre's roster order, which is role order unless the genre says otherwise (spec §5.5).
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		decidePart(p, step);
	}
	if (arc_.stepInBar() == 0) {
		lateEntries(step);
	}
	updateControls(step);

	queueNoteOffsBefore(decisionTick(n + 1));
	nextStep_ = n + 1;
	if (stoppedDeciding()) {
		endAtStop(); // the Build's or the Outro's last step
	}
}

void Generator::endAtStop() {
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
		nextStep_++;
	}
}

void Generator::decidePart(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	if (!spec.lanes.empty()) {
		decideKit(partIndex, step);
	}
	else if (spec.flavour == Flavour::Loop) {
		decideLoop(partIndex, step);
	}
	parts_[partIndex].newTake = false;
}

int32_t Generator::pickEntry(const PatternState& state, std::span<const Pattern> pool, TierBand band, uint8_t part,
                             Lane lane, int32_t complexity, PickPurposes purposes) const {
	// The tier draw holds for the whole pick, so each wrap re-checks with the same draw. Within the tier, a weighted
	// pick keyed on the pick and the tier, so it changes only when one of them does.
	const uint8_t take = takes_.take[part];
	const int32_t tier = playingTier(pool, bandTier(band, complexity,
	                                                onPhrase(partKey(seed_, part, lane, take, purposes.tier),
	                                                         state.pick.groove, state.pick.phrase)));
	DrawKey key = onPhrase(partKey(seed_, part, lane, take, purposes.pick), state.pick.groove, state.pick.phrase,
	                       static_cast<uint32_t>(tier));
	return drawInTier(key, pool, tier);
}

bool Generator::placePick(PatternState& state, uint8_t part, Lane lane, const Step& step) const {
	bool newPick = step.drop; // keyed on the Groove's index
	if (step.repickLine) {
		// Whether a lane re-picks leaves out the take, so every take changes at the same phrase lines.
		DrawKey key = onPhrase(partKey(seed_, part, lane, 0, Purpose::PatternRepick), step.groove, step.phrase);
		newPick = drawChance(key, scaleChance(genre_->repickChance, curves::patternRepick, step.motion));
	}
	if (!newPick) {
		return false;
	}
	// A frozen part holds its pick, and takes this one when it unfreezes (spec §6.7).
	const bool frozen = partFrozen(part);
	const PatternState::Pick here{step.groove, step.phrase, static_cast<uint16_t>(step.index / kStepsPerBar)};
	(frozen ? state.waiting : state.pick) = here;
	state.hasWaiting = state.hasWaiting || frozen;
	return !frozen;
}

Generator::PickChange Generator::updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band,
                                            uint8_t part, Lane lane, const Step& step) const {
	const bool newPick = placePick(state, part, lane, step);
	const bool frozen = partFrozen(part);
	// A take lands on a phrase line and picks what that take would have now: the same pick, keyed on the new take.
	bool fresh = newPick || state.entry < 0 || parts_[part].newTake;
	if (!fresh && step.index % pool[state.entry].length != 0) {
		return PickChange::None;
	}
	if (!fresh && frozen) {
		return PickChange::Wrap; // no tier check while frozen: the pick holds
	}
	// The tier is checked each time the pattern wraps. If it hasn't changed, neither has the entry.
	int32_t entry =
	    pickEntry(state, pool, band, part, lane, step.complexity, {Purpose::ComplexityTier, Purpose::PatternPick});
	if (!fresh && entry == state.entry) {
		return PickChange::Wrap;
	}
	state.entry = static_cast<int8_t>(entry);
	return PickChange::NewPick;
}

bool Generator::plays(const Pattern& pattern, int32_t patternStep, uint8_t part, Lane lane, const PatternState& state,
                      int32_t density) const {
	int32_t importance = pattern.importance[patternStep];
	if (importance == 0) {
		return false;
	}
	if (importance == kAnchor) {
		return true;
	}
	// Each step's jittered importance is fixed while the pick holds, so more Density only ever adds hits.
	DrawKey key = partKey(seed_, part, lane, takes_.take[part], Purpose::DensityJitter);
	key = onPick(key, state, 0, static_cast<uint32_t>(patternStep));
	int32_t jitter = drawBelow(key, 2 * genre_->densityJitter + 1) - genre_->densityJitter;
	return importance * 100 + jitter >= genre_->densityCurve.threshold(density);
}

// ---- Planned onsets and interplay rules (spec §5.5) ----

namespace {

/// The index of `lane` in a Kit part's lanes, or -1.
int32_t laneIndexOf(const PartSpec& part, Lane lane) {
	for (int32_t l = 0; l < laneCount(part); l++) {
		if (part.lanes[l].lane == lane) {
			return l;
		}
	}
	return -1;
}

/// The step `back` steps before `step`, wrapping inside the pattern pass `step` is in.
int32_t stepBack(int32_t step, int32_t back, int32_t length) {
	int32_t inPattern = step % length;
	return step - inPattern + (inPattern - back + length) % length;
}

/// Effective strengths run 0 to this: 100% in units fine enough for Complexity's every macro unit.
constexpr int32_t kFullStrength = 100 * kMacroFull;

} // namespace

Generator::Source Generator::source(int32_t part, int32_t laneIndex) const {
	const PartSpec& spec = genre_->parts[part];
	if (laneIndex < 0) {
		return {spec.pool, spec.rules, Lane::None, &parts_[part].lanes[0]};
	}
	const LaneSpec& lane = spec.lanes[laneIndex];
	return {lane.pool, lane.rules, lane.lane, &parts_[part].lanes[laneIndex]};
}

bool Generator::planned(int32_t part, int32_t laneIndex, int32_t step, int32_t density) const {
	Source from = source(part, laneIndex);
	if (from.pool.empty() || from.state->entry < 0) {
		return false;
	}
	const Pattern& pattern = from.pool[from.state->entry];
	int32_t patternStep = step % pattern.length;
	const auto partId = static_cast<uint8_t>(part);
	if (!plays(pattern, patternStep, partId, from.lane, *from.state, density)) {
		return false;
	}
	if (pattern.importance[patternStep] == kAnchor) {
		return true; // rules never remove an anchor
	}
	// With several rules, an onset plays only if it survives all of them. Each draw is fixed while the pick holds,
	// so the filtered shape loops and a higher strength only ever removes more.
	for (int32_t r = 0; r < static_cast<int32_t>(from.rules.size()); r++) {
		const InterplayRule& rule = from.rules[r];
		if (!against(rule, step, density)) {
			continue;
		}
		DrawKey key = onPick(partKey(seed_, partId, from.lane, takes_.take[part], Purpose::Interplay), *from.state,
		                     static_cast<uint16_t>(r), static_cast<uint32_t>(patternStep));
		if (drawBelow(key, kFullStrength) < effectiveStrength(rule)) {
			return false;
		}
	}
	return true;
}

bool Generator::targetPlanned(const RuleTarget& target, int32_t step, int32_t back, int32_t density) const {
	const PartSpec& spec = genre_->parts[target.part];
	auto plannedBack = [&](int32_t laneIndex) {
		Source from = source(target.part, laneIndex);
		if (from.pool.empty() || from.state->entry < 0) {
			return false;
		}
		int32_t length = from.pool[from.state->entry].length;
		return planned(target.part, laneIndex, stepBack(step, back, length), density);
	};
	if (target.lane != Lane::None) {
		int32_t laneIndex = laneIndexOf(spec, target.lane);
		return laneIndex >= 0 && plannedBack(laneIndex);
	}
	if (spec.lanes.empty()) {
		return plannedBack(-1);
	}
	// A Kit part reads the union of its lanes.
	for (int32_t l = 0; l < laneCount(spec); l++) {
		if (plannedBack(l)) {
			return true;
		}
	}
	return false;
}

bool Generator::against(const InterplayRule& rule, int32_t step, int32_t density) const {
	switch (rule.kind) {
	case RuleKind::Lock:
		// Free where the target plays, or with `off` an 8th after it.
		return !targetPlanned(rule.target, step, rule.offBeat ? kStepsPerBeat / 2 : 0, density);
	}
	return false;
}

int32_t Generator::effectiveStrength(const InterplayRule& rule) const {
	int32_t moved = rule.sensitivity * (recipe_.macros[Macro::Complexity] - genre_->defaults[Macro::Complexity]);
	return std::clamp<int32_t>(rule.strength * kMacroFull + moved, 0, kFullStrength);
}

int32_t Generator::energyLayer(int32_t energy) const {
	int64_t layer = static_cast<int64_t>(energy - kMacroFull / 2) * 2 * kEnergyVelocityRange
	                * genre_->energyVelocityWeight / (static_cast<int64_t>(kMacroFull) * 2 * 1000);
	return static_cast<int32_t>(layer);
}

uint8_t Generator::velocity(int32_t baseVelocity, int32_t energy) const {
	return static_cast<uint8_t>(std::clamp<int32_t>(baseVelocity + energyLayer(energy), 1, 127));
}

// ---- Gates and lines (spec §5.6, §12.5) ----

bool Generator::gateOpen(int32_t part, Lane lane) const {
	if (!parts_[part].gateOpen) {
		return false;
	}
	int32_t laneIndex = lane == Lane::None ? -1 : laneIndexOf(genre_->parts[part], lane);
	return laneIndex < 0 || parts_[part].laneGateOpen[laneIndex];
}

void Generator::updateGates(const Step& step) {
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const PartSpec& spec = genre_->parts[p];
		PartState& part = parts_[p];
		part.gateOpen = gateOpens(part.gateOpen, spec.densityGate, step.density);
		for (int32_t l = 0; l < laneCount(spec); l++) {
			part.laneGateOpen[l] = gateOpens(part.laneGateOpen[l], spec.lanes[l].densityGate, step.density);
		}
		// A closing gate ends its notes on the line, holding them back: a gate that reopens while one still lasts
		// to the end of that bar lets it enter late. Drum and FX one-shots play out anyway. An opening gate lets what
		// it held back enter late, after the parts (lateEntries).
		sounding_.forEach(p, [&](SoundingNote& note) {
			if (!gateOpen(p, note.lane) && note.offTick > step.tick) {
				held_.hold(p, note);
				note.offTick = step.tick;
			}
		});
	}
}

void Generator::cutSectionLine(const Step& step) {
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		// An FX note held back by its gate belongs to the Section that placed it, too.
		held_.forEach(p, [](SoundingNote& note) {
			if (isFxLane(note.lane)) {
				note.used = false;
			}
		});
		sounding_.forEach(p, [&](SoundingNote& note) {
			if (note.offTick > step.tick) {
				// An FX note belongs to the Section that placed it, so it ends here and its tail rings out: a Riser
				// cut short by a forced Section never comes back in the next one.
				if (!isFxLane(note.lane)) {
					held_.hold(p, note);
				}
				note.offTick = step.tick;
			}
		});
	}
}

void Generator::lateEntries(const Step& step) {
	const int32_t barEnd = step.tick + kStepsPerBar * kTicksPerStep;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		held_.forEach(p, [&](SoundingNote& note) {
			if (!gateOpen(p, note.lane) || keptOut(step, p, note.lane)) {
				return; // still gated out, or kept out by a section rule or a Rest: it waits
			}
			note.used = false;
			// The note started earlier and lasts at least to the end of this bar: its remainder starts on the line.
			// A shorter remainder is skipped.
			if (note.onTick < step.tick && note.offTick >= barEnd) {
				noteOn(p, {note.lane, note.pitch, note.baseVelocity, velocity(note.baseVelocity, step.energy),
				           step.tick, note.offTick});
			}
		});
	}
}

int32_t Generator::keyFloor(int32_t partIndex, Lane lane, uint8_t pitch) const {
	const Event key = Event::noteOn(0, static_cast<uint8_t>(partIndex), lane, pitch, 0);
	int32_t latest = INT32_MIN;
	for (int32_t i = 0; i < events_.size(); i++) {
		const Event& e = events_.at(i);
		if (e.kind == EventKind::NoteOn && e.sameKey(key) && e.tick > latest) {
			latest = e.tick;
		}
	}
	sounding_.forEach(partIndex, [&](const SoundingNote& note) {
		if (note.is(key) && note.onTick > latest) {
			latest = note.onTick;
		}
	});
	return latest == INT32_MIN ? INT32_MIN : latest + 1;
}

void Generator::decideKit(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	const int32_t lanes = laneCount(spec);
	LaneNote notes[kMaxLanesPerPart] = {};
	bool rolls[kMaxLanesPerPart] = {};
	for (int32_t l = 0; l < lanes; l++) {
		const LaneSpec& lane = spec.lanes[l];
		LaneNote& note = notes[l];
		if (lane.pool.empty() && !lane.fillPool.empty()) {
			// A lane that only Swaps plays still changes its pick at Drops and re-picks, so its fills change with it.
			placePick(parts_[partIndex].lanes[l], static_cast<uint8_t>(partIndex), lane.lane, step);
		}
		if (!lane.pool.empty()) {
			PatternState& pick = parts_[partIndex].lanes[l];
			updatePick(pick, lane.pool, lane.band, static_cast<uint8_t>(partIndex), lane.lane, step);
			const Pattern& pattern = lane.pool[pick.entry];
			note.patternStep = step.index % pattern.length;
			if (planned(partIndex, l, step.index, step.density)) {
				note = {true, pattern.level[note.patternStep], lane.gateTicks, note.patternStep};
			}
		}
		// A Swap replaces the lane's pattern before Density: its fill pattern, picked as the lane's patterns are and
		// keyed on the lane's pick, so the same pick plays the same fill. Interplay rules have read the normal plan.
		if (step.swap.has(partIndex, lane.lane) && !lane.fillPool.empty()) {
			const PatternState& lanePick = parts_[partIndex].lanes[l];
			const int32_t fill = pickEntry(lanePick, lane.fillPool, lane.band, static_cast<uint8_t>(partIndex),
			                               lane.lane, step.complexity, {Purpose::FillTier, Purpose::FillPattern});
			const Pattern& pattern = lane.fillPool[fill];
			const int32_t patternStep = step.index % pattern.length;
			note = {};
			if (plays(pattern, patternStep, static_cast<uint8_t>(partIndex), lane.lane, fillPick(lanePick, lane, fill),
			          step.density)) {
				note = {true, pattern.level[patternStep], lane.gateTicks, patternStep, static_cast<int8_t>(fill)};
			}
		}
		// Section rules come after the plan that interplay rules read (spec §9.5). The Phrase accent is a single hit,
		// merged with any hit the lane has there; FX lanes play only by section rules; the roll replaces the lane's
		// own hits while it plays.
		if (step.accent && lane.lane == genre_->arc.accentLane) {
			note = {true, VelocityLevel::Accent, lane.gateTicks, note.patternStep};
		}
		if (int32_t fx = fxNoteTicks(lane, step); fx > 0) {
			note = {true, VelocityLevel::Normal, fx, 0};
		}
		if (step.rollStage != RollStage::None && lane.lane == genre_->arc.roll.lane) {
			rolls[l] = true;
			note.on = false;
		}
		if (silenced(step, partIndex, lane.lane)) {
			note.on = false;
			rolls[l] = false;
		}
		// A Rest comes after everything else. The roll lane is never in it while it rolls (Build roll > fill).
		if (step.rest.has(partIndex, lane.lane)) {
			note.on = false;
		}
	}
	// Replace pairs come after the plan that interplay rules read, before any voice starts (spec §5.7). A lane that
	// doesn't land (gated out or silenced) replaces nothing; a rolling lane lands where it hits, whatever its gate.
	// Nothing replaces the roll: the roll lane always plays (spec §9.5).
	const bool rollHitsHere = rollHits(step.rollStage, step.index) > 0;
	for (const ReplacePair& pair : genre_->replacePairs) {
		int32_t replacing = laneIndexOf(spec, pair.replacing);
		int32_t replaced = laneIndexOf(spec, pair.replaced);
		if (replacing >= 0 && replaced >= 0
		    && ((rolls[replacing] && rollHitsHere) || (notes[replacing].on && gateOpen(partIndex, pair.replacing)))) {
			notes[replaced].on = false;
		}
	}
	for (int32_t l = 0; l < lanes; l++) {
		if (rolls[l]) {
			playRoll(partIndex, l, step);
		}
		else if (notes[l].on) {
			playLane(partIndex, l, notes[l], step);
		}
	}
}

void Generator::playLane(int32_t partIndex, int32_t laneIndex, const LaneNote& note, const Step& step) {
	const LaneSpec& spec = genre_->parts[partIndex].lanes[laneIndex];
	const PatternState& pick = parts_[partIndex].lanes[laneIndex];
	express(partIndex, spec.expression, note.fill >= 0 ? fillPick(pick, spec, note.fill) : pick, note.patternStep,
	        {spec.lane, 0, note.level, note.length}, step);
}

PatternState Generator::fillPick(const PatternState& lanePick, const LaneSpec& lane, int32_t fill) {
	PatternState pick = lanePick;
	pick.entry = static_cast<int8_t>(static_cast<int32_t>(lane.pool.size()) + fill);
	return pick;
}

// ---- Section rules (spec §8.3, §9.5) ----

void Generator::sectionRules(Step& step) {
	const ArcSpec& rules = genre_->arc;
	const SectionKind kind = arc_.section();
	step.keeps = keepsFor(rules, kind);
	if (kind == SectionKind::Build) {
		const int32_t toDrop = arc_.stepsToDrop();
		const int32_t gap = gapSteps(rules.gap);
		const int32_t cut = rules.buildCutBars * kStepsPerBar;
		step.gap = toDrop <= gap;
		if (toDrop <= cut) {
			step.cut = rules.buildCut;
		}
		// The Gap and the cut end the pitched notes they stop where they start. Drum and FX tails ring out.
		if (step.gap && (toDrop == gap || step.sectionStart)) {
			sounding_.endPitchedBy(step.tick);
		}
		else if (toDrop <= cut && (toDrop == cut || step.sectionStart)) {
			for (int32_t p = 0; p < partCount(*genre_); p++) {
				if (rules.buildCut.has(p, Lane::None)) {
					sounding_.endPitchedBy(p, step.tick);
				}
			}
		}

		// The roll climbs through its stages towards the Gap. The start and the stage fractions are measured from the
		// Drop, so with Build lengths powers of two every change lands on the grid, and the Gap then takes the end.
		// Complexity sets only how early it starts: one that starts late joins at the stage already reached, and
		// velocity climbs to the Gap (spec §8.3).
		// It starts no later than the last grid step before the Gap, so a short Build with a Gap still rolls.
		const int32_t length = arc_.sectionSteps();
		const int32_t share =
		    kRollStartAtZero
		    + (1000 - kRollStartAtZero) * std::clamp<int32_t>(step.complexity, 0, kMacroFull) / kMacroFull;
		const int32_t latest = gap + rollGrid(rollStageAt(rules.roll, gap + 1, length));
		const int32_t start = std::max(length * share / 1000, latest);
		if (rules.roll.lane != Lane::None && toDrop > gap && toDrop <= start) {
			const int32_t span = length - gap; // the roll's whole run, to the Gap
			const int32_t left = toDrop - gap;
			step.rollStage = rollStageAt(rules.roll, toDrop, length);
			step.rollVelocity =
			    static_cast<int16_t>(kRollVelocity[0] + (kRollVelocity[1] - kRollVelocity[0]) * (span - left) / span);
		}
	}

	// The Phrase accent: always on a Drop and after a fill that covered the phrase's last step, and on other Groove
	// phrase starts by a keyed chance that Motion scales along the fill's curve.
	if (rules.accentLane != Lane::None && arc_.stepInPhrase() == 0) {
		step.accent = filled_;
		if (kind == SectionKind::Groove) {
			DrawKey key = partKey(seed_, kArcPart, rules.accentLane, 0, Purpose::PhraseAccent);
			key = onPhrase(key, arc_.occurrence(), static_cast<uint16_t>(arc_.phraseInSection()));
			step.accent = step.accent || arc_.dropStartsHere()
			              || drawChance(key, scaleChance(rules.accentChance, curves::phraseAccent, step.motion));
		}
	}
}

// ---- Fills (spec §8.7) ----

void Generator::fillRules(Step& step) {
	const std::span<const FillEntry> fills = genre_->fills;
	const int32_t inPhrase = arc_.stepInPhrase();
	if (inPhrase == 0) {
		autoFill_ = static_cast<int8_t>(drawAutoFill(step));
	}
	// A held fill overrides the automatic one. No fill covers a phrase's first step: a held one pauses there and
	// carries on from the next. The Gap silences fills too.
	int32_t entry = -1;
	if (heldFill_ >= 0) {
		entry = inPhrase > 0 ? heldFill_ : -1;
	}
	else if (autoFill_ >= 0 && (arc_.section() == SectionKind::Groove || arc_.section() == SectionKind::Intro)
	         && inPhrase >= autoFillStart(fills[autoFill_].beats)) {
		entry = autoFill_;
	}
	if (step.gap) {
		entry = -1;
	}
	step.rest = {};
	step.swap = {};
	if (entry >= 0) {
		PartsAndLanes mask = fills[entry].mask;
		// The roll lane keeps rolling under a fill, which changes only the other lanes (Build roll > fill).
		if (step.rollStage != RollStage::None && genre_->arc.roll.lane != Lane::None) {
			mask.lanes &= static_cast<uint16_t>(~laneBit(genre_->arc.roll.lane));
		}
		(fills[entry].kind == FillKind::Rest ? step.rest : step.swap) = mask;
	}

	// As a Rest starts, the notes still sounding on its pitched parts end, so their release plays: a clean drop. Drum
	// one-shots play out. A held Rest that pauses on a phrase's first step starts again on the next.
	uint8_t rested = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		if (genre_->parts[p].lanes.empty() && step.rest.has(p, Lane::None)) {
			rested |= static_cast<uint8_t>(1U << p);
			if ((rested_ & (1U << p)) == 0) {
				sounding_.endPitchedBy(p, step.tick);
			}
		}
	}
	rested_ = rested;
	filled_ = entry >= 0;
}

int32_t Generator::autoFillStart(int32_t beats) const {
	// It ends on the phrase line and never covers the phrase's first step.
	return std::max<int32_t>(genre_->phraseSteps() - beats * kStepsPerBeat, 1);
}

int32_t Generator::drawAutoFill(const Step& step) const {
	const std::span<const FillEntry> fills = genre_->fills;
	const SectionKind kind = arc_.section();
	// Only in Groove phrases and the Intro's last phrase: not in Breaks, Builds or the Outro.
	const bool introEnd =
	    kind == SectionKind::Intro && arc_.stepInSection() + genre_->phraseSteps() == arc_.sectionSteps();
	if (fills.empty() || (kind != SectionKind::Groove && !introEnd)) {
		return -1;
	}
	int32_t chance = genre_->fillChance;
	if (genre_->sectionEndChance != kSameAsFillChance && lastGroovePhrase()) {
		chance = genre_->sectionEndChance;
	}
	// Keyed on the Section and the phrase, never on a take.
	const DrawKey key =
	    onPhrase(partKey(seed_, kArcPart, Lane::None, 0, Purpose::Fill), arc_.occurrence(),
	             static_cast<uint16_t>(arc_.phraseInSection()), fieldWord(static_cast<uint32_t>(kind), kFillWhether));
	if (!drawChance(key, scaleChance(chance, curves::fill, step.motion))) {
		return -1;
	}
	return drawWeighted(withField(key, kFillWhich), fills);
}

bool Generator::lastGroovePhrase() const {
	if (arc_.section() != SectionKind::Groove || pending_.has(CommandKind::End)) {
		return false;
	}
	// A forced Section counts if it was queued by now, the phrase's first step: it lands within the phrase.
	if (const Command* force = waitingForce(); force != nullptr) {
		const auto kind = static_cast<SectionKind>(force->target);
		return kind == SectionKind::Break || kind == SectionKind::Build;
	}
	return arc_.grooveEndsInPhrase(recipe_.macros[Macro::Motion]);
}

bool Generator::barBeforeAutoFill() const {
	if (autoFill_ < 0) {
		return false;
	}
	// The bar before the one the fill starts in, when that bar is in the phrase.
	const int32_t fillBar = autoFillStart(genre_->fills[autoFill_].beats) / kStepsPerBar;
	return arc_.stepInBar() == 0 && arc_.barInPhrase() == fillBar - 1;
}

bool Generator::silenced(const Step& step, int32_t part, Lane lane) const {
	if (step.gap || step.cut.has(part, lane)) {
		return true;
	}
	return !kept(*genre_, step.keeps, arc_.section(), part, lane);
}

void Generator::updateSilent(const Step& step) {
	silent_ = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const PartSpec& spec = genre_->parts[p];
		bool plays = parts_[p].gateOpen;
		if (plays && spec.lanes.empty()) {
			plays = !silenced(step, p, Lane::None);
		}
		else if (plays) {
			// A Kit part sounds if any of its lanes may: its gate open and no section rule silencing it.
			plays = false;
			for (int32_t l = 0; l < laneCount(spec) && !plays; l++) {
				plays = parts_[p].laneGateOpen[l] && !silenced(step, p, spec.lanes[l].lane);
			}
		}
		silent_ |= plays ? 0 : static_cast<uint8_t>(1U << p);
	}
}

int32_t Generator::fxNoteTicks(const LaneSpec& lane, const Step& step) const {
	if (!step.sectionStart) {
		return 0;
	}
	switch (lane.lane) {
	case Lane::Riser:
		// One note across the Build, ending on the Drop.
		return arc_.section() == SectionKind::Build ? arc_.sectionSteps() * kTicksPerStep : 0;
	case Lane::Fall:
		return arc_.section() == SectionKind::Break ? lane.gateTicks : 0;
	case Lane::Impact:
		return arc_.dropStartsHere() ? lane.gateTicks : 0;
	default:
		return 0; // the Siren lands with Dub
	}
}

void Generator::playRoll(int32_t partIndex, int32_t laneIndex, const Step& step) {
	// The roll lane always plays in a Build, whatever its Density gate (spec §8.3, §9.5).
	const LaneSpec& spec = genre_->parts[partIndex].lanes[laneIndex];
	// The roll decides its steps, not Density, and plays no Lean, spread or Ornament.
	const int32_t hits = rollHits(step.rollStage, step.index);
	if (hits == 0) {
		return;
	}
	const int32_t spacing = kTicksPerStep / hits;
	const int32_t length = spacing < spec.gateTicks ? spacing : spec.gateTicks;
	const int32_t floor = keyFloor(partIndex, spec.lane, 0);
	for (int32_t i = 0; i < hits; i++) {
		int32_t on = step.tick + i * spacing;
		if (on < floor) {
			continue;
		}
		noteOn(partIndex,
		       {spec.lane, 0, step.rollVelocity, velocity(step.rollVelocity, step.energy), on, on + length, i > 0});
	}
}

void Generator::express(int32_t partIndex, const Expression& expression, const PatternState& pick, int32_t patternStep,
                        const Decided& decided, const Step& step) {
	const auto part = static_cast<uint8_t>(partIndex);
	const uint8_t take = takes_.take[partIndex];

	// Velocity spread: a seeded nudge after Energy's layer and before clamping (spec §8.4). Kept in the base, so a
	// late entry and every Ornament hit take it too.
	int32_t base = kLevelVelocity[static_cast<int32_t>(decided.level)];
	if (expression.spread > 0) {
		DrawKey key = partKey(seed_, part, decided.lane, take, Purpose::VelocitySpread);
		if (expression.spreadMode == SpreadMode::Repeat) {
			key = onPick(key, pick, 0, static_cast<uint32_t>(patternStep));
		}
		else {
			key.position[0] = static_cast<uint32_t>(heldStep(partIndex, step.index)); // freeze replays its phrase
			key.position[1] = UINT32_MAX;                                             // apart from every Repeat key
		}
		base += drawBelow(key, 2 * expression.spread + 1) - expression.spread;
	}

	// Lean: the whole note moves by its 16th's offset, clamped to half a 16th either side. Never earlier than a
	// Section's first step, so Compose clips stay self-contained and downbeats land together (spec §8.5).
	int32_t lean = std::clamp<int32_t>(expression.lean[step.index % kStepsPerBeat], -kMaxLean, kMaxLean);
	if (step.sectionStart && lean < 0) {
		lean = 0;
	}
	// Steps that lean into each other on one key never cross: a hit starts after the last one on its key.
	const int32_t floor = keyFloor(partIndex, decided.lane, decided.pitch);
	const int32_t start = step.tick + lean > floor ? step.tick + lean : floor;

	Note note{decided.lane, decided.pitch,         static_cast<int16_t>(base), velocity(base, step.energy),
	          start,        start + decided.length};
	if (!gateOpen(partIndex, decided.lane)) {
		// Gated out: held back plain, for late entry (spec §5.6).
		held_.hold(partIndex, {note.onTick, note.offTick, note.baseVelocity, note.lane, note.pitch, true});
		return;
	}

	const OrnamentKind ornament = ornamentFor(partIndex, expression.ornaments, pick, patternStep, decided.lane);
	if (decided.tie && ornament == OrnamentKind::None
	    && sounding_.extend(partIndex, note.lane, note.pitch, note.onTick, note.offTick)) {
		return; // a slide onto the same note ties
	}
	switch (ornament) {
	case OrnamentKind::None:
		break;
	case OrnamentKind::Ratchet2:
	case OrnamentKind::Ratchet3:
	case OrnamentKind::Ratchet4: {
		// Even hits inside the 16th, the spacing kept from the shifted start and shrunk so the last hit stays in the
		// step. Each lasts the spacing, or the note if that's shorter, and plays softer than the one before.
		const int32_t ratchet = static_cast<int32_t>(ornament) - static_cast<int32_t>(OrnamentKind::Ratchet2);
		const int32_t hits = ratchet + 2;
		const int32_t room = step.tick + kTicksPerStep - 1 - start;
		int32_t spacing = kRatchetSpacing[ratchet];
		if (spacing * (hits - 1) > room) {
			spacing = room / (hits - 1);
		}
		if (spacing < 1) {
			break;
		}
		const int32_t length = spacing < decided.length ? spacing : decided.length;
		for (int32_t i = 0; i < hits; i++) {
			int32_t hitBase = base - i * kRatchetSoftening;
			int32_t on = start + i * spacing;
			noteOn(partIndex, {decided.lane, decided.pitch, static_cast<int16_t>(hitBase),
			                   velocity(hitBase, step.energy), on, on + length, i > 0});
		}
		return;
	}
	case OrnamentKind::Flam: {
		// A soft grace hit a gap before the main hit, which never moves. The grace hit lands no earlier than -12, or
		// than the line on a Section's first step; the gap shrinks to fit, and with none left the note plays plain.
		int32_t limit = step.sectionStart ? step.tick : step.tick - kMaxLean;
		limit = limit > floor ? limit : floor;
		int32_t grace = start - expression.ornaments.flamGap;
		grace = grace < limit ? limit : grace;
		if (grace < start) {
			int32_t graceBase = base * kFlamGraceVelocity / 1000;
			auto graceVelocity =
			    static_cast<uint8_t>(std::clamp<int32_t>(note.velocity * kFlamGraceVelocity / 1000, 1, 127));
			noteOn(partIndex,
			       {decided.lane, decided.pitch, static_cast<int16_t>(graceBase), graceVelocity, grace, start, true});
		}
		break;
	}
	}
	noteOn(partIndex, note);
}

Generator::OrnamentKind Generator::ornamentFor(int32_t partIndex, const Ornaments& ornaments, const PatternState& pick,
                                               int32_t patternStep, Lane lane) const {
	const struct Choice {
		uint8_t weight;
	} choices[] = {{ornaments.ratchet[0]}, {ornaments.ratchet[1]}, {ornaments.ratchet[2]}, {ornaments.flam}};
	if (choices[0].weight + choices[1].weight + choices[2].weight + choices[3].weight == 0) {
		return OrnamentKind::None;
	}
	// The live Complexity sets the chance between the genre's floor and ceiling (spec §8.2). Each draw is keyed on
	// the pick and the step within the pattern, so a take re-rolls them, freeze holds them, and a Complexity turn
	// only ever adds Ornaments on the way up.
	int32_t rate = ornaments.rateFloor
	               + (ornaments.rateCeiling - ornaments.rateFloor) * recipe_.macros[Macro::Complexity] / kMacroFull;
	DrawKey key = partKey(seed_, static_cast<uint8_t>(partIndex), lane, takes_.take[partIndex], Purpose::Ornament);
	if (!drawChance(onPick(key, pick, 0, static_cast<uint32_t>(patternStep)), rate)) {
		return OrnamentKind::None;
	}
	static_assert(static_cast<int32_t>(OrnamentKind::Ratchet3) == static_cast<int32_t>(OrnamentKind::Ratchet2) + 1
	                  && static_cast<int32_t>(OrnamentKind::Ratchet4)
	                         == static_cast<int32_t>(OrnamentKind::Ratchet2) + 2
	                  && static_cast<int32_t>(OrnamentKind::Flam) == static_cast<int32_t>(OrnamentKind::Ratchet2) + 3,
	              "choices[] lists ratchets of 2, 3 and 4 hits and the flam in OrnamentKind's order from Ratchet2");
	int32_t which =
	    drawWeighted(onPick(key, pick, 1, static_cast<uint32_t>(patternStep)), std::span<const Choice>(choices));
	return static_cast<OrnamentKind>(static_cast<int32_t>(OrnamentKind::Ratchet2) + which);
}

// ---- Loop lines (spec §5.2) ----

namespace {

DrawKey lineKey(uint32_t seed, uint8_t part, uint8_t take, Purpose purpose, const PatternState& pick, uint16_t passes,
                int32_t lineStep, int32_t field) {
	return onPick(partKey(seed, part, Lane::None, take, purpose), pick, passes,
	              fieldWord(static_cast<uint32_t>(lineStep), field));
}

/// Fields of one line step's draws.
enum LineField : int32_t { kDegree, kOctaveUp, kOctaveDown, kAccent, kSlide, kMutate };
/// Fields of an Answer's draws: the tier's chance (0, the key as made), which transform, and a shift's way.
enum AnswerField : int32_t { kAnswerTier, kAnswerWhich, kAnswerWay };

/// One line step, rolled from the live Complexity: its leap range, octave-jump chances and slide chance (spec §8.1).
LineStep rollLineStep(const LoopSpec& loop, DrawKey key, int32_t complexity) {
	LineStep out;
	// A weighted draw over the degrees within the leap range. With none in range, the step stays on the root.
	const int32_t leap = loop.leap.at(complexity);
	const int32_t degree = drawWeightedIf(withField(key, kDegree), loop.degrees, [leap](const DegreeWeight& d) {
		return d.degree <= leap && -d.degree <= leap;
	});
	if (degree >= 0) {
		out.degree = loop.degrees[degree].degree;
	}
	if (drawChance(withField(key, kOctaveUp), loop.octaveUp.at(complexity))) {
		out.octave = 1;
	}
	else if (drawChance(withField(key, kOctaveDown), loop.octaveDown.at(complexity))) {
		out.octave = -1;
	}
	out.accent = drawChance(withField(key, kAccent), loop.accent);
	out.slide = drawChance(withField(key, kSlide), loop.slide.at(complexity));
	return out;
}

} // namespace

void Generator::rollLine(int32_t partIndex, int32_t complexity) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes = 0;
	for (int32_t i = 0; i < kMaxPatternSteps; i++) {
		part.line[i] = rollLineStep(spec.loop,
		                            lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex],
		                                    Purpose::LineRoll, part.lanes[0], 0, i, 0),
		                            complexity);
	}
}

void Generator::mutateLine(int32_t partIndex, int32_t motion, int32_t complexity) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes++;
	// The genre's chance at Motion 25, scaled by the mutation curve: never at Motion 0 (spec §3.5).
	const int32_t chance = scaleChance(spec.loop.mutation, curves::loopMutation, motion);
	const Pattern& pattern = spec.pool[part.lanes[0].entry];
	for (int32_t i = 0; i < pattern.length; i++) {
		if (pattern.importance[i] == kAnchor) {
			continue; // anchors never move
		}
		DrawKey key = lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex], Purpose::LineMutation,
		                      part.lanes[0], part.passes, i, kMutate);
		if (drawChance(key, chance)) {
			part.line[i] = rollLineStep(spec.loop, key, complexity); // drawn the way a fresh pick rolls it
		}
	}
}

namespace {

/// Bars from one Answer to the next: N at Motion 25 divided by the Answers curve's multiple, to the nearest bar and at
/// least 1. 0 at Motion 0, which never answers (spec §3.5).
int32_t answerPeriod(int32_t bars, int32_t motion) {
	const int32_t multiple = motionMultiple(curves::answers, motion);
	if (multiple == 0) {
		return 0;
	}
	const int32_t period = (bars * kCurveOne + multiple / 2) / multiple;
	return period > 0 ? period : 1;
}

} // namespace

int32_t Generator::heldStep(int32_t partIndex, int32_t step) const {
	return partFrozen(partIndex) ? parts_[partIndex].frozenPhraseStart + step % genre_->phraseSteps() : step;
}

void Generator::thaw(int32_t partIndex) {
	PartState& part = parts_[partIndex];
	for (PatternState& state : part.lanes) {
		if (state.hasWaiting) {
			// It jumps to where its counters are now: that pick's pattern, and a Loop's line rolled fresh with it.
			state.pick = state.waiting;
			state.entry = -1;
			state.hasWaiting = false;
			// The bar's Answer was drawn from the old pick. An Answer starts only on a bar line, so the rest of the bar
			// plays the new line's call.
			part.answer = AnswerKind::None;
		}
	}
}

void Generator::updateAnswer(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.answer = AnswerKind::None;
	const std::span<const Answer> answers = spec.loop.answers;
	const int32_t period = answerPeriod(spec.loop.answerBars, step.motion);
	// An automatic fill is a drum change known a bar or more ahead: the bar before it answers too (spec §5.2). A
	// frozen part answers there as well: the fill is the Arc's, acting on top like a section rule.
	const bool beforeFill = barBeforeAutoFill();
	if (answers.empty() || (period == 0 && !beforeFill)) {
		return;
	}
	// The last bar of every `period` bars from the pick answers.
	const PatternState& pick = part.lanes[0];
	const auto bars = static_cast<uint16_t>(heldStep(partIndex, step.index) / kStepsPerBar - pick.pick.bar);
	if (!beforeFill && (bars + 1) % period != 0) {
		return;
	}
	// One draw per Answer, here at its first step, from the live Complexity: keyed on the take, the pick and the bars
	// since it, so a take you return to replays its Answers.
	const DrawKey key =
	    onPick(partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex], Purpose::Answer),
	           pick, bars, 0);
	// The band runs from the list's lowest tier to its highest, and Complexity maps onto it as onto a pattern's.
	TierBand band{kMaxTier, 0};
	for (const Answer& answer : answers) {
		band.low = answer.tier < band.low ? answer.tier : band.low;
		band.high = answer.tier > band.high ? answer.tier : band.high;
	}
	const int32_t tier = playingTier(answers, bandTier(band, step.complexity, key));
	part.answer = answers[drawInTier(withField(key, kAnswerWhich), answers, tier)].kind;
	part.answerWay = drawBelow(withField(key, kAnswerWay), 2) == 0 ? 1 : -1;
}

bool Generator::linePitch(int32_t partIndex, const Pattern& pattern, int32_t patternStep, LinePitch& out) const {
	const PartState& part = parts_[partIndex];
	const int32_t length = pattern.length;
	const AnswerKind answer = part.answer;
	const bool answering = answer != AnswerKind::None;

	// Reverse and rotate reorder pitch content only: the rhythm stays the pattern's. Reverse counts the pattern's
	// onsets before Density thinning: the k-th of n plays onset n - 1 - k's content.
	int32_t from = patternStep;
	if (answering && answer == AnswerKind::Reverse) {
		int32_t k = 0;
		int32_t n = 0;
		for (int32_t i = 0; i < length; i++) {
			if (pattern.importance[i] > 0) {
				k += i < patternStep ? 1 : 0;
				n++;
			}
		}
		for (int32_t i = 0, onset = 0; i < length; i++) {
			if (pattern.importance[i] > 0 && onset++ == n - 1 - k) {
				from = i;
				break;
			}
		}
	}
	else if (answering && answer == AnswerKind::Rotate) {
		from = (patternStep - kStepsPerBeat + length) % length;
	}
	const LineStep& source = part.line[from];
	out = {source.degree + kDegreesPerScale * source.octave, source.accent, source.slide};
	if (!answering) {
		return true;
	}

	const bool anchor = pattern.importance[patternStep] == kAnchor;
	switch (answer) {
	case AnswerKind::Sparse:
		return anchor || patternStep % 2 == 0;
	case AnswerKind::AccentsOnly:
		return anchor || out.accent || pattern.level[patternStep] == VelocityLevel::Accent;
	case AnswerKind::Octave:
		out.steps += part.answerWay * kDegreesPerScale;
		break;
	case AnswerKind::Fifth:
		out.steps += part.answerWay * 4;
		break;
	case AnswerKind::Third:
		out.steps += part.answerWay * 2;
		break;
	case AnswerKind::Mirror: {
		// About the middle of the line's own range: the steps its pattern has onsets on.
		int32_t low = INT32_MAX;
		int32_t high = INT32_MIN;
		for (int32_t i = 0; i < length; i++) {
			if (pattern.importance[i] > 0) {
				const int32_t steps = part.line[i].degree + kDegreesPerScale * part.line[i].octave;
				low = steps < low ? steps : low;
				high = steps > high ? steps : high;
			}
		}
		out.steps = low + high - out.steps;
		break;
	}
	case AnswerKind::None:
	case AnswerKind::Reverse:
	case AnswerKind::Rotate:
		break;
	}
	return true;
}

uint8_t Generator::resolvePitch(const PartSpec& spec, int32_t steps) const {
	// Flavour step and Answers → snap → Register fold → absolute note (spec §7.5). In a narrow Register an octave
	// Answer moves only the notes whose pitch class fits twice: the fold takes the rest back to where they were.
	steps = snap(kHomeChordRoot + steps, spec.degreeSet, recipe_.scale, kHomeChordRoot);
	return noteFor(steps, key(), spec.registerOrDefault());
}

void Generator::decideLoop(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	PatternState& state = part.lanes[0];
	if (spec.pool.empty()) {
		return;
	}
	switch (updatePick(state, spec.pool, spec.band, static_cast<uint8_t>(partIndex), Lane::None, step)) {
	case PickChange::NewPick:
		rollLine(partIndex, step.complexity); // a new pick, a Complexity tier change included, rolls the line fresh
		break;
	case PickChange::Wrap:
		if (partFrozen(partIndex)) {
			part.passes++; // a frozen line holds, and its passes count on (spec §6.7)
		}
		else {
			mutateLine(partIndex, step.motion, step.complexity); // the end of a pass
		}
		break;
	case PickChange::None:
		break;
	}
	if (step.index % kStepsPerBar == 0) {
		updateAnswer(partIndex, step);
	}
	const Pattern& pattern = spec.pool[state.entry];
	int32_t patternStep = step.index % pattern.length;
	LinePitch pitch;
	if (!planned(partIndex, -1, step.index, step.density) || keptOut(step, partIndex, Lane::None)
	    || !linePitch(partIndex, pattern, patternStep, pitch)) {
		return;
	}
	// A slid note runs into the next step, so a LEGATO voice glides to it; a note after a gap starts fresh (§8.6).
	VelocityLevel level = pitch.accent ? VelocityLevel::Accent : pattern.level[patternStep];
	int32_t length = pitch.slide ? kTicksPerStep + spec.loop.slideOverlapTicks : spec.loop.gateTicks;
	express(partIndex, spec.expression, state, patternStep,
	        {Lane::None, resolvePitch(spec, pitch.steps), level, length, part.slid}, step);
	part.slid = pitch.slide;
}

// ---- Note bookkeeping (spec §2.7, §12.5) ----

void Generator::noteOn(int32_t partIndex, const Note& note) {
	const uint8_t part = static_cast<uint8_t>(partIndex);
	const Lane lane = note.lane;
	const uint8_t pitch = note.pitch;
	const int32_t onTick = note.onTick;
	Event on = Event::noteOn(onTick, part, lane, pitch, note.velocity);
	on.ornament = note.ornament;
	on.muted = !audible(partIndex);

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
	auto pushNoteOff = [&](const Event& off) {
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
		// A full table: the part's oldest note gets a note-off before the new note-on, but never before its own
		// note-on. Only bad data gets here, so it is counted. A roster past the pool (more than one Drums part) can
		// leave a part no slots at all: its notes don't play.
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
	*slot = SoundingNote{onTick, note.offTick, note.baseVelocity, lane, pitch, true};
}

bool Generator::push(const Event& event) {
	PushResult result = events_.push(event);
	if (result.dropped) {
		dropped_++;
		if (result.stored) {
			forgetUnplayed(result.droppedEvent);
		}
	}
	return result.stored;
}

void Generator::forgetUnplayed(const Event& noteOn) {
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
