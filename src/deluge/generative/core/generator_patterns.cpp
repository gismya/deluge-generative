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

// The Generator's pattern parts: picks, planned onsets and interplay rules, gates, the Kit's lanes and expression
// (spec §5.1 to §5.6, §8.2).

#include "generative/core/generator.h"
#include "generative/core/motion_curve.h"
#include "generative/core/tiers.h"
#include <algorithm>

namespace deluge::generative {

GEN_STEP_CODE int32_t Generator::pickEntry(const PatternState& state, std::span<const Pattern> pool, TierBand band,
                                           uint8_t part, Lane lane, int32_t complexity, PickPurposes purposes) const {
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

GEN_STEP_CODE bool Generator::placePick(PatternState& state, uint8_t part, Lane lane, const Step& step) const {
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

GEN_STEP_CODE Generator::PickChange Generator::updatePick(PatternState& state, std::span<const Pattern> pool,
                                                          TierBand band, uint8_t part, Lane lane,
                                                          const Step& step) const {
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

GEN_STEP_CODE bool Generator::plays(const Pattern& pattern, int32_t patternStep, uint8_t part, Lane lane,
                                    const PatternState& state, int32_t density) const {
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
	return jitteredImportance(key, importance) >= genre_->densityCurve.threshold(density);
}

GEN_STEP_CODE int32_t Generator::jitteredImportance(DrawKey key, int32_t importance) const {
	return importance * 100 + drawBelow(key, 2 * genre_->densityJitter + 1) - genre_->densityJitter;
}

// ---- Planned onsets and interplay rules (spec §5.5) ----

namespace {

/// Where Respond keeps `target`'s count in Generator::busyHalves_: a lane by its bit, a part (a Kit part's lanes read
/// whole) by its own.
constexpr PartsAndLanes respondMask(const RuleTarget& target) {
	return target.lane != Lane::None ? PartsAndLanes{0, laneBit(target.lane)} : PartsAndLanes{partBit(target.part), 0};
}

/// The step `back` steps before `step`, wrapping inside the pattern pass `step` is in.
int32_t stepBack(int32_t step, int32_t back, int32_t length) {
	int32_t inPattern = step % length;
	return step - inPattern + (inPattern - back + length) % length;
}

} // namespace

GEN_STEP_CODE Generator::Source Generator::source(int32_t part, int32_t laneIndex) const {
	const PartSpec& spec = genre_->parts[part];
	if (laneIndex < 0) {
		return {spec.pool, spec.rules, Lane::None, &parts_[part].lanes[0]};
	}
	const LaneSpec& lane = spec.lanes[laneIndex];
	return {lane.pool, lane.rules, lane.lane, &parts_[part].lanes[laneIndex]};
}

GEN_STEP_CODE bool Generator::planned(int32_t part, int32_t laneIndex, int32_t step, int32_t density) const {
	Source from = source(part, laneIndex);
	if (from.pool.empty() || from.state->entry < 0) {
		return false;
	}
	const Pattern& pattern = from.pool[from.state->entry];
	int32_t patternStep = step % pattern.length;
	const auto partId = static_cast<uint8_t>(part);
	// `density` is the shared one: each source thins at its own biased Density, and its rules plan their targets at
	// theirs (spec §3.3).
	if (!plays(pattern, patternStep, partId, from.lane, *from.state, biasedOn(part, Macro::Density, density))) {
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
		if (drawBelow(key, kFullStrength) < effectiveStrength(rule, part)) {
			return false;
		}
	}
	return true;
}

GEN_STEP_CODE bool Generator::targetPlanned(const RuleTarget& target, int32_t step, int32_t back,
                                            int32_t density) const {
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

GEN_STEP_CODE bool Generator::against(const InterplayRule& rule, int32_t step, int32_t density) const {
	switch (rule.kind) {
	case RuleKind::Lock:
		// Free where the target plays, or with `off` an 8th after it.
		return !targetPlanned(rule.target, step, rule.offBeat ? kStepsPerBeat / 2 : 0, density);
	case RuleKind::Avoid:
		// Against where the target plays, as planned: a muted, Rested or cut target still leaves its holes.
		return targetPlanned(rule.target, step, 0, density);
	case RuleKind::Respond:
		// Against in the busier half, as counted at its first step (countHalves()).
		return busyHalves_[halfOf(step)].intersects(respondMask(rule.target));
	}
	return false;
}

GEN_STEP_CODE void Generator::countHalves(int32_t part, int32_t laneIndex, const Step& step) {
	if (step.index % kHalfBarSteps != 0) {
		return;
	}
	const int32_t half = halfOf(step.index);
	const int32_t bar = step.index - step.index % kStepsPerBar;
	for (const InterplayRule& rule : source(part, laneIndex).rules) {
		if (rule.kind != RuleKind::Respond) {
			continue;
		}
		// The target's planned onsets in each half of this bar, at the Density now, with its pick of this step.
		int32_t counts[2] = {};
		for (int32_t s = 0; s < kStepsPerBar; s++) {
			counts[s / kHalfBarSteps] += targetPlanned(rule.target, bar + s, 0, step.sharedDensity) ? 1 : 0;
		}
		// A non-zero tie makes the first half against, so the response comes after the call.
		const int32_t mine = counts[half];
		const int32_t other = counts[1 - half];
		const bool busy = mine > other || (half == 0 && mine == other && mine > 0);
		const PartsAndLanes mask = respondMask(rule.target);
		busyHalves_[half] = busy ? busyHalves_[half].with(mask) : busyHalves_[half].without(mask);
	}
}

GEN_STEP_CODE bool Generator::avoided(int32_t partIndex, const Step& step) const {
	const PartSpec& spec = genre_->parts[partIndex];
	if (!spec.lanes.empty() || spec.pool.empty()) {
		return false; // drum hits are one-shots, and a part with no pattern has no onsets for a rule to thin
	}
	// Only a rule at full strength counts (spec §5.5): below it the rule only thins, and strength stays the one dial.
	// There every onset but an anchor is dropped where the target plays, so the part plays nothing here: no second
	// walk of the target's plan, which planned() would make. Most parts list no Avoid, so the kind check comes first
	// and they never reach the anchor lookup.
	bool anchorChecked = false;
	for (const InterplayRule& rule : spec.rules) {
		if (rule.kind != RuleKind::Avoid || effectiveStrength(rule, partIndex) != kFullStrength) {
			continue;
		}
		// The short-cut, before the first rule's walk of its target: an anchor always plays, so a step the source
		// anchors is never avoided.
		if (!anchorChecked) {
			const PatternState& state = parts_[partIndex].lanes[0];
			if (state.entry >= 0) {
				const Pattern& pattern = spec.pool[state.entry];
				if (pattern.importance[step.index % pattern.length] == kAnchor) {
					return false;
				}
			}
			anchorChecked = true;
		}
		if (against(rule, step.index, step.sharedDensity)) {
			return true;
		}
	}
	return false;
}

GEN_STEP_CODE int32_t Generator::effectiveStrength(const InterplayRule& rule, int32_t part) const {
	return deluge::generative::effectiveStrength(rule,
	                                             biasedOn(part, Macro::Complexity, recipe_.macros[Macro::Complexity]),
	                                             genre_->defaults[Macro::Complexity]);
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

GEN_STEP_CODE void Gates::crossBar(const PartSpec& spec, int32_t density) {
	open = gateOpens(open, spec.densityGate, density);
	for (int32_t l = 0; l < laneCount(spec); l++) {
		const bool laneOpened = gateOpens(laneOpen(l), spec.lanes[l].densityGate, density);
		lanes = static_cast<uint16_t>(laneOpened ? lanes | (1U << l) : lanes & ~(1U << l));
	}
}

GEN_STEP_CODE bool Generator::gateOpen(int32_t part, Lane lane) const {
	const Gates& gates = parts_[part].gates;
	if (!gates.open) {
		return false;
	}
	int32_t laneIndex = lane == Lane::None ? -1 : laneIndexOf(genre_->parts[part], lane);
	return laneIndex < 0 || gates.laneOpen(laneIndex);
}

GEN_STEP_CODE void Generator::updateGates(const Step& step) {
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		PartState& part = parts_[p];
		part.gates.crossBar(genre_->parts[p], biasedOn(p, Macro::Density, step.density)); // its own (spec §3.3)
		// A closing gate ends its notes on the line, holding them back: a gate that reopens while one still lasts
		// to the end of that bar lets it enter late. Drum and FX one-shots play out anyway, and the Fall never enters
		// late (spec §9.5). An opening gate lets what it held back enter late, after the parts (lateEntries).
		sounding_.forEach(p, [&](SoundingNote& note) {
			if (!gateOpen(p, note.lane) && note.offTick > step.tick) {
				if (note.lane != Lane::Fall) {
					held_.hold(p, note);
				}
				SoundingNotes::cutAt(note, step.tick);
			}
		});
	}
}

void Generator::cutSectionLine(const Step& step) {
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		// A Sustain that holds across Section lines keeps its notes ending on this line uncut, so decideSustain() can
		// hold those whose pitch stays (spec §5.2). A Section line falls on a bar line, where every one of them ends.
		const bool holds = genre_->parts[p].sustain.holdsAcrossSections;
		// An FX note held back by its gate belongs to the Section that placed it, too.
		held_.forEach(p, [](SoundingNote& note) {
			if (isFxLane(note.lane)) {
				note.used = false;
			}
		});
		sounding_.forEach(p, [&](SoundingNote& note) {
			if (holds && note.offTick == step.tick) {
				return;
			}
			// An FX note belongs to the Section that placed it, so it ends here and its tail rings out: a Riser cut
			// short by a forced Section never comes back in the next one.
			if (note.offTick > step.tick && !isFxLane(note.lane)) {
				held_.hold(p, note);
			}
			// Nothing holds on across the line (a Sustain re-strikes, unless it holds).
			SoundingNotes::cutAt(note, step.tick);
		});
	}
}

GEN_STEP_CODE void Generator::lateEntries(const Step& step, uint8_t avoidedParts) {
	const int32_t barEnd = step.tick + kStepsPerBar * kTicksPerStep;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const bool avoidsLine = (avoidedParts & partBit(p)) != 0;
		held_.forEach(p, [&](SoundingNote& note) {
			if (avoidsLine || !gateOpen(p, note.lane) || keptOut(step, p, note.lane)) {
				return; // still gated out, kept out by a section rule or a Rest, or avoiding the line: it waits
			}
			note.used = false;
			// The note started earlier and lasts at least to the end of this bar: its remainder starts on the line.
			// A shorter remainder is skipped.
			if (note.onTick < step.tick && note.offTick >= barEnd) {
				const int32_t energy = biasedOn(p, Macro::Energy, step.energy); // the part's own (spec §3.3)
				noteOn(p, {note.lane, note.pitch, note.baseVelocity, velocity(note.baseVelocity, energy), step.tick,
				           note.offTick});
			}
		});
	}
}

GEN_STEP_CODE int32_t Generator::keyFloor(int32_t partIndex, Lane lane, uint8_t pitch) const {
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

GEN_STEP_CODE void Generator::decideKit(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	const int32_t lanes = laneCount(spec);
	LaneNote notes[kMaxLanesPerPart] = {};
	// The roll each lane plays on this step, the Build roll's or a Roll fill's; RollStep's stage starts at None.
	RollStep rolls[kMaxLanesPerPart];
	for (int32_t l = 0; l < lanes; l++) {
		const LaneSpec& lane = spec.lanes[l];
		LaneNote& note = notes[l];
		if (lane.pool.empty() && !lane.fillPool.empty()) {
			// A lane that only Swaps plays still changes its pick at Drops and re-picks, so its fills change with it.
			placePick(parts_[partIndex].lanes[l], static_cast<uint8_t>(partIndex), lane.lane, step);
		}
		if (!lane.pool.empty()) {
			countHalves(partIndex, l, step); // its targets, earlier lanes and parts, have picked
			PatternState& pick = parts_[partIndex].lanes[l];
			updatePick(pick, lane.pool, lane.band, static_cast<uint8_t>(partIndex), lane.lane, step);
			const Pattern& pattern = lane.pool[pick.entry];
			note.patternStep = step.index % pattern.length;
			if (planned(partIndex, l, step.index, step.sharedDensity)) {
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
		if (buildRollsOn(step, lane.lane)) {
			rolls[l] = step.buildRoll;
			note.on = false;
		}
		// A Roll fill replaces the lane's own hits too, but plays only where the lane's and the part's gates let the
		// lane play, as a Swap does (spec §8.7). fillRules() has left the Build roll's lane out of it.
		else if (step.roll.has(partIndex, lane.lane)) {
			note.on = false;
			if (gateOpen(partIndex, lane.lane)) {
				rolls[l] = step.fillRoll;
			}
		}
		if (silenced(step, partIndex, lane.lane)) {
			note.on = false;
			rolls[l] = {};
		}
		// A Rest comes after everything else. The roll lane is never in it while it rolls (Build roll > fill).
		if (step.rest.has(partIndex, lane.lane)) {
			note.on = false;
		}
	}
	// Alternate lane (spec §5.7): a Drums lane may hand whole Groove bars over to a pool-less lane in the same kit
	// (Dub's Rim -> Snare). Skipped while a fill or Build roll touches either lane, when either lane is gated out or
	// silenced, or on the bar before a drop-out whose Throw targets the primary lane.
	if (arc_.section() == SectionKind::Groove) {
		for (int32_t l = 0; l < lanes; l++) {
			const LaneSpec& lane = spec.lanes[l];
			if (lane.alternateLane == Lane::None || lane.alternateChance <= 0 || !notes[l].on) {
				continue;
			}
			const int32_t alt = laneIndexOf(spec, lane.alternateLane);
			if (alt < 0 || notes[alt].on || rolls[alt].stage != RollStage::None || !gateOpen(partIndex, lane.lane)
			    || !gateOpen(partIndex, lane.alternateLane) || silenced(step, partIndex, lane.alternateLane)
			    || step.rest.has(partIndex, lane.alternateLane) || step.swap.has(partIndex, lane.lane)
			    || step.swap.has(partIndex, lane.alternateLane) || step.roll.has(partIndex, lane.lane)
			    || step.roll.has(partIndex, lane.alternateLane) || buildRollsOn(step, lane.lane)
			    || buildRollsOn(step, lane.alternateLane)) {
				continue;
			}
			if (dropOut_ >= 0 && step.index < dropOutFrom_
			    && genre_->dropOuts[dropOut_].throwTarget.has(partIndex, lane.lane)) {
				continue;
			}
			DrawKey key = partKey(seed_, static_cast<uint8_t>(partIndex), lane.lane, takes_.take[partIndex],
			                      Purpose::AlternateLane);
			key.position[0] =
			    static_cast<uint32_t>(heldStep(partIndex, step.index)) / static_cast<uint32_t>(kStepsPerBar);
			if (drawChance(key, lane.alternateChance)) {
				notes[alt] = notes[l];
				notes[alt].length = spec.lanes[alt].gateTicks;
				notes[l].on = false;
			}
		}
	}
	// Replace pairs come after the plan that interplay rules read, before any voice starts (spec §5.7). A lane that
	// doesn't land (gated out or silenced) replaces nothing; a rolling lane lands where it hits, the Build roll's
	// whatever its gate. Nothing replaces the Build roll: the roll lane always plays (spec §9.5).
	for (const ReplacePair& pair : genre_->replacePairs) {
		int32_t replacing = laneIndexOf(spec, pair.replacing);
		int32_t replaced = laneIndexOf(spec, pair.replaced);
		if (replacing >= 0 && replaced >= 0
		    && (rollHits(rolls[replacing].stage, step.index) > 0
		        || (notes[replacing].on && gateOpen(partIndex, pair.replacing)))) {
			notes[replaced].on = false;
			// A Roll fill's hits are the lane's own, so they give way as a Swap's do; the Build roll's never do.
			if (!buildRollsOn(step, pair.replaced)) {
				rolls[replaced] = {};
			}
		}
	}
	// A punch-in's roll replaces the lane's hits whatever gates, section rules, fills and Replace pairs say: the
	// player's (spec §8.9). A LaneMute still flags it.
	if (punch_.effects != 0 && punchesPart(partIndex)) {
		for (int32_t l = 0; l < lanes; l++) {
			if (const RollStep roll = punchRoll(partIndex, spec.lanes[l].lane); roll.stage != RollStage::None) {
				rolls[l] = roll;
			}
		}
	}
	for (int32_t l = 0; l < lanes; l++) {
		if (rolls[l].stage != RollStage::None) {
			playRoll(partIndex, l, step, rolls[l]);
		}
		else if (notes[l].on) {
			playLane(partIndex, l, notes[l], step);
		}
	}
}

GEN_STEP_CODE void Generator::playLane(int32_t partIndex, int32_t laneIndex, const LaneNote& note, const Step& step) {
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

GEN_STEP_CODE int32_t Generator::express(int32_t partIndex, const Expression& expression, const PatternState& pick,
                                         int32_t patternStep, const Decided& decided, const Step& step) {
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
	// Section's first step, so Compose clips stay self-contained and downbeats land together (spec §8.5), or than the
	// line a new sound or key change cut the part's notes on, so the new sound plays them in the new key (§10.7).
	const bool cutHere = step.sectionStart || (cutParts_ & partBit(partIndex)) != 0;
	int32_t lean = std::clamp<int32_t>(expression.lean[step.index % kStepsPerBeat], -kMaxLean, kMaxLean);
	if (cutHere && lean < 0) {
		lean = 0;
	}
	// Steps that lean into each other on one key never cross: a hit starts after the last one on its key.
	const int32_t floor = keyFloor(partIndex, decided.lane, decided.pitch);
	const int32_t start = step.tick + lean > floor ? step.tick + lean : floor;

	Note note{decided.lane, decided.pitch,         static_cast<int16_t>(base), velocity(base, step.energy),
	          start,        start + decided.length};
	if (!gateOpen(partIndex, decided.lane)) {
		// Gated out: held back plain, for late entry (spec §5.6). The Fall plays only on a Break's first step and
		// never enters late (spec §9.5).
		if (decided.lane != Lane::Fall) {
			held_.hold(partIndex, {.onTick = note.onTick,
			                       .offTick = note.offTick,
			                       .baseVelocity = note.baseVelocity,
			                       .lane = note.lane,
			                       .pitch = note.pitch,
			                       .used = true});
		}
		return start;
	}

	OrnamentKind ornament = ornamentFor(partIndex, expression.ornaments, pick, patternStep, decided.lane);
	if (punch_.effects != 0 && punchesPart(partIndex)) {
		ornament = punchOrnament(partIndex, decided.lane, ornament);
	}
	if (decided.tie && ornament == OrnamentKind::None
	    && sounding_.extend(partIndex, note.lane, note.pitch, note.onTick, note.offTick)) {
		return start; // a slide onto the same note ties
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
		return start;
	}
	case OrnamentKind::Flam: {
		// A soft grace hit a gap before the main hit, which never moves. The grace hit lands no earlier than -12, or
		// than the line on a Section's first step or a cut's line; the gap shrinks to fit, and with none left the note
		// plays plain.
		int32_t limit = cutHere ? step.tick : step.tick - kMaxLean;
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
	return start;
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
	const int32_t complexity = biasedOn(partIndex, Macro::Complexity, recipe_.macros[Macro::Complexity]);
	int32_t rate = ornaments.rateFloor + (ornaments.rateCeiling - ornaments.rateFloor) * complexity / kMacroFull;
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

GEN_STEP_CODE Generator::OrnamentKind Generator::punchOrnament(int32_t partIndex, Lane lane, OrnamentKind drawn) const {
	// Each a fixed amount (spec §8.9): a 32nd double on the Kick, a flam on the lanes flams() names. They replace the
	// hit's own Ornament, whose draw stays as it was, so letting go plays the piece's own again.
	if (lane == Lane::Kick) {
		return punch_.on(PunchIn::DoubleKick) ? OrnamentKind::Ratchet2 : drawn;
	}
	return punch_.on(PunchIn::Flam) && flams(genre_->parts[partIndex], lane) ? OrnamentKind::Flam : drawn;
}

} // namespace deluge::generative
