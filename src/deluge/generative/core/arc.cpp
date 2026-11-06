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

#include "generative/core/arc.h"
#include "generative/core/draw.h"

namespace deluge::generative {

namespace {

/// Arc draws belong to no part.
constexpr uint8_t kArcPart = 0xFF;

DrawKey arcKey(uint32_t seed, Purpose purpose, uint32_t occurrence) {
	DrawKey key;
	key.seed = seed;
	key.part = kArcPart;
	key.purpose = purpose;
	key.position[0] = occurrence;
	return key;
}

/// `from` on step 0 to `to` on step `length - 1`, linearly, rounding toward zero.
int32_t lerp(int32_t from, int32_t to, int32_t step, int32_t length) {
	if (length <= 1) {
		return to;
	}
	return from + (to - from) * step / (length - 1);
}

int32_t drawInRange(const DrawKey& key, Range range) {
	int32_t span = range.high >= range.low ? range.high - range.low + 1 : 1;
	return range.low + drawBelow(key, span);
}

} // namespace

void Arc::start(const Genre& genre, uint32_t seed) {
	*this = Arc{};
	phraseSteps_ = genre.phraseSteps();
	(void)seed;
}

bool Arc::enterStep(const Genre& genre, uint32_t seed, int32_t step) {
	if (step_ < 0) {
		step_ = step;
		begin(genre, seed, SectionKind::Intro, step);
		return true;
	}
	step_ = step;
	stepInPhrase_ = (stepInPhrase_ + 1) % phraseSteps_;
	if (stepInSection_ + 1 < sectionSteps_) {
		stepInSection_++;
		if (stepInPhrase_ == 0) {
			phraseInSection_++;
		}
		return false;
	}
	// The counters still stand on the old Section's last step, so a Build starts from its offsets there.
	begin(genre, seed, next(genre, seed), step);
	return true;
}

SectionKind Arc::next(const Genre& genre, uint32_t seed) const {
	switch (kind_) {
	case SectionKind::Intro:
		return SectionKind::Groove;
	case SectionKind::Groove:
		if (drawChance(arcKey(seed, Purpose::GrooveToBuild, occurrence_), genre.arc.grooveToBuild)) {
			return SectionKind::Build;
		}
		return SectionKind::Break;
	case SectionKind::Break:
		return SectionKind::Build;
	case SectionKind::Build:
		return SectionKind::Groove; // on a Drop
	case SectionKind::Outro:
		return SectionKind::Outro; // the transport stops where the Outro ends
	}
	return SectionKind::Groove;
}

int32_t Arc::sectionLength(const Genre& genre, uint32_t seed, SectionKind kind, uint16_t occurrence) const {
	const ArcSpec& arc = genre.arc;
	switch (kind) {
	case SectionKind::Intro:
		return arc.introPhrases * phraseSteps_;
	case SectionKind::Outro:
		return arc.outroPhrases * phraseSteps_;
	case SectionKind::Groove:
		return drawInRange(arcKey(seed, Purpose::GrooveLength, occurrence), arc.groovePhrases) * phraseSteps_;
	case SectionKind::Break:
		return drawInRange(arcKey(seed, Purpose::BreakLength, occurrence), arc.breakPhrases) * phraseSteps_;
	case SectionKind::Build: {
		int32_t total = 0;
		for (const WeightedBars& entry : arc.buildBars) {
			total += entry.weight;
		}
		int32_t pick = drawBelow(arcKey(seed, Purpose::BuildLength, occurrence), total > 0 ? total : 1);
		for (const WeightedBars& entry : arc.buildBars) {
			if (pick < entry.weight) {
				return entry.bars * kStepsPerBar;
			}
			pick -= entry.weight;
		}
		return phraseSteps_;
	}
	}
	return phraseSteps_;
}

void Arc::begin(const Genre& genre, uint32_t seed, SectionKind kind, int32_t step) {
	(void)step;
	if (kind == SectionKind::Build) {
		// Each macro ramps from its own offset at the Build's start, so no Build jumps on its first step.
		buildFrom_[0] = static_cast<int16_t>(offset(genre, Macro::Energy));
		buildFrom_[1] = static_cast<int16_t>(offset(genre, Macro::Density));
	}
	afterDrop_ = kind == SectionKind::Groove && kind_ == SectionKind::Build;
	kind_ = kind;
	occurrence_ = started_[static_cast<int32_t>(kind)]++;
	stepInSection_ = 0;
	phraseInSection_ = 0;
	sectionSteps_ = sectionLength(genre, seed, kind, occurrence_);
	if (sectionSteps_ < 1) {
		sectionSteps_ = phraseSteps_;
	}
}

int32_t Arc::offset(const Genre& genre, Macro macro) const {
	if (macro != Macro::Energy && macro != Macro::Density) {
		return 0;
	}
	if (step_ < 0) {
		return 0;
	}
	const ArcSpec& arc = genre.arc;
	const SectionOffsets& shape = arc.offsets[static_cast<int32_t>(kind_)];
	bool energy = macro == Macro::Energy;
	bool densityUnset = shape.densityFrom == kSameAsEnergy || shape.densityTo == kSameAsEnergy;
	int32_t from = energy || densityUnset ? shape.energyFrom : shape.densityFrom;
	int32_t to = energy || densityUnset ? shape.energyTo : shape.densityTo;

	switch (kind_) {
	case SectionKind::Groove: {
		if (!afterDrop_) {
			return 0;
		}
		int32_t decaySteps = arc.dropDecayPhrases * phraseSteps_;
		if (stepInSection_ >= decaySteps) {
			return 0;
		}
		int32_t lift = energy ? arc.dropLiftEnergy : arc.dropLiftDensity;
		return lift * (decaySteps - stepInSection_) / decaySteps;
	}
	case SectionKind::Build:
		return lerp(buildFrom_[energy ? 0 : 1], to, stepInSection_, sectionSteps_);
	default:
		return lerp(from, to, stepInSection_, sectionSteps_);
	}
}

} // namespace deluge::generative
