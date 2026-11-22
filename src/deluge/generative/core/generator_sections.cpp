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

// The Generator's Section rules on the step: keeps, the Build roll, fills and silences (spec §8.3, §8.7, §9.5).

#include "generative/core/generator.h"
#include "generative/core/motion_curve.h"
#include <algorithm>
#ifdef GENERATOR_TEST_SEAMS
#include <cassert>
#endif

namespace deluge::generative {

namespace {

/// Fields of an automatic fill's draws: whether one plays, and which entry.
enum FillField : int32_t { kFillWhether, kFillWhich };
/// Fields of a drop-out's draw: whether one starts on the next bar line, and which entry.
enum DropOutField : int32_t { kDropOutWhether, kDropOutWhich };

} // namespace

// ---- Section rules (spec §8.3, §9.5) ----

GEN_STEP_CODE void Generator::sectionRules(Step& step) {
	const ArcSpec& rules = genre_->arc;
	const SectionKind kind = arc_.section();
	step.keeps = rules.keepsIn(kind);
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

		// Velocity climbs to the Gap (spec §8.3).
		step.buildRoll.stage = rollStageOn(rules, arc_, step.complexity);
		if (step.buildRoll.stage != RollStage::None) {
			const int32_t span = arc_.sectionSteps() - gap; // the roll's whole run, to the Gap
			const int32_t left = toDrop - gap;
			step.buildRoll.velocity = climbVelocity(kRollVelocity[0], kRollVelocity[1], span - left, span);
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

// ---- Drop-outs (spec §8.8) ----

GEN_STEP_CODE bool DropOutState::stepOrDraw(const Genre& genre, uint32_t seed, const Arc& arc, int32_t stepIndex,
                                            int32_t motion, PartsAndLanes& outMask, DrawKey& outKey) {
	const std::span<const DropOutEntry> entries = genre.dropOuts;
	if (entries.empty()) {
		return false;
	}
	const int32_t n = stepIndex;
	// Any Section line ends a drop-out on it: one playing ends there, and one drawn to start there or after doesn't
	// play. A part already spiked for it comes home on its next hit, as for any plan that changes.
	if (entry >= 0 && n < to() && arc.sectionStartsHere()) {
		if (n <= from) {
			entry = -1;
		}
		else {
#ifdef GENERATOR_TEST_SEAMS
			// A Groove starts only on a phrase line, which no drop-out runs past, so no Groove's Reset ever ends one
			// early: a Throw it holds always falls (updateThrow()), never snaps home at a downbeat.
			assert(arc.section() != SectionKind::Groove);
#endif
			length = static_cast<int16_t>(n - from);
		}
	}
	if (entry >= 0 && n == to()) {
		after = n + genre.dropOutGapBars * kStepsPerBar;
	}
	if (entry >= 0 && n >= from && n < to()) {
		outMask = entries[entry].mask;
		return false;
	}

	// Drawn a bar ahead, on a Groove's bar line before a grid line (`dropOutGridBars`) with none waiting and the gap
	// passed, so the bar in between can throw the target's last hit. It ends on or before the phrase line after its
	// start and the next Section start known now (a Build that takes the phrase's end, a waiting force, End or
	// transition). Keyed on the Groove, the phrase and the bar, never on a take.
	const bool waiting = entry >= 0 && n < from;
	const int32_t nextFrom = n + kStepsPerBar;
	if (waiting || arc.section() != SectionKind::Groove || arc.stepInBar() != 0 || nextFrom < after) {
		return false;
	}
	if (((arc.barInPhrase() + 1) & (genre.dropOutGridBars - 1)) != 0) {
		return false;
	}
	// Whether one starts first: the chance is keyed, so drawing it before the room changes nothing but the work.
	outKey = onPhrase(partKey(seed, kArcPart, Lane::None, 0, Purpose::DropOut), arc.occurrence(),
	                  static_cast<uint16_t>(arc.phraseInSection()),
	                  fieldWord(static_cast<uint32_t>(arc.barInPhrase()), kDropOutWhether));
	return drawChance(outKey, scaleChance(genre.dropOutChance, curves::dropOut, motion));
}

GEN_STEP_CODE void DropOutState::pick(const Genre& genre, const Arc& arc, int32_t stepIndex, int32_t motion,
                                      const Arc::Move& waiting, const DrawKey& key) {
	const std::span<const DropOutEntry> entries = genre.dropOuts;
	const int32_t phraseSteps = genre.phraseSteps();
	int32_t room = phraseSteps - (arc.stepInPhrase() + kStepsPerBar) % phraseSteps;
	const Arc::Ahead ahead = arc.ahead(motion, waiting);
	if (ahead.start != Arc::kNotAhead) {
		room = std::min(room, ahead.start - kStepsPerBar);
	}
	const int32_t picked = drawWeightedIf(withField(key, kDropOutWhich), entries,
	                                      [&](const DropOutEntry& e) { return e.bars * kStepsPerBar <= room; });
	if (picked >= 0) {
		const int32_t nextFrom = stepIndex + kStepsPerBar;
		entry = static_cast<int8_t>(picked);
		from = nextFrom;
		length = static_cast<int16_t>(entries[picked].bars * kStepsPerBar);
	}
}

GEN_STEP_CODE void Generator::dropOutRules(Step& step) {
	step.dropOut =
	    dropOut_.step(*genre_, seed_, arc_, step.index, step.motion, [this] GEN_STEP_CODE { return waitingMove(); });
}

// ---- Fills (spec §8.7) ----

GEN_STEP_CODE void Generator::fillRules(Step& step) {
	const std::span<const FillEntry> fills = genre_->fills;
	const int32_t inPhrase = arc_.stepInPhrase();
	if (inPhrase == 0) {
		autoFill_ = static_cast<int8_t>(drawAutoFill(step));
	}
	// A held fill overrides the automatic one. No fill covers a phrase's first step: a held one pauses there and
	// carries on from the next. The Gap silences fills too.
	int32_t entry = -1;
	const bool held = heldFill_ >= 0;
	const int32_t heldSteps = heldSteps_;
	if (held) {
		// Every step held counts, a phrase's first step and the Gap's included (spec §8.7).
		entry = inPhrase > 0 ? heldFill_ : -1;
		heldSteps_ = static_cast<uint8_t>(std::min<int32_t>(heldSteps + 1, kStepsPerBeat));
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
	step.roll = {};
	if (entry >= 0) {
		const FillEntry& fill = fills[entry];
		PartsAndLanes mask = fill.mask;
		// The roll lane keeps rolling under a fill, which changes only the other lanes (Build roll > fill).
		if (buildRollsOn(step, genre_->arc.roll.lane)) {
			mask.lanes &= static_cast<uint16_t>(~laneBit(genre_->arc.roll.lane));
		}
		switch (fill.kind) {
		case FillKind::Rest:
			step.rest = mask;
			break;
		case FillKind::Swap:
			step.swap = mask;
			break;
		case FillKind::Roll:
			step.roll = mask;
			if (held) {
				step.fillRoll = heldRollAt(heldSteps);
			}
			else {
				const int32_t start = autoFillStart(fill.beats);
				step.fillRoll = autoRollAt(inPhrase - start, genre_->phraseSteps() - start);
			}
			break;
		}
	}
	// A drop-out beats a fill on a part or lane they share: it stays silent, and a Swap or a Roll there doesn't play
	// (spec §8.8). Taking it out of the Rest keeps the Rest from ending a dropped part's sounding notes. Taking it out
	// of the Swap and the Roll changes no output (silenced() already stops a dropped lane in decideKit()), but keeps
	// each mask saying what plays.
	step.rest = step.rest.without(step.dropOut);
	step.swap = step.swap.without(step.dropOut);
	step.roll = step.roll.without(step.dropOut);

	// As a Rest starts, the notes still sounding on its pitched parts end, so their release plays: a clean drop. Drum
	// one-shots play out. A held Rest that pauses on a phrase's first step starts again on the next.
	uint8_t rested = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		if (genre_->parts[p].lanes.empty() && step.rest.has(p, Lane::None)) {
			rested |= partBit(p);
			if ((rested_ & partBit(p)) == 0) {
				sounding_.endPitchedBy(p, step.tick);
			}
		}
	}
	rested_ = rested;
	filled_ = entry >= 0;
}

namespace {

/// A Roll fill's step `i`, its velocity climbing from normal to accent over `climb` steps and its stage `top` from
/// then on.
GEN_STEP_CODE RollStep rollStep(int32_t i, int32_t climb, bool topStage, RollStage top) {
	constexpr int32_t kNormal = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Normal)];
	constexpr int32_t kAccent = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Accent)];
	return {topStage ? top : RollStage::Sixteenth,
	        climbVelocity(kNormal, kAccent, std::min<int32_t>(i, climb - 1), climb - 1)};
}

} // namespace

