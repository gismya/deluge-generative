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

namespace deluge::generative {

namespace {

constexpr int32_t clamp(int32_t value, int32_t low, int32_t high) {
	return value < low ? low : value > high ? high : value;
}

/// Floor division and modulo for possibly negative scale steps.
constexpr int32_t floorDiv(int32_t a, int32_t b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

} // namespace

void Generator::start(const Recipe& recipe, uint32_t seed, const Takes& takes) {
	recipe_ = recipe;
	seed_ = seed;
	takes_ = takes;
	genre_ = recipe.genre;
	nextStep_ = 0;
	arc_.start(*genre_, seed_);
	for (PartState& part : parts_) {
		part = PartState{};
	}
	sounding_.clear();
	events_.clear();
	dropped_ = 0;
	evicted_ = 0;
}

bool Generator::apply(const Command& command) {
	switch (command.kind) {
	case CommandKind::SetMacro:
		if (command.target < kNumMacros) {
			recipe_.macros.set(static_cast<Macro>(command.target), command.value);
		}
		return true;
	case CommandKind::SetTake:
		// A new take or a step back starts at the next phrase line (spec §6.6).
		if (command.target < kMaxParts && command.value >= 0 && command.value < kMaxTakes) {
			parts_[command.target].pendingTake = static_cast<int8_t>(command.value);
		}
		return false;
	}
	return true;
}

void Generator::landTakes(int32_t step, SteeringLog* log) {
	if (arc_.stepInPhrase() != 0) {
		return;
	}
	for (int32_t p = 0; p < kMaxParts; p++) {
		PartState& part = parts_[p];
		if (part.pendingTake < 0) {
			continue;
		}
		takes_.take[p] = static_cast<uint8_t>(part.pendingTake);
		if (log != nullptr) {
			log->append(step, {CommandKind::SetTake, static_cast<uint8_t>(p), part.pendingTake});
		}
		part.pendingTake = -1;
	}
}

int32_t Generator::effectiveMacro(Macro macro) const {
	return clamp(recipe_.macros[macro] + arc_.offset(*genre_, macro), 0, kMacroFull);
}

void Generator::decideNextStep(SteeringQueue& queue, SteeringLog* log) {
	const int32_t n = nextStep_;
	events_.beginStep();

	// Steering lands here, on the next undecided step, and is logged where it lands (ADR 0005).
	Command command;
	while (queue.pop(&command)) {
		if (apply(command) && log != nullptr) {
			log->append(n, command);
		}
	}

	Step step{};
	step.index = n;
	step.tick = stepTick(n);
	if (arc_.enterStep(*genre_, seed_, n)) {
		push(Event::sectionStart(step.tick, arc_.section(), arc_.occurrence()));
	}
	if (n == 0) {
		push(Event::keyScale(step.tick, recipe_.keyRoot, recipe_.scale));
	}
	landTakes(n, log);
	step.energy = effectiveMacro(Macro::Energy);
	step.density = effectiveMacro(Macro::Density);
	step.complexity = recipe_.macros[Macro::Complexity];
	step.motion = recipe_.macros[Macro::Motion];
	// The Intro and the first Groove share the first pick; Breaks and Builds keep what was playing.
	if (arc_.section() == SectionKind::Groove && arc_.stepInPhrase() == 0) {
		step.drop = arc_.sectionStartsHere() && arc_.occurrence() > 0;
		step.repickLine = !arc_.sectionStartsHere();
		step.groove = arc_.occurrence();
		step.phrase = static_cast<uint16_t>(arc_.phraseInSection());
	}

	// Parts in the genre's roster order, which is role order unless the genre says otherwise (spec §5.5).
	for (int32_t p = 0; p < static_cast<int32_t>(genre_->parts.size()) && p < kMaxParts; p++) {
		decidePart(p, step);
	}

	queueNoteOffsBefore(decisionTick(n + 1));
	nextStep_ = n + 1;
}

void Generator::decidePart(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	if (!spec.lanes.empty()) {
		for (int32_t l = 0; l < static_cast<int32_t>(spec.lanes.size()) && l < kMaxLanesPerPart; l++) {
			decideLane(partIndex, l, step);
		}
		return;
	}
	if (spec.flavour == Flavour::Loop) {
		decideLoop(partIndex, step);
	}
}

namespace {

DrawKey laneKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose) {
	DrawKey key;
	key.seed = seed;
	key.part = part;
	key.lane = static_cast<uint8_t>(lane);
	key.take = take;
	key.purpose = purpose;
	return key;
}

} // namespace

int32_t Generator::pickEntry(const PatternState& state, std::span<const Pattern> pool, TierBand band, uint8_t part,
                             Lane lane, int32_t complexity) const {
	// Complexity maps linearly onto the band, and the remainder is a keyed chance of the next tier up. The draw holds
	// for the whole pick, so a slow turn moves lanes up one at a time and each wrap re-checks with the same draw.
	int32_t width = band.high > band.low ? band.high - band.low : 0;
	int32_t position = complexity * width;
	int32_t tier = band.low + position / kMacroFull;
	int32_t remainder = position % kMacroFull;
	if (remainder > 0) {
		DrawKey key = laneKey(seed_, part, lane, state.take, Purpose::ComplexityTier);
		key.position[0] = state.groove;
		key.position[1] = state.phrase;
		tier += drawBelow(key, kMacroFull) < remainder ? 1 : 0;
	}

	// An empty tier falls back to the nearest lower one (or, failing that, the lowest the pool has).
	int32_t playing = -1;
	int32_t lowest = kMaxTier + 1;
	for (const Pattern& pattern : pool) {
		if (pattern.tier <= tier && pattern.tier > playing) {
			playing = pattern.tier;
		}
		lowest = pattern.tier < lowest ? pattern.tier : lowest;
	}
	if (playing < 0) {
		playing = lowest;
	}

	// Within the tier, a weighted pick keyed on the pick and the tier, so it changes only when one of them does.
	int32_t total = 0;
	for (const Pattern& pattern : pool) {
		total += pattern.tier == playing ? pattern.weight : 0;
	}
	DrawKey key = laneKey(seed_, part, lane, state.take, Purpose::PatternPick);
	key.position[0] = state.groove;
	key.position[1] = state.phrase;
	key.position[2] = static_cast<uint32_t>(playing);
	int32_t draw = drawBelow(key, total);
	for (int32_t i = 0; i < static_cast<int32_t>(pool.size()); i++) {
		if (pool[i].tier != playing) {
			continue;
		}
		if (draw < pool[i].weight) {
			return i;
		}
		draw -= pool[i].weight;
	}
	return 0;
}

Generator::PickChange Generator::updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band,
                                            uint8_t part, Lane lane, const Step& step) const {
	bool newPick = step.drop; // keyed on the Groove's index
	if (step.repickLine) {
		// Whether a lane re-picks leaves out the take, so every take changes at the same phrase lines.
		DrawKey key = laneKey(seed_, part, lane, 0, Purpose::PatternRepick);
		key.position[0] = step.groove;
		key.position[1] = step.phrase;
		newPick = drawChance(key, scaleChance(genre_->repickChance, curves::patternRepick, step.motion));
	}
	if (newPick) {
		state.groove = step.groove;
		state.phrase = step.phrase;
	}
	// A take lands on a phrase line and picks what that take would have now: the same pick, keyed on the new take.
	bool fresh = newPick || state.entry < 0 || state.take != takes_.take[part];
	state.take = takes_.take[part];
	if (!fresh && step.index % pool[state.entry].length != 0) {
		return PickChange::None;
	}
	// The tier is checked each time the pattern wraps. If it hasn't changed, neither has the entry.
	int32_t entry = pickEntry(state, pool, band, part, lane, step.complexity);
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
	DrawKey key = laneKey(seed_, part, lane, state.take, Purpose::DensityJitter);
	key.position[0] = state.pick();
	key.position[1] = static_cast<uint32_t>(patternStep);
	int32_t jitter = drawBelow(key, 2 * genre_->densityJitter + 1) - genre_->densityJitter;
	return importance * 100 + jitter >= genre_->densityCurve.threshold(density);
}

