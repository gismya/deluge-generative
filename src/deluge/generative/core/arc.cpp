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
#include "generative/core/motion_curve.h"

namespace deluge::generative {

namespace {

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

GEN_STEP_CODE int32_t drawInRange(const DrawKey& key, Range range) {
	return range.low + drawBelow(key, inclusiveSpan(range.low, range.high));
}

/// The lowest Density gate of any part or lane that plays patterns: a lane opens only with its part, so its gate is
/// the higher of the two. Lanes without a pool play only by section rules and never in an Intro, so they don't count.
int32_t lowestGate(const Genre& genre) {
	int32_t lowest = INT32_MAX;
	for (const PartSpec& part : genre.parts) {
		if (!part.pool.empty() && part.densityGate < lowest) {
			lowest = part.densityGate;
		}
		for (const LaneSpec& lane : part.lanes) {
			int32_t gate = lane.densityGate > part.densityGate ? lane.densityGate : part.densityGate;
			if (!lane.pool.empty() && gate < lowest) {
				lowest = gate;
			}
		}
	}
	return lowest == INT32_MAX ? 0 : lowest;
}

/// A Density value a genre may leave unset, or the Energy one it then uses (spec §9.4): for Energy itself, or when
/// `density` is kSameAsEnergy.
constexpr int32_t forMacro(bool energy, int32_t energyValue, int32_t density) {
	return energy || density == kSameAsEnergy ? energyValue : density;
}

} // namespace

void Arc::start(const Genre& genre, SectionKind first, bool onDrop) {
	*this = Arc{};
	first_ = first;
	// The first Section begins as if a Build had just ended: begin() then makes it a Drop.
	kind_ = onDrop ? SectionKind::Build : kind_;
	phraseSteps_ = genre.phraseSteps();
	introFloor_ = static_cast<int16_t>(lowestGate(genre));
	// Headroom scales each side of each macro by the largest offset the section rules give it (spec §9.4). A Build
	// ramps from wherever it starts, so its `from` isn't an offset; Density's Intro and Outro curves aren't either.
	const ArcSpec& arc = genre.arc;
	for (int32_t m = 0; m < 2; m++) {
		bool energy = m == 0;
		int32_t highest = forMacro(energy, arc.dropLiftEnergy, arc.dropLiftDensity);
		int32_t deepest = 0;
		for (int32_t k = 0; k < kNumSectionKinds; k++) {
			auto kind = static_cast<SectionKind>(k);
			if (!energy && (kind == SectionKind::Intro || kind == SectionKind::Outro)) {
				continue;
			}
			const SectionOffsets& shape = arc.offsets[k];
			int32_t ends[2] = {forMacro(energy, shape.energyFrom, shape.densityFrom),
			                   forMacro(energy, shape.energyTo, shape.densityTo)};
			for (int32_t e = kind == SectionKind::Build ? 1 : 0; e < 2; e++) {
				highest = ends[e] > highest ? ends[e] : highest;
				deepest = -ends[e] > deepest ? -ends[e] : deepest;
			}
		}
		maxPositive_[m] = static_cast<int16_t>(highest);
		maxNegative_[m] = static_cast<int16_t>(deepest);
	}
}

GEN_STEP_CODE bool Arc::enterStep(const Genre& genre, uint32_t seed, int32_t step, const Macros& centre) {
	if (step_ < 0) {
		step_ = step;
		begin(genre, seed, forced_ ? forcedKind_ : first_, centre, forcedBuild_);
		forced_ = false;
		return true;
	}
	step_ = step;
	const int32_t inPhrase = (stepInPhrase_ + 1) % phraseSteps_;
	if (forced_ || endsBefore(inPhrase, stepInSection_ + 1, centre[Macro::Motion])) {
		// The counters still stand on the old Section's last step, so a Build starts from its offsets there.
		SectionKind kind = forced_ ? forcedKind_ : next_;
		begin(genre, seed, kind, centre, forced_ ? forcedBuild_ : nextBuild_);
		forced_ = false;
		stepInPhrase_ = inPhrase;
		return true;
	}
	stepInPhrase_ = inPhrase;
	stepInSection_++;
	if (inPhrase == 0) {
		phraseInSection_++;
	}
	return false;
}

bool Arc::endsBefore(int32_t stepInPhrase, int32_t stepInSection, int32_t motion) const {
	// The next Section can only start where it fits the phrase grid: a phrase line, or for a Build shorter than a
	// phrase, the bar line that lets it end on one (spec §9.3).
	if (!startsOnLine(next_, nextBuild_, stepInPhrase)) {
		return false;
	}
	switch (kind_) {
	case SectionKind::Build:
		return stepInSection >= lengthSteps_; // a Build always lands its Drop, held or not
	case SectionKind::Groove:
		return grooveEndsInPhrase(motion);
	case SectionKind::Outro:
		return false; // the transport stops where the Outro ends (spec §13.2)
	default:
		return !held_ && stepInSection >= lengthSteps_;
	}
}

GEN_STEP_CODE bool Arc::grooveEndsInPhrase(int32_t motion) const {
	// It ends at the first line where phrases played (the one this line ends included) × the Groove curve's multiple at
	// the live Motion reaches its base. Motion 0 never ends it.
	if (kind_ != SectionKind::Groove || held_ || motion <= 0) {
		return false;
	}
	int64_t phrases = phraseInSection_ + 1;
	return phrases * motionMultiple(curves::grooveLength, motion) >= int64_t{grooveBase_} * kCurveOne;
}

SectionKind Arc::naturalNext(const Genre& genre, uint32_t seed) const {
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
		return SectionKind::Outro;
	}
	return SectionKind::Groove;
}

