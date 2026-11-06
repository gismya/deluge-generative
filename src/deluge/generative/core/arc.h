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

// The Arc (spec §9): the Generator's movement through Sections around the Recipe's macro centre. It keeps the Arc
// counters every keyed draw positions on (spec §6.2) and the effective Energy and Density offsets (spec §9.4).
// Part of a Generator's state block; it holds no pointers.

#pragma once

#include "generative/core/genre.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

class Arc {
public:
	/// Resets to the piece's first step: the genre's Intro.
	void start(const Genre& genre, uint32_t seed);

	/// Moves to `step`, the next step after the last one (or 0 after start()).
	/// @return true if a Section starts on this step.
	bool enterStep(const Genre& genre, uint32_t seed, int32_t step);

	[[nodiscard]] SectionKind section() const { return kind_; }
	/// 0 for the first Section of its kind in the piece.
	[[nodiscard]] uint16_t occurrence() const { return occurrence_; }
	[[nodiscard]] bool sectionStartsHere() const { return stepInSection_ == 0; }

	[[nodiscard]] int32_t stepInBar() const { return stepInPhrase_ % kStepsPerBar; }
	[[nodiscard]] int32_t barInPhrase() const { return stepInPhrase_ / kStepsPerBar; }
	[[nodiscard]] int32_t stepInPhrase() const { return stepInPhrase_; }
	[[nodiscard]] int32_t phraseInSection() const { return phraseInSection_; }
	[[nodiscard]] int32_t sectionPhrases() const { return sectionSteps_ / phraseSteps_; }

	/// The Arc's offset from the Recipe centre for Energy or Density on the current step, in macro units.
	/// Complexity and Motion are never moved (spec §3.3).
	[[nodiscard]] int32_t offset(const Genre& genre, Macro macro) const;

private:
	void begin(const Genre& genre, uint32_t seed, SectionKind kind, int32_t step);
	[[nodiscard]] int32_t sectionLength(const Genre& genre, uint32_t seed, SectionKind kind, uint16_t occurrence) const;
	[[nodiscard]] SectionKind next(const Genre& genre, uint32_t seed) const;

	SectionKind kind_ = SectionKind::Intro;
	uint16_t occurrence_ = 0;
	uint16_t started_[kNumSectionKinds] = {}; ///< Sections of each kind begun so far.
	int32_t phraseSteps_ = kStepsPerBar * 8;
	int32_t step_ = -1;
	int32_t stepInSection_ = 0;
	int32_t sectionSteps_ = 0;
	int32_t stepInPhrase_ = 0;
	int32_t phraseInSection_ = 0;
	bool afterDrop_ = false;    ///< This Groove started on a Drop.
	int16_t buildFrom_[2] = {}; ///< Energy and Density offsets when the Build started.
};

} // namespace deluge::generative
