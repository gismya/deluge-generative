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
#include <algorithm>

namespace deluge::generative {

namespace {

constexpr int32_t clamp(int32_t value, int32_t low, int32_t high) {
	return value < low ? low : value > high ? high : value;
}

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

/// How many of the genre's parts the Generator plays: the roster, up to kMaxParts.
constexpr int32_t partCount(const Genre& genre) {
	return std::min<int32_t>(static_cast<int32_t>(genre.parts.size()), kMaxParts);
}

/// How many of a Kit part's lanes the Generator plays: its lanes, up to kMaxLanesPerPart.
int32_t laneCount(const PartSpec& part) {
	return std::min<int32_t>(static_cast<int32_t>(part.lanes.size()), kMaxLanesPerPart);
}

/// Whether a SetKey names a pitch class and a scale.
constexpr bool validKey(const Command& command) {
	return command.target < 12 && command.value >= 0 && command.value < kNumScales;
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
DrawKey onPick(DrawKey key, const PatternState& pick, uint16_t index, uint32_t step) {
	key.position[0] = pick.groove;
	key.position[1] = static_cast<uint32_t>(pick.phrase) << 16 | index;
	key.position[2] = static_cast<uint32_t>(static_cast<uint8_t>(pick.entry)) << 24 | (step & 0xFFFFFFU);
	return key;
}

} // namespace

void Generator::start(const Recipe& recipe, uint32_t seed, const Takes& takes, bool skipIntro) {
	recipe_ = recipe;
	seed_ = seed;
	takes_ = takes;
	genre_ = recipe.genre;
	nextStep_ = 0;
	arc_.start(*genre_, skipIntro ? SectionKind::Groove : SectionKind::Intro);
	// A stop cancels the pending list: every start is fresh (spec §2.6, §13.2).
	pending_.clear();
	for (PartState& part : parts_) {
		part = PartState{};
	}
	sounding_.clear();
	held_.clear();
	// A part's tables hold what the event buffer counts for its role, so a fuller one evicts (spec §2.7).
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		sounding_.setCapacity(p, maxSounding(genre_->parts[p].role));
		held_.setCapacity(p, maxSounding(genre_->parts[p].role));
	}
	events_.clear();
	dropped_ = 0;
	refused_ = 0;
	evicted_ = 0;
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
		if (!validKey(command)) {
			return false;
		}
		const Key before = key();
		recipe_.keyRoot = command.target;
		recipe_.scale = static_cast<Scale>(command.value);
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
		arc_.hold(command.value != 0);
		return true;
	}
	return false;
}

bool Generator::wait(Command command) {
	// An invalid command is dropped here, so it never replaces a valid one or gets logged.
	switch (command.kind) {
	case CommandKind::SetKey:
		if (!validKey(command)) {
			return true;
		}
		break;
	case CommandKind::SetTake:
		if (!validTake(command, *genre_)) {
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
		if (waiting != nullptr && waiting->target == command.target) {
			pending_.cancel(command);
			return true;
		}
		// A forced Build draws its length when forced, so it knows where it starts (spec §9.6).
		command.value = kind == SectionKind::Build ? arc_.nextBuildSteps(*genre_, seed_) : 0;
		break;
	}
	default:
		break;
	}
	return pending_.add(command);
}

void Generator::landPending(SteeringLog* log) {
	const int32_t n = nextStep_;
	pending_.land([&](const Command& command) {
		// Key changes and takes land on the next phrase line, which every Arc Drop is (spec §7.6, §9.3, §6.6).
		bool lands = command.kind == CommandKind::ForceSection
		                 ? arc_.forceLandsNext(static_cast<SectionKind>(command.target), command.value)
		                 : arc_.nextStepInPhrase() == 0;
		if (!lands) {
			return false;
		}
		if (apply(command) && log != nullptr) {
			log->append(n, command);
		}
		return true;
	});
}

Chord Generator::chord() const {
	return voiceChord(recipe_.scale, kHomeChordRoot,
	                  extensionLevel(recipe_.macros[Macro::Complexity], genre_->harmony.extensionFloor,
	                                 genre_->harmony.extensionCeiling));
}

int32_t Generator::effectiveMacro(Macro macro) const {
	const int32_t centre = recipe_.macros[macro];
	return clamp(centre + arc_.offset(*genre_, macro, centre), 0, kMacroFull);
}

