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

namespace deluge::generative {

namespace {

/// Fields of an automatic fill's draws: whether one plays, and which entry.
enum FillField : int32_t { kFillWhether, kFillWhich };

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
		step.rollStage = rollStageOn(rules, arc_, step.complexity);
		if (step.rollStage != RollStage::None) {
			const int32_t span = arc_.sectionSteps() - gap; // the roll's whole run, to the Gap
			const int32_t left = toDrop - gap;
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

GEN_STEP_CODE void Generator::fillRules(Step& step) {
	const std::span<const FillEntry> fills = genre_->fills;
	const int32_t inPhrase = arc_.stepInPhrase();
	if (inPhrase == 0) {
		autoFill_ = static_cast<int8_t>(drawAutoFill(step));
	}
	// A held fill overrides the automatic one. No fill covers a phrase's first step: a held one pauses there and
	// carries on from the next. The Gap silences fills too.
	int32_t entry = -1;
	const int32_t held = heldSteps_;
	if (heldFill_ >= 0) {
		entry = inPhrase > 0 ? heldFill_ : -1;
		heldSteps_ = static_cast<uint8_t>(std::min<int32_t>(held + 1, kStepsPerBeat));
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
		if (step.rollStage != RollStage::None && genre_->arc.roll.lane != Lane::None) {
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
			if (entry == heldFill_) {
				rollFillAt(step, held, 0);
			}
			else {
				const int32_t start = autoFillStart(fill.beats);
				rollFillAt(step, inPhrase - start, genre_->phraseSteps() - start);
			}
			break;
		}
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

GEN_STEP_CODE void Generator::rollFillAt(Step& step, int32_t i, int32_t steps) const {
	// 16ths from the first step, then the genre's top roll stage: for an automatic Roll its last quarter, for a held
	// one from its second beat on. Velocity climbs from normal to accent, over the ramp or the held Roll's first beat.
	const RollStage top = genre_->arc.roll.top;
	const int32_t climb = steps > 0 ? steps : kStepsPerBeat;
	const bool topStage = steps > 0 ? 4 * i >= 3 * steps : i >= kStepsPerBeat;
	step.rollFillStage = topStage ? top : RollStage::Sixteenth;
	constexpr int32_t kNormal = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Normal)];
	constexpr int32_t kAccent = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Accent)];
	const int32_t climbed = std::min(i, climb - 1);
	step.rollFillVelocity = static_cast<int16_t>(kNormal + (kAccent - kNormal) * climbed / std::max(climb - 1, 1));
}

int32_t Generator::autoFillStart(int32_t beats) const {
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
	if (step.gap || step.cut.has(part, lane)) {
		return true;
	}
	return !step.keeps.lets(part, lane);
}

void Generator::updateSilent(const Step& step) {
	silent_ = 0;
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		const bool plays = mayPlay(genre_->parts[p], parts_[p].gates, genre_->arc.roll.lane, step.rollStage,
		                           [&](Lane lane) { return !silenced(step, p, lane); });
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

GEN_STEP_CODE void Generator::playRoll(int32_t partIndex, int32_t laneIndex, const Step& step, RollStage stage,
                                       int16_t base) {
	const LaneSpec& spec = genre_->parts[partIndex].lanes[laneIndex];
	// A roll decides its steps, not Density, and plays no Lean, spread or Ornament.
	const int32_t hits = rollHits(stage, step.index);
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
		noteOn(partIndex, {spec.lane, 0, base, velocity(base, step.energy), on, on + length, i > 0});
	}
}

} // namespace deluge::generative
