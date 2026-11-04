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

// Generative mode POC: the Generator core.
//
// How a step is made:
//  - Step n is decided at swung tick n*24 - 12 (ADR 0003). Deciding reads the Recipe as it is then and writes the
//    step's note-ons (each with its gate length) into events_. Lean places each one within ±12 ticks of the grid.
//  - Drum lanes play a step when its importance (plus seeded jitter) reaches the Density threshold (#24).
//  - Bass plays a 16-step Loop (#12) held in loop_. Density rests its weaker steps, and at every phrase line Motion
//    re-rolls a few of its steps. Steps are scale degrees, resolved to MIDI through the Recipe key and scale and
//    folded into the Register (#21).
//  - Every random choice is a hash of (seed, part, take, purpose, position) (#17), never a running stream.
//  - When a note-on plays it enters sounding_, which holds its note-off tick. Offs are emitted from there, so a
//    retrigger can end the old note without leaving a stale off behind.
//
// Play order within one tick: note-offs, then note-ons by role priority (Kick, Clap, ClosedHat, OpenHat, Bass).
// Across ticks (when catching up) events go out in tick order.

#include "generative/core/generator.h"
#include "generative/core/genre.h"

namespace deluge::generative {

namespace {

// Hash purposes. Each draw names one, so steering one choice never reshuffles another.
enum Purpose : uint32_t {
	kPurposeDrumJitter = 1,
	kPurposeDrumVelocity = 2,
	kPurposePhraseEndDrop = 3,
	kPurposeDrumVariation = 4,
	kPurposeLoopStep = 5,
	kPurposeLoopMutateCount = 6,
	kPurposeLoopMutatePick = 7,
	kPurposeLoopJitter = 8,
	kPurposeLoopVelocity = 9,
};

// Sub-draws when rolling one Loop step.
enum LoopRoll : uint32_t {
	kRollRest = 0,
	kRollAccent = 1,
	kRollTie = 2,
	kRollSlide = 3,
	kRollDegree = 4,
	kRollLength = 5,
};

constexpr uint8_t kLoopRest = 1 << 0;
constexpr uint8_t kLoopAccent = 1 << 1;
constexpr uint8_t kLoopTie = 1 << 2;
constexpr uint8_t kLoopSlide = 1 << 3;

constexpr int32_t kNumScales = 3;
constexpr int8_t kScaleIntervals[kNumScales][7] = {
    {0, 2, 3, 5, 7, 8, 10}, // Minor (Aeolian)
    {0, 1, 3, 5, 7, 8, 10}, // Phrygian
    {0, 2, 3, 5, 7, 9, 10}, // Dorian
};

// A 32-bit integer finaliser (two multiply-xorshift rounds; constants from Chris Wellons' public-domain
// "hash prospector" search). Good avalanche, no division, a handful of cycles on ARM.
constexpr uint32_t mix32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/// A uniform integer in [0, n) from a 32-bit hash, by multiply-high (no division).
constexpr int32_t pick(uint32_t h, int32_t n) {
	return static_cast<int32_t>((static_cast<uint64_t>(h) * static_cast<uint32_t>(n)) >> 32);
}

/// True with `percent` in 100 chance.
constexpr bool chance(uint32_t h, int32_t percent) {
	return pick(h, 100) < percent;
}

/// a at macro 0, b at macro kMacroMax, linear between, rounded. 1311/65536 ≈ 1/50, so no division.
constexpr int32_t macroLerp(int32_t a, int32_t b, int32_t macro) {
	static_assert(kMacroMax == 50, "macroLerp's reciprocal assumes a 0-50 macro");
	return a + (((b - a) * macro * 1311 + 32768) >> 16);
}

constexpr int32_t clampInt(int32_t v, int32_t lo, int32_t hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

constexpr int32_t clampVelocity(int32_t v) {
	return clampInt(v, 1, 127);
}

/// Lean for a 16th: even steps are on-beat 16ths, odd ones off-beat. Clamped to the ADR 0003 range.
constexpr int32_t leanFor(int32_t stepInBar, int8_t onBeat, int8_t offBeat) {
	return clampInt((stepInBar & 1) ? offBeat : onBeat, -kMaxLean, kMaxLean);
}

/// Play-order rank within a tick: drum lanes in Lane order, then Bass.
constexpr int32_t priority(PartId part, uint8_t lane) {
	return part == PartId::Drums ? lane : kNumLanes + static_cast<int32_t>(part) - 1;
}

/// A seeded offset in [-range, +range].
constexpr int32_t jitter(uint32_t h, int32_t range) {
	return range > 0 ? pick(h, 2 * range + 1) - range : 0;
}

Recipe sanitised(const Recipe& r) {
	Recipe s = r;
	s.key = static_cast<uint8_t>(s.key % 12);
	if (static_cast<uint8_t>(s.scale) >= kNumScales) {
		s.scale = Scale::Minor;
	}
	s.energy = s.energy > kMacroMax ? kMacroMax : s.energy;
	s.density = s.density > kMacroMax ? kMacroMax : s.density;
	s.complexity = s.complexity > kMacroMax ? kMacroMax : s.complexity;
	s.motion = s.motion > kMacroMax ? kMacroMax : s.motion;
	return s;
}

/// The instance of the key's pitch class nearest the anchor (the lower one on a tie).
int16_t tonicNear(int32_t anchor, int32_t key) {
	int32_t below = anchor - key;
	while (below >= 12) {
		below -= 12;
	}
	while (below < 0) {
		below += 12;
	}
	int32_t tonic = anchor - below; // At or below the anchor.
	if (below > 6) {
		tonic += 12;
	}
	return static_cast<int16_t>(tonic);
}

} // namespace

Generator::Generator() {
	reset();
}

uint32_t Generator::draw(PartId part, uint32_t purpose, uint32_t posA, uint32_t posB) const {
	auto p = static_cast<uint32_t>(part);
	uint32_t h = mix32(seed_ ^ 0x9e3779b9U);
	h = mix32(h ^ (p | (static_cast<uint32_t>(take_[p]) << 8) | (purpose << 16)));
	h = mix32(h ^ posA);
	return mix32(h ^ (posB * 0x85ebca6bU));
}

void Generator::reset() {
	genre_ = &kGenreTechno;
	nextStepToDecide_ = 0;
	currentStep_ = -1;
	numEvents_ = 0;
	for (auto& t : take_) {
		t = 0;
	}
	nextDue_ = -kLookaheadTicks;
	lastNow_ = 0;
	stepInBar_ = 0;
	barInPhrase_ = 0;
	phrase_ = 0;
	drumEpoch_ = 0;
	bassTiedStep_ = -1;
	droppedEvents_ = 0;
	for (auto& s : sounding_) {
		s = Sounding{};
	}
	tonic_ = tonicNear(genre_->bass.registerAnchor, recipe_.key);
	for (int32_t i = 0; i < kLoopSteps; i++) {
		rollLoopStep(i, 0);
	}
}

void Generator::start(const Recipe& recipe, uint32_t seed) {
	recipe_ = sanitised(recipe);
	seed_ = seed;
	reset();
}

void Generator::setRecipe(const Recipe& recipe) {
	recipe_ = sanitised(recipe);
	tonic_ = tonicNear(genre_->bass.registerAnchor, recipe_.key);
}

// ---- Deciding ------------------------------------------------------------------------------------------------

void Generator::rollLoopStep(int32_t index, uint32_t generation) {
	const LoopTable& t = genre_->bass;
	auto roll = [&](uint32_t what) {
		return draw(PartId::Bass, kPurposeLoopStep, static_cast<uint32_t>(index) | (what << 8), generation);
	};
	LoopStep& s = loop_[index];
	bool home = index == 0 && t.tonicOnFirstStep;
	s.flags = 0;
	if (!home && chance(roll(kRollRest), t.restChance)) {
		s.flags |= kLoopRest;
	}
	if (chance(roll(kRollAccent), t.accentChance)) {
		s.flags |= kLoopAccent;
	}
	if (chance(roll(kRollTie), t.tieChance)) {
		s.flags |= kLoopTie;
	}
	if (chance(roll(kRollSlide), t.slideChance)) {
		s.flags |= kLoopSlide;
	}

	s.degree = 0;
	s.octave = 0;
	if (!home && t.numDegrees > 0) {
		int32_t total = 0;
		for (int32_t i = 0; i < t.numDegrees; i++) {
			total += t.degrees[i].weight;
		}
		int32_t r = pick(roll(kRollDegree), total);
		for (int32_t i = 0; i < t.numDegrees; i++) {
			r -= t.degrees[i].weight;
			if (r < 0) {
				s.degree = static_cast<int8_t>(t.degrees[i].degree % 7);
				s.octave = t.degrees[i].octave;
				break;
			}
		}
	}

	s.lengthTicks = kTicksPerStep / 2;
	if (t.numLengths > 0) {
		int32_t total = 0;
		for (int32_t i = 0; i < t.numLengths; i++) {
			total += t.lengths[i].weight;
		}
		int32_t r = pick(roll(kRollLength), total);
		for (int32_t i = 0; i < t.numLengths; i++) {
			r -= t.lengths[i].weight;
			if (r < 0) {
				s.lengthTicks = t.lengths[i].ticks;
				break;
			}
		}
	}
	s.generation = generation;
}

void Generator::onPhraseLine(uint32_t phrase) {
	int32_t motion = recipe_.motion;

	// Drums: the jitter pattern moves on to this phrase's with a Motion-scaled chance, else the old one repeats.
	if (chance(draw(PartId::Drums, kPurposeDrumVariation, 0, phrase),
	           macroLerp(0, genre_->drumVariationAtMaxMotion, motion))) {
		drumEpoch_ = phrase;
	}

	// Bass: re-roll a Motion-scaled number of Loop steps, chosen by this phrase's draws. Repeat picks are allowed,
	// so the count is an upper bound and the line keeps most of itself.
	int32_t base = macroLerp(0, genre_->bass.mutateStepsAtMaxMotion, motion);
	if (base <= 0) {
		return;
	}
	int32_t count = base + pick(draw(PartId::Bass, kPurposeLoopMutateCount, 0, phrase), 3) - 1;
	for (int32_t j = 0; j < count; j++) {
		int32_t index = pick(draw(PartId::Bass, kPurposeLoopMutatePick, static_cast<uint32_t>(j), phrase), kLoopSteps);
		rollLoopStep(index, phrase);
	}
}

void Generator::push(int32_t tick, PartId part, uint8_t lane, int16_t note, int32_t velocity, int32_t step,
                     int32_t length) {
	if (numEvents_ >= kMaxEvents) {
		// Unreachable by construction (see the static_assert on kMaxEvents). Drop rather than overrun.
		droppedEvents_++;
		return;
	}
	events_[numEvents_] = Event{tick, part, lane, note, static_cast<uint8_t>(clampVelocity(velocity))};
	eventStep_[numEvents_] = step;
	eventLength_[numEvents_] = length < 1 ? 1 : length;
	numEvents_++;
}

void Generator::decideDrumLane(int32_t lane, int32_t step, int32_t stepInBar, int32_t threshold) {
	const DrumLaneTable& t = genre_->lanes[lane];
	if (recipe_.density < t.densityGate) {
		return;
	}
	int32_t importance = t.importance[stepInBar];
	if (importance == 0) {
		return;
	}
	auto laneKey = static_cast<uint32_t>(lane);

	// Phrase-end drop: rest for the last N beats of the phrase's last bar.
	if (t.phraseEndDropBeatsMax > 0 && barInPhrase_ + 1 == genre_->phraseBars) {
		int32_t span = t.phraseEndDropBeatsMax - t.phraseEndDropBeatsMin + 1;
		int32_t beats = t.phraseEndDropBeatsMin
		                + (span > 1 ? pick(draw(PartId::Drums, kPurposePhraseEndDrop, laneKey, phrase_), span) : 0);
		if (stepInBar >= kStepsPerBar - beats * 4) {
			return;
		}
	}

	if (importance != kImportanceAnchor) {
		// Jitter repeats every two bars, and changes only when Motion moves the drum epoch on.
		uint32_t pos =
		    laneKey | (static_cast<uint32_t>(stepInBar) << 8) | (static_cast<uint32_t>(barInPhrase_ & 1) << 16);
		importance += jitter(draw(PartId::Drums, kPurposeDrumJitter, pos, drumEpoch_), genre_->importanceJitter);
		if (importance < threshold) {
			return;
		}
	}

	int32_t energy = recipe_.energy;
	int32_t velocity = macroLerp(t.velocityLow, t.velocityHigh, energy);
	velocity += (t.accent[stepInBar] * macroLerp(t.accentLow, t.accentHigh, energy)) >> 6;
	uint32_t velPos = laneKey | (static_cast<uint32_t>(stepInBar) << 8) | (static_cast<uint32_t>(barInPhrase_) << 16);
	velocity += jitter(draw(PartId::Drums, kPurposeDrumVelocity, velPos, 0), t.velocityJitter);

	int32_t lean = leanFor(stepInBar, t.leanOnBeat, t.leanOffBeat);
	if (step == 0 && lean < 0) {
		lean = 0; // No push on a Section's first step (ADR 0003). The piece's start is the POC's only one.
	}
	push(step * kTicksPerStep + lean, PartId::Drums, static_cast<uint8_t>(lane), 0, velocity, step, t.gateTicks);
}

void Generator::decideBass(int32_t step, int32_t stepInBar, int32_t threshold) {
	const LoopTable& t = genre_->bass;
	if (step == bassTiedStep_) {
		return; // The previous note holds through this step.
	}
	const LoopStep& s = loop_[stepInBar];
	if (s.flags & kLoopRest) {
		return;
	}
	int32_t importance = t.importance[stepInBar];
	if (importance == 0) {
		return;
	}
	if (importance != kImportanceAnchor) {
		uint32_t pos = static_cast<uint32_t>(stepInBar) | (static_cast<uint32_t>(barInPhrase_ & 1) << 8);
		importance += jitter(draw(PartId::Bass, kPurposeLoopJitter, pos, s.generation), genre_->importanceJitter);
		if (importance < threshold) {
			return;
		}
	}

	// Pitch: scale degree -> semitones from the tonic -> folded into the Register.
	int32_t note = tonic_ + kScaleIntervals[static_cast<uint8_t>(recipe_.scale)][s.degree] + 12 * s.octave;
	int32_t lo = t.registerAnchor + t.spanLow;
	int32_t hi = t.registerAnchor + t.spanHigh;
	while (note < lo) {
		note += 12;
	}
	while (note > hi) {
		note -= 12;
	}

	// Timing and length. A plain note ends minGapTicks before the next step's note would start; a slid one
	// overlaps it so a LEGATO voice glides; a tied one holds through the next step, which then rests.
	int32_t lean = leanFor(stepInBar, t.leanOnBeat, t.leanOffBeat);
	if (step == 0 && lean < 0) {
		lean = 0;
	}
	int32_t tied = (s.flags & kLoopTie) ? 1 : 0;
	int32_t nextLean = leanFor(stepInBar + 1 + tied, t.leanOnBeat, t.leanOffBeat);
	int32_t toNext = (1 + tied) * kTicksPerStep + nextLean - lean;
	int32_t length;
	if (s.flags & kLoopSlide) {
		length = toNext + t.slideOverlapTicks;
	}
	else {
		length = s.lengthTicks + tied * kTicksPerStep + macroLerp(0, t.lengthEnergyBonus, recipe_.energy);
		if (length > toNext - t.minGapTicks) {
			length = toNext - t.minGapTicks;
		}
	}
	if (tied) {
		bassTiedStep_ = step + 1;
	}

	int32_t energy = recipe_.energy;
	int32_t velocity = macroLerp(t.velocityLow, t.velocityHigh, energy);
	if (s.flags & kLoopAccent) {
		velocity += macroLerp(t.accentLow, t.accentHigh, energy);
	}
	uint32_t velPos = static_cast<uint32_t>(stepInBar) | (static_cast<uint32_t>(barInPhrase_) << 8);
	velocity += jitter(draw(PartId::Bass, kPurposeLoopVelocity, velPos, 0), t.velocityJitter);

	push(step * kTicksPerStep + lean, PartId::Bass, 0, static_cast<int16_t>(note), velocity, step, length);
}

void Generator::decideStep() {
	int32_t step = nextStepToDecide_++;
	int32_t stepInBar = stepInBar_;
	if (stepInBar == 0 && barInPhrase_ == 0 && phrase_ > 0) {
		onPhraseLine(phrase_);
	}

	int32_t threshold = macroLerp(genre_->thresholdAtDensityMin, genre_->thresholdAtDensityMax, recipe_.density);
	for (int32_t lane = 0; lane < kNumLanes; lane++) {
		decideDrumLane(lane, step, stepInBar, threshold);
	}
	decideBass(step, stepInBar, threshold);

	if (++stepInBar_ >= kStepsPerBar) {
		stepInBar_ = 0;
		if (++barInPhrase_ >= genre_->phraseBars) {
			barInPhrase_ = 0;
			phrase_++;
		}
	}
}

// ---- Playing -------------------------------------------------------------------------------------------------

void Generator::startSounding(const Event& e, int32_t length, int32_t now, EventSink& sink) {
	// A retrigger ends the note it replaces first. Drums key on the lane, Bass on the pitch.
	int32_t freeSlot = -1;
	int32_t oldest = -1;
	for (int32_t i = 0; i < kMaxSounding; i++) {
		Sounding& s = sounding_[i];
		if (!s.active) {
			if (freeSlot < 0) {
				freeSlot = i;
			}
			continue;
		}
		bool same = s.part == e.part && (e.part == PartId::Drums ? s.lane == e.lane : s.note == e.note);
		if (same) {
			sink.noteOff(now, s.part, s.lane, s.note);
			s.active = false;
			if (freeSlot < 0 || freeSlot > i) {
				freeSlot = i;
			}
			continue;
		}
		if (oldest < 0 || s.offTick < sounding_[oldest].offTick) {
			oldest = i;
		}
	}
	if (freeSlot < 0) {
		// Unreachable with the POC roster. End the note due off soonest rather than leak a hanging note.
		Sounding& s = sounding_[oldest];
		sink.noteOff(now, s.part, s.lane, s.note);
		freeSlot = oldest;
	}
	sounding_[freeSlot] = Sounding{e.tick + length, e.note, e.part, e.lane, true};
	sink.noteOn(now, e.part, e.lane, e.note, e.velocity);
}

void Generator::emitDue(int32_t limit, int32_t now, EventSink& sink) {
	for (;;) {
		// Earliest due note-off: by tick, then priority, then pitch.
		int32_t off = -1;
		for (int32_t i = 0; i < kMaxSounding; i++) {
			const Sounding& s = sounding_[i];
			if (!s.active || s.offTick > limit) {
				continue;
			}
			if (off < 0) {
				off = i;
				continue;
			}
			const Sounding& b = sounding_[off];
			int32_t pa = priority(s.part, s.lane);
			int32_t pb = priority(b.part, b.lane);
			if (s.offTick < b.offTick || (s.offTick == b.offTick && (pa < pb || (pa == pb && s.note < b.note)))) {
				off = i;
			}
		}
		// Earliest due note-on: by tick, then priority, then step.
		int32_t on = -1;
		for (int32_t i = 0; i < numEvents_; i++) {
			const Event& e = events_[i];
			if (e.tick > limit) {
				continue;
			}
			if (on < 0) {
				on = i;
				continue;
			}
			const Event& b = events_[on];
			int32_t pa = priority(e.part, e.lane);
			int32_t pb = priority(b.part, b.lane);
			if (e.tick < b.tick || (e.tick == b.tick && (pa < pb || (pa == pb && eventStep_[i] < eventStep_[on])))) {
				on = i;
			}
		}

		if (off < 0 && on < 0) {
			return;
		}
		// Within one tick, offs go before ons.
		if (off >= 0 && (on < 0 || sounding_[off].offTick <= events_[on].tick)) {
			Sounding& s = sounding_[off];
			sink.noteOff(now, s.part, s.lane, s.note);
			s.active = false;
			continue;
		}

		Event e = events_[on];
		int32_t length = eventLength_[on];
		int32_t step = eventStep_[on];
		numEvents_--;
		events_[on] = events_[numEvents_];
		eventStep_[on] = eventStep_[numEvents_];
		eventLength_[on] = eventLength_[numEvents_];
		startSounding(e, length, now, sink);
		if (step > currentStep_) {
			currentStep_ = step;
		}
	}
}

int32_t Generator::computeNextDue() const {
	int32_t next = nextStepToDecide_ * kTicksPerStep - kLookaheadTicks;
	for (int32_t i = 0; i < numEvents_; i++) {
		if (events_[i].tick < next) {
			next = events_[i].tick;
		}
	}
	for (const Sounding& s : sounding_) {
		if (s.active && s.offTick < next) {
			next = s.offTick;
		}
	}
	return next;
}

void Generator::advanceCurrentStep(int32_t now) {
	// Steps with no notes still count as started once their grid line passes.
	while (currentStep_ + 1 < nextStepToDecide_ && (currentStep_ + 1) * kTicksPerStep <= now) {
		currentStep_++;
	}
}

int32_t Generator::tick(int32_t now, EventSink& sink) {
	lastNow_ = now;
	if (now < nextDue_) {
		advanceCurrentStep(now);
		return nextDue_ - now;
	}

	// Catch up in time order: before deciding a step, play everything due before its decision point. That keeps
	// the buffer to at most two steps of note-ons however far `now` jumps.
	for (;;) {
		int32_t decisionTick = nextStepToDecide_ * kTicksPerStep - kLookaheadTicks;
		if (decisionTick > now) {
			break;
		}
		emitDue(decisionTick - 1, now, sink);
		decideStep();
	}
	emitDue(now, now, sink);
	advanceCurrentStep(now);

	nextDue_ = computeNextDue();
	int32_t wait = nextDue_ - now;
	return wait < 1 ? 1 : wait;
}

void Generator::stop(EventSink& sink) {
	// Note-offs in play order: by priority, then pitch.
	for (;;) {
		int32_t best = -1;
		for (int32_t i = 0; i < kMaxSounding; i++) {
			const Sounding& s = sounding_[i];
			if (!s.active) {
				continue;
			}
			if (best < 0) {
				best = i;
				continue;
			}
			const Sounding& b = sounding_[best];
			int32_t pa = priority(s.part, s.lane);
			int32_t pb = priority(b.part, b.lane);
			if (pa < pb || (pa == pb && s.note < b.note)) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		Sounding& s = sounding_[best];
		sink.noteOff(lastNow_, s.part, s.lane, s.note);
		s.active = false;
	}
	numEvents_ = 0;
	bassTiedStep_ = -1;
	nextDue_ = computeNextDue();
}

} // namespace deluge::generative