uint8_t Generator::velocity(VelocityLevel level, int32_t energy) const {
	int64_t layer = static_cast<int64_t>(energy - kMacroFull / 2) * 2 * kEnergyVelocitySwing
	                * genre_->energyVelocityWeight / (static_cast<int64_t>(kMacroFull) * 2 * 1000);
	return static_cast<uint8_t>(
	    clamp(kLevelVelocity[static_cast<int32_t>(level)] + static_cast<int32_t>(layer), 1, 127));
}

void Generator::decideLane(int32_t partIndex, int32_t laneIndex, const Step& step) {
	const LaneSpec& spec = genre_->parts[partIndex].lanes[laneIndex];
	PatternState& state = parts_[partIndex].lanes[laneIndex];
	if (spec.pool.empty()) {
		return;
	}
	updatePick(state, spec.pool, spec.band, static_cast<uint8_t>(partIndex), spec.lane, step);
	const Pattern& pattern = spec.pool[state.entry];
	int32_t patternStep = step.index % pattern.length;
	if (!plays(pattern, patternStep, static_cast<uint8_t>(partIndex), spec.lane, state, step.density)) {
		return;
	}
	noteOn(partIndex, spec.lane, 0, velocity(pattern.level[patternStep], step.energy), step.tick,
	       step.tick + spec.gateTicks, false);
}