GEN_STEP_CODE int32_t Arc::buildSteps(const Genre& genre, uint32_t seed, uint16_t occurrence) const {
	std::span<const WeightedBars> bars = genre.arc.buildBars;
	int32_t entry = drawWeighted(arcKey(seed, Purpose::BuildLength, occurrence), bars);
	int32_t steps = entry < 0 ? phraseSteps_ : bars[entry].bars * kStepsPerBar;
	// Only lengths that divide the phrase or are whole phrases land the Drop on a phrase line. A genre data spec
	// checks every genre; anything else plays a phrase.
	return buildFits(steps) ? steps : phraseSteps_;
}

int32_t Arc::nextBuildSteps(const Genre& genre, uint32_t seed) const {
	return buildSteps(genre, seed, started_[static_cast<int32_t>(SectionKind::Build)]);
}

int32_t Arc::stepsToStartFrom(SectionKind kind, int32_t buildSteps, int32_t stepInPhrase) const {
	// A Build's end, any other Section's start, must meet a phrase line.
	const int32_t meets = stepInPhrase + (kind == SectionKind::Build ? buildSteps : 0);
	return (phraseSteps_ - meets % phraseSteps_) % phraseSteps_;
}

void Arc::force(SectionKind kind, int32_t buildSteps) {
	forced_ = true;
	forcedKind_ = kind;
	forcedBuild_ = buildSteps;
}