GEN_STEP_CODE RollStep Generator::heldRollAt(int32_t i) const {
	// 16ths for its first beat, climbing to accent, then the genre's top roll stage.
	return rollStep(i, kStepsPerBeat, i >= kStepsPerBeat, genre_->arc.roll.top);
}

GEN_STEP_CODE RollStep Generator::autoRollAt(int32_t i, int32_t steps) const {
	// 16ths, then the genre's top roll stage for its last quarter; velocity climbs over the whole ramp.
	return rollStep(i, steps, 4 * i >= 3 * steps, genre_->arc.roll.top);
}

GEN_STEP_CODE int32_t Generator::autoFillStart(int32_t beats) const {
	// It ends on the phrase line and never covers the phrase's first step.
	return std::max<int32_t>(genre_->phraseSteps() - beats * kStepsPerBeat, 1);
}

GEN_STEP_CODE int32_t Generator::drawAutoFill(const Step& step) const {
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

GEN_STEP_CODE bool Generator::silenced(const Step& step, int32_t part, Lane lane) const {
	if (step.gap || step.cut.has(part, lane) || step.dropOut.has(part, lane)) {
		return true;
	}
	return !step.keeps.lets(part, lane);
}

void Generator::updateSilent(const Step& step) {
	silent_ = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const bool plays = mayPlay(genre_->parts[p], parts_[p].gates, genre_->arc.roll.lane, step.buildRoll.stage,
		                           [&](Lane lane) { return !silenced(step, p, lane); });
		silent_ |= plays ? 0 : partBit(p);
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
		// One note across the Break, from its first step: a Fall sample of 2 s or more plays in CUT mode, and its
		// note-off would cut it (spec §10.4).
		return arc_.section() == SectionKind::Break ? arc_.sectionSteps() * kTicksPerStep : 0;
	case Lane::Impact:
		return arc_.dropStartsHere() ? lane.gateTicks : 0;
	case Lane::Siren:
		// One held note from a Break's first step or a Drop; a Section line or its gate may end it sooner.
		return arc_.section() == SectionKind::Break || arc_.dropStartsHere()
		           ? lane.heldBars * kStepsPerBar * kTicksPerStep
		           : 0;
	default:
		return 0;
	}
}