void Generator::decideNextStep(SteeringQueue& queue, SteeringLog* log) {
	const int32_t n = nextStep_;
	events_.beginStep();

	// Steering lands here, on the next undecided step, and is logged where it lands (ADR 0005). Bar- and
	// phrase-quantised commands wait for their line in the pending list, and land before the Arc moves, so a forced
	// Section starts on this step.
	Command command;
	while (queue.pop(&command)) {
		if (isQuantised(command.kind)) {
			refused_ += wait(command) ? 0 : 1;
		}
		else if (apply(command) && log != nullptr) {
			log->append(n, command);
		}
	}
	landPending(log);

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
		step.repickLine = !arc_.sectionStartsHere();
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

	// Parts in the genre's roster order, which is role order unless the genre says otherwise (spec §5.5).
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		decidePart(p, step);
	}
	if (arc_.stepInBar() == 0) {
		lateEntries(step);
	}

	queueNoteOffsBefore(decisionTick(n + 1));
	nextStep_ = n + 1;
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
                             Lane lane, int32_t complexity) const {
	// Complexity maps linearly onto the band, and the remainder is a keyed chance of the next tier up. The draw holds
	// for the whole pick, so a slow turn moves lanes up one at a time and each wrap re-checks with the same draw.
	int32_t width = band.high > band.low ? band.high - band.low : 0;
	int32_t position = complexity * width;
	int32_t tier = band.low + position / kMacroFull;
	int32_t remainder = position % kMacroFull;
	if (remainder > 0) {
		DrawKey key = onPhrase(partKey(seed_, part, lane, takes_.take[part], Purpose::ComplexityTier), state.groove,
		                       state.phrase);
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

	// Within the tier, a weighted pick keyed on the pick and the tier, so it changes only when one of them does. Pools
	// are listed by tier (a genre data spec checks it), so the tier is one slice.
	int32_t first = 0;
	while (pool[first].tier != playing) {
		first++;
	}
	int32_t count = 0;
	while (first + count < static_cast<int32_t>(pool.size()) && pool[first + count].tier == playing) {
		count++;
	}
	DrawKey key = onPhrase(partKey(seed_, part, lane, takes_.take[part], Purpose::PatternPick), state.groove,
	                       state.phrase, static_cast<uint32_t>(playing));
	return first + drawWeighted(key, pool.subspan(first, count));
}

Generator::PickChange Generator::updatePick(PatternState& state, std::span<const Pattern> pool, TierBand band,
                                            uint8_t part, Lane lane, const Step& step) const {
	bool newPick = step.drop; // keyed on the Groove's index
	if (step.repickLine) {
		// Whether a lane re-picks leaves out the take, so every take changes at the same phrase lines.
		DrawKey key = onPhrase(partKey(seed_, part, lane, 0, Purpose::PatternRepick), step.groove, step.phrase);
		newPick = drawChance(key, scaleChance(genre_->repickChance, curves::patternRepick, step.motion));
	}
	if (newPick) {
		state.groove = step.groove;
		state.phrase = step.phrase;
	}
	// A take lands on a phrase line and picks what that take would have now: the same pick, keyed on the new take.
	bool fresh = newPick || state.entry < 0 || parts_[part].newTake;
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
	return clamp(rule.strength * kMacroFull + moved, 0, kFullStrength);
}

int32_t Generator::energyLayer(int32_t energy) const {
	int64_t layer = static_cast<int64_t>(energy - kMacroFull / 2) * 2 * kEnergyVelocityRange
	                * genre_->energyVelocityWeight / (static_cast<int64_t>(kMacroFull) * 2 * 1000);
	return static_cast<int32_t>(layer);
}

uint8_t Generator::velocity(int32_t baseVelocity, int32_t energy) const {
	return static_cast<uint8_t>(clamp(baseVelocity + energyLayer(energy), 1, 127));
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
			if (!gateOpen(p, note.lane) || silenced(step, p, note.lane)) {
				return; // still gated out or kept out by a section rule: it waits
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
		if (!lane.pool.empty()) {
			PatternState& pick = parts_[partIndex].lanes[l];
			updatePick(pick, lane.pool, lane.band, static_cast<uint8_t>(partIndex), lane.lane, step);
			const Pattern& pattern = lane.pool[pick.entry];
			note.patternStep = step.index % pattern.length;
			if (planned(partIndex, l, step.index, step.density)) {
				note = {true, pattern.level[note.patternStep], lane.gateTicks, note.patternStep};
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
	express(partIndex, spec.expression, parts_[partIndex].lanes[laneIndex], note.patternStep,
	        {spec.lane, 0, note.level, note.length}, step);
}

// ---- Section rules (spec §8.3, §9.5) ----

void Generator::sectionRules(Step& step) {
	const ArcSpec& rules = genre_->arc;
	const SectionKind kind = arc_.section();
	if (kind == SectionKind::Break) {
		step.keeps = &rules.breakKeeps;
	}
	if (kind == SectionKind::Build) {
		step.keeps = rules.buildKeeps == kSameAsBreakKeeps ? &rules.breakKeeps : &rules.buildKeeps;
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
		    kRollStartAtZero + (1000 - kRollStartAtZero) * clamp(step.complexity, 0, kMacroFull) / kMacroFull;
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

	// The Phrase accent: always on a Drop, and on other Groove phrase starts by a keyed chance that Motion scales
	// along the fill's curve. TODO(#103): always after a fill that covered the phrase's last step.
	if (rules.accentLane != Lane::None && kind == SectionKind::Groove && arc_.stepInPhrase() == 0) {
		DrawKey key = partKey(seed_, kArcPart, rules.accentLane, 0, Purpose::PhraseAccent);
		key = onPhrase(key, arc_.occurrence(), static_cast<uint16_t>(arc_.phraseInSection()));
		step.accent = arc_.dropStartsHere()
		              || drawChance(key, scaleChance(rules.accentChance, curves::phraseAccent, step.motion));
	}
}

bool Generator::silenced(const Step& step, int32_t part, Lane lane) const {
	if (step.gap || step.cut.has(part, lane)) {
		return true;
	}
	// FX lanes are the section rules' own, and the roll lane always plays in a Build.
	if (step.keeps == nullptr || isFxLane(lane)
	    || (lane != Lane::None && lane == genre_->arc.roll.lane && arc_.section() == SectionKind::Build)) {
		return false;
	}
	return !step.keeps->has(part, lane);
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
			key.position[0] = static_cast<uint32_t>(step.index);
			key.position[1] = UINT32_MAX; // apart from every Repeat key
		}
		base += drawBelow(key, 2 * expression.spread + 1) - expression.spread;
	}

	// Lean: the whole note moves by its 16th's offset, clamped to half a 16th either side. Never earlier than a
	// Section's first step, so Compose clips stay self-contained and downbeats land together (spec §8.5).
	int32_t lean = clamp(expression.lean[step.index % kStepsPerBeat], -kMaxLean, kMaxLean);
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
			auto graceVelocity = static_cast<uint8_t>(clamp(note.velocity * kFlamGraceVelocity / 1000, 1, 127));
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
	              static_cast<uint32_t>(lineStep) << 8 | static_cast<uint32_t>(field));
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
	int32_t degree = drawWeighted(withField(key, kDegree), loop.degrees);
	if (degree >= 0) {
		out.degree = loop.degrees[degree].degree;
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
		                                               Purpose::LineRoll, part.lanes[0], 0, i, 0));
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
		                      part.lanes[0], part.passes, i, kMutate);
		if (drawChance(key, chance)) {
			part.line[i] = rollLineStep(spec.loop, key);
		}
	}
}