GEN_STEP_CODE void Arc::begin(const Genre& genre, uint32_t seed, SectionKind kind, const Macros& centre,
                              int32_t buildSteps) {
	if (kind == SectionKind::Build) {
		// Each macro ramps from where it plays at the Build's start, so no Build jumps on its first step: after a
		// Break, a Groove, or forced in the Intro.
		buildFrom_[0] = static_cast<int16_t>(offset(genre, Macro::Energy, centre[Macro::Energy]));
		buildFrom_[1] = static_cast<int16_t>(offset(genre, Macro::Density, centre[Macro::Density]));
	}
	afterDrop_ = kind == SectionKind::Groove && kind_ == SectionKind::Build;
	if (kind == SectionKind::Groove && holdAtGroove_) {
		held_ = true;
		holdAtGroove_ = false;
	}
	kind_ = kind;
	occurrence_ = started_[static_cast<int32_t>(kind)]++;
	stepInSection_ = 0;
	phraseInSection_ = 0;
	next_ = naturalNext(genre, seed);
	nextBuild_ = next_ == SectionKind::Build ? nextBuildSteps(genre, seed) : 0;
	// A Build shorter than a phrase takes the end of the phrase before it (spec §9.3).
	const int32_t taken = nextBuild_ < phraseSteps_ ? nextBuild_ : 0;

	const ArcSpec& arc = genre.arc;
	switch (kind) {
	case SectionKind::Intro:
		lengthSteps_ = arc.introPhrases * phraseSteps_;
		break;
	case SectionKind::Outro:
		lengthSteps_ = arc.outroPhrases * phraseSteps_;
		break;
	case SectionKind::Groove:
		grooveBase_ = drawInRange(arcKey(seed, Purpose::GrooveLength, occurrence_), arc.groovePhrases);
		lengthSteps_ = 0;
		break;
	case SectionKind::Break:
		lengthSteps_ =
		    drawInRange(arcKey(seed, Purpose::BreakLength, occurrence_), arc.breakPhrases) * phraseSteps_ - taken;
		break;
	case SectionKind::Build:
		lengthSteps_ = buildSteps;
		break;
	}
	if (kind != SectionKind::Groove && lengthSteps_ < 1) {
		lengthSteps_ = phraseSteps_;
	}
}

GEN_STEP_CODE int32_t Arc::headroom(int32_t offset, int32_t centre, bool energy) const {
	const int32_t i = energy ? 0 : 1;
	if (offset > 0) {
		int32_t room = kMacroFull - centre;
		return room < maxPositive_[i] ? offset * room / maxPositive_[i] : offset;
	}
	if (offset < 0) {
		int32_t room = centre;
		return room < maxNegative_[i] ? offset * room / maxNegative_[i] : offset;
	}
	return 0;
}

GEN_STEP_CODE int32_t Arc::offset(const Genre& genre, Macro macro, int32_t centre) const {
	if (macro != Macro::Energy && macro != Macro::Density) {
		return 0;
	}
	if (step_ < 0) {
		return 0;
	}
	const ArcSpec& arc = genre.arc;
	const SectionOffsets& shape = arc.offsets[static_cast<int32_t>(kind_)];
	const bool energy = macro == Macro::Energy;
	// A held Section plays on at its last step's offsets.
	const int32_t step = lengthSteps_ > 0 && stepInSection_ >= lengthSteps_ ? lengthSteps_ - 1 : stepInSection_;

	if (!energy && (kind_ == SectionKind::Intro || kind_ == SectionKind::Outro)) {
		// Density rises from the Intro floor to the centre, and falls back in the Outro. Not an offset, so no
		// headroom. A centre below the floor stays where it is.
		if (centre <= introFloor_) {
			return 0;
		}
		bool intro = kind_ == SectionKind::Intro;
		return lerp(intro ? introFloor_ : centre, intro ? centre : introFloor_, step, lengthSteps_) - centre;
	}
	// Each end of a Density offset the genre leaves unset uses the Energy offset there (spec §9.4).
	int32_t from = forMacro(energy, shape.energyFrom, shape.densityFrom);
	int32_t to = forMacro(energy, shape.energyTo, shape.densityTo);

	switch (kind_) {
	case SectionKind::Groove: {
		if (!afterDrop_) {
			return 0;
		}
		// The lift decays back to 0. A Section starting before the decay ends takes over there.
		int32_t decaySteps = arc.dropDecayPhrases * phraseSteps_;
		if (stepInSection_ >= decaySteps) {
			return 0;
		}
		int32_t lift = forMacro(energy, arc.dropLiftEnergy, arc.dropLiftDensity);
		return headroom(lift * (decaySteps - stepInSection_) / decaySteps, centre, energy);
	}
	case SectionKind::Build:
		return lerp(buildFrom_[energy ? 0 : 1], headroom(to, centre, energy), step, lengthSteps_);
	default:
		return headroom(lerp(from, to, step, lengthSteps_), centre, energy);
	}
}

} // namespace deluge::generative