// ---- Loop lines (spec §5.2) ----

namespace {

DrawKey lineKey(uint32_t seed, uint8_t part, uint8_t take, Purpose purpose, uint32_t pick, uint32_t passes,
                int32_t lineStep, int32_t field) {
	DrawKey key;
	key.seed = seed;
	key.part = part;
	key.take = take;
	key.purpose = purpose;
	key.position[0] = pick;
	key.position[1] = passes;
	key.position[2] = static_cast<uint32_t>(lineStep) << 8 | static_cast<uint32_t>(field);
	return key;
}

/// Fields of one line step's draws.
enum LineField : int32_t { kDegree, kOctaveUp, kOctaveDown, kAccent, kSlide, kMutate };

/// `key` for another field of the same line step: the field is position[2]'s low byte.
DrawKey withField(DrawKey key, LineField field) {
	key.position[2] = (key.position[2] & ~0xFFU) | static_cast<uint32_t>(field);
	return key;
}

LineStep rollLineStep(const LoopSpec& loop, DrawKey key) {
	LineStep out;
	int32_t total = 0;
	for (const DegreeWeight& d : loop.degrees) {
		total += d.weight;
	}
	int32_t pick = total > 0 ? drawBelow(withField(key, kDegree), total) : 0;
	for (const DegreeWeight& d : loop.degrees) {
		if (pick < d.weight) {
			out.degree = d.degree;
			break;
		}
		pick -= d.weight;
	}
	if (drawChance(withField(key, kOctaveUp), loop.octaveUp)) {
		out.octave = 1;
	}
	else if (drawChance(withField(key, kOctaveDown), loop.octaveDown)) {
		out.octave = -1;
	}
	out.accent = drawChance(withField(key, kAccent), loop.accent);
	out.slide = drawChance(withField(key, kSlide), loop.slide);
	return out;
}

} // namespace

void Generator::rollLine(int32_t partIndex) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes = 0;
	for (int32_t i = 0; i < kMaxPatternSteps; i++) {
		part.line[i] = rollLineStep(spec.loop, lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex],
		                                               Purpose::LineRoll, part.lanes[0].pick(), 0, i, 0));
	}
}

void Generator::mutateLine(int32_t partIndex, int32_t motion) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes++;
	// Motion 0 never mutates. Linear in Motion until the Loop slice brings the mutation curve (spec §3.5).
	int32_t chance =
	    clamp(static_cast<int32_t>(static_cast<int64_t>(spec.loop.mutation) * motion / macroPoints(25)), 0, 1000);
	const Pattern& pattern = spec.pool[part.lanes[0].entry];
	for (int32_t i = 0; i < pattern.length; i++) {
		if (pattern.importance[i] == kAnchor) {
			continue; // anchors never move
		}
		DrawKey key = lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex], Purpose::LineMutation,
		                      part.lanes[0].pick(), part.passes, i, kMutate);
		if (drawChance(key, chance)) {
			part.line[i] = rollLineStep(spec.loop, key);
		}
	}
}