uint8_t Generator::resolvePitch(const PartSpec& spec, const LineStep& lineStep) const {
	// Flavour step → Answers → snap → Register fold → absolute note (spec §7.5). A Loop step is an offset from the
	// chord root. Answers transform `steps` here, before the snap.
	int32_t steps = kHomeChordRoot + lineStep.degree + kDegreesPerScale * lineStep.octave;
	steps = snap(steps, spec.degreeSet, recipe_.scale, kHomeChordRoot);
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
	if (!planned(partIndex, -1, step.index, step.density) || silenced(step, partIndex, Lane::None)) {
		return;
	}
	const LineStep& lineStep = part.line[patternStep];
	VelocityLevel level = lineStep.accent ? VelocityLevel::Accent : pattern.level[patternStep];
	int32_t length = lineStep.slide ? kTicksPerStep + spec.loop.slideOverlapTicks : spec.loop.gateTicks;
	express(partIndex, spec.expression, state, patternStep, {Lane::None, resolvePitch(spec, lineStep), level, length},
	        step);
}

// ---- Note bookkeeping (spec §2.7, §12.5) ----

void Generator::noteOn(int32_t partIndex, const Note& note) {
	const uint8_t part = static_cast<uint8_t>(partIndex);
	const Lane lane = note.lane;
	const uint8_t pitch = note.pitch;
	const int32_t onTick = note.onTick;
	Event on = Event::noteOn(onTick, part, lane, pitch, note.velocity);
	on.ornament = note.ornament;

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
		// note-on. Only bad data gets here, so it is counted.
		evicted_++;
		SoundingNote* oldest = sounding_.oldest(partIndex);
		oldest->offTick = onTick > oldest->onTick ? onTick : oldest->onTick + 1;
		if (pushNoteOff(oldest->noteOff(part))) {
			oldest->used = false;
			slot = onDropped ? nullptr : oldest;
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