GEN_STEP_CODE RollStep Generator::punchRoll(int32_t partIndex, Lane lane) const {
	// A fixed stage at normal velocity (tuning), on the Closed hat, and on the Snare or, in a kit with none, the Clap
	// (spec §8.9).
	constexpr auto kNormal = static_cast<int16_t>(kLevelVelocity[static_cast<int32_t>(VelocityLevel::Normal)]);
	if (lane == Lane::ClosedHat && punch_.on(PunchIn::HatRoll)) {
		return {RollStage::ThirtySecond, kNormal};
	}
	if (punch_.on(PunchIn::SnareRoll)
	    && lane == (laneIndexOf(genre_->parts[partIndex], Lane::Snare) >= 0 ? Lane::Snare : Lane::Clap)) {
		return {RollStage::Sixteenth, kNormal};
	}
	return {};
}

GEN_STEP_CODE void Generator::playRoll(int32_t partIndex, int32_t laneIndex, const Step& step, RollStep roll) {
	const LaneSpec& spec = genre_->parts[partIndex].lanes[laneIndex];
	// A roll decides its steps, not Density, and plays no Lean, spread or Ornament.
	const int32_t hits = rollHits(roll.stage, step.index);
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
		noteOn(partIndex, {spec.lane, 0, roll.velocity, velocity(roll.velocity, step.energy), on, on + length, i > 0});
	}
}

} // namespace deluge::generative