uint8_t Generator::resolvePitch(const PartSpec& spec, const LineStep& lineStep) const {
	Register reg = spec.registerOrDefault();
	// The tonic is the key root's instance nearest the anchor, the lower one on a 6-semitone tie (spec §7.3).
	int32_t below = reg.anchor - ((reg.anchor - recipe_.keyRoot) % 12 + 12) % 12;
	int32_t tonic = reg.anchor - below > 6 ? below + 12 : below;
	// Scale steps to semitones. The home chord's root is degree 0 (an empty Harmony slot, spec §7.4).
	int32_t steps = lineStep.degree + kDegreesPerScale * lineStep.octave;
	int32_t octave = floorDiv(steps, kDegreesPerScale);
	int32_t degree = steps - octave * kDegreesPerScale;
	int32_t note = tonic + 12 * octave + kScaleSemitones[static_cast<int32_t>(recipe_.scale)][degree];
	// Fold into the span by octaves; where a pitch class fits twice, the note's own octave picks (spec §7.3).
	while (note > tonic + reg.spanHigh) {
		note -= 12;
	}
	while (note < tonic + reg.spanLow) {
		note += 12;
	}
	return static_cast<uint8_t>(clamp(note, 0, 127));
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
		rollLine(partIndex); // a new pick, a Complexity tier change included, rolls the line fresh
		break;
	case PickChange::Wrap:
		mutateLine(partIndex, step.motion); // the end of a pass
		break;
	case PickChange::None:
		break;
	}
	const Pattern& pattern = spec.pool[state.entry];
	int32_t patternStep = step.index % pattern.length;
	if (!plays(pattern, patternStep, static_cast<uint8_t>(partIndex), Lane::None, state, step.density)) {
		return;
	}
	const LineStep& lineStep = part.line[patternStep];
	VelocityLevel level = lineStep.accent ? VelocityLevel::Accent : pattern.level[patternStep];
	int32_t length = lineStep.slide ? kTicksPerStep + spec.loop.slideOverlapTicks : spec.loop.gateTicks;
	noteOn(partIndex, Lane::None, resolvePitch(spec, lineStep), velocity(level, step.energy), step.tick,
	       step.tick + length, false);
}

// ---- Note bookkeeping (spec §2.7, §12.5) ----

void Generator::noteOn(int32_t partIndex, Lane lane, uint8_t pitch, uint8_t velocity, int32_t onTick, int32_t offTick,
                       bool ornament) {
	const uint8_t part = static_cast<uint8_t>(partIndex);
	Event on = Event::noteOn(onTick, part, lane, pitch, velocity);
	on.ornament = ornament;

	// A full table: the part's oldest note gets a note-off before the new note-on. Bad data, so it is counted.
	if (sounding_.freeSlot(partIndex) == nullptr) {
		evicted_++;
		SoundingNote* oldest = sounding_.oldest(partIndex);
		oldest->offTick = onTick;
		if (push(oldest->noteOff(part))) {
			oldest->used = false;
		}
	}

	PushResult result = events_.pushNoteOn(on, genre_->parts[partIndex].role);
	if (result.dropped) {
		dropped_++;
		if (!result.stored) {
			return; // nothing else changes for a note that won't play
		}
		forgetUnplayed(result.droppedEvent);
	}

	// The same key sounding again ends the earlier note no later than this one starts.
	sounding_.endKeyBy(on);
	for (int32_t i = 0; i < events_.size(); i++) {
		Event& queued = events_.at(i);
		if (queued.kind == EventKind::NoteOff && queued.sameKey(on) && queued.tick > onTick) {
			queued.tick = onTick;
		}
	}

	SoundingNote* slot = sounding_.freeSlot(partIndex);
	if (slot == nullptr) {
		// Only if the eviction's note-off couldn't be stored: the new note can't be tracked, so it doesn't play.
		for (int32_t i = events_.size() - 1; i >= 0; i--) {
			if (events_.at(i) == on) {
				events_.removeAt(i);
				break;
			}
		}
		dropped_++;
		return;
	}
	*slot = SoundingNote{onTick, offTick, lane, pitch, true};
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

void Generator::flush(int32_t now) {
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

	// What's left: note-offs for notes that started, which now end at `now`, and Section and key events that never
	// played, which go.
	for (int32_t i = 0; i < events_.size();) {
		Event& e = events_.at(i);
		if (e.kind == EventKind::NoteOff) {
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
