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
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

class Arc {
public:
	/// Resets to the piece's first step, with no hold and nothing forced. A piece starts with the genre's Intro, or in
	/// Groove for SHIFT+PLAY (spec §13.2).
	void start(const Genre& genre, SectionKind first = SectionKind::Intro);

	/// Moves to `step`, the next step after the last one (or 0 after start()). `centre` is the Recipe's macros now:
	/// Motion paces the Groove (spec §9.3), and a Build ramps from where Energy and Density play when it starts.
	/// @return true if a Section starts on this step.
	bool enterStep(const Genre& genre, uint32_t seed, int32_t step, const Macros& centre);

	[[nodiscard]] SectionKind section() const { return kind_; }
	/// 0 for the first Section of its kind in the piece.
	[[nodiscard]] uint16_t occurrence() const { return occurrence_; }
	[[nodiscard]] bool sectionStartsHere() const { return stepInSection_ == 0; }
	/// Whether this step is an Arc Drop: the Groove a Build lands on starts here.
	[[nodiscard]] bool dropStartsHere() const { return stepInSection_ == 0 && afterDrop_; }

	[[nodiscard]] int32_t stepInBar() const { return stepInPhrase_ % kStepsPerBar; }
	[[nodiscard]] int32_t barInPhrase() const { return stepInPhrase_ / kStepsPerBar; }
	[[nodiscard]] int32_t stepInPhrase() const { return stepInPhrase_; }
	[[nodiscard]] int32_t phraseInSection() const { return phraseInSection_; }
	[[nodiscard]] int32_t stepInSection() const { return stepInSection_; }
	/// Where the next step falls in its phrase: 0 if it is a phrase line.
	[[nodiscard]] int32_t nextStepInPhrase() const { return step_ < 0 ? 0 : (stepInPhrase_ + 1) % phraseSteps_; }
	/// The Section's planned length in steps, or 0 for a Groove, which ends by Motion (spec §9.3). A Break that a short
	/// Build follows is already shortened by it.
	[[nodiscard]] int32_t sectionSteps() const { return lengthSteps_; }
	/// In a Build: steps from this one to the Drop, 1 on its last step.
	[[nodiscard]] int32_t stepsToDrop() const { return lengthSteps_ - stepInSection_; }

	/// The Arc's offset from the Recipe centre for Energy or Density on the current step, in macro units, as it plays
	/// around `centre` (that macro's Recipe value): scaled to the headroom, with the Intro floor for Density (spec
	/// §9.4). Complexity and Motion are never moved (spec §3.3).
	[[nodiscard]] int32_t offset(const Genre& genre, Macro macro, int32_t centre) const;
	/// In a Groove: whether it ends on its own at the end of the phrase it is in, or at the bar line in it where a
	/// short Build starts, with Motion at `motion`. Never while held, or at Motion 0 (spec §9.3).
	[[nodiscard]] bool grooveEndsInPhrase(int32_t motion) const;
	/// The lowest Density gate in the roster: where Density starts in an Intro and ends in an Outro (spec §9.4).
	[[nodiscard]] int32_t introFloor() const { return introFloor_; }

	// ---- Force and hold (spec §9.6) ----

	/// While held, the Arc doesn't move on its own: an Intro, Groove or Break plays on. A Build still lands its Drop,
	/// and the hold applies in the Groove it lands on.
	void hold(bool on) { held_ = on; }
	[[nodiscard]] bool held() const { return held_; }
	/// Whether the step after this one is where a forced `kind` starts: the next phrase line, or for a Build, the bar
	/// line that lets it end on a phrase line.
	[[nodiscard]] bool forceLandsNext(SectionKind kind, int32_t buildSteps) const {
		return stepsToStart(kind, buildSteps) == 0;
	}
	/// Steps from the next step to where a forced `kind` starts, 0 if it starts there.
	[[nodiscard]] int32_t stepsToStart(SectionKind kind, int32_t buildSteps) const {
		return stepsToStartFrom(kind, buildSteps, nextStepInPhrase());
	}
	/// Steps from the next step to the next phrase line, or bar line, at or after it.
	[[nodiscard]] int32_t stepsToPhraseLine() const { return (phraseSteps_ - nextStepInPhrase()) % phraseSteps_; }
	[[nodiscard]] int32_t stepsToBarLine() const {
		return (kStepsPerBar - nextStepInPhrase() % kStepsPerBar) % kStepsPerBar;
	}
	/// `kind` starts on the next step entered, a Build lasting `buildSteps`.
	void force(SectionKind kind, int32_t buildSteps);
	/// Whether a Build of `steps` lands its Drop on a phrase line: whole bars that divide the phrase or are a whole
	/// number of phrases (spec §9.3).
	[[nodiscard]] bool buildFits(int32_t steps) const {
		return steps > 0 && steps % kStepsPerBar == 0 && (phraseSteps_ % steps == 0 || steps % phraseSteps_ == 0);
	}
	/// The length of the next Build to start, in steps: drawn (seeded) from the genre's list (spec §9.3).
	[[nodiscard]] int32_t nextBuildSteps(const Genre& genre, uint32_t seed) const;

private:
	void begin(const Genre& genre, uint32_t seed, SectionKind kind, const Macros& centre, int32_t buildSteps);
	/// Steps from `stepInPhrase` to where a Section of `kind` (a Build lasting `buildSteps`) may start: a phrase line,
	/// or for a Build, where it ends on one (spec §9.3, §9.6). The one placement rule for the Arc's own moves, for
	/// forces and for counting down to them.
	[[nodiscard]] int32_t stepsToStartFrom(SectionKind kind, int32_t buildSteps, int32_t stepInPhrase) const;
	[[nodiscard]] bool startsOnLine(SectionKind kind, int32_t buildSteps, int32_t stepInPhrase) const {
		return stepsToStartFrom(kind, buildSteps, stepInPhrase) == 0;
	}
	/// Whether the Section ends before a step at `stepInPhrase`, the `stepInSection`th of the Section.
	[[nodiscard]] bool endsBefore(int32_t stepInPhrase, int32_t stepInSection, int32_t motion) const;
	[[nodiscard]] SectionKind naturalNext(const Genre& genre, uint32_t seed) const;
	[[nodiscard]] int32_t buildSteps(const Genre& genre, uint32_t seed, uint16_t occurrence) const;
	/// A raw offset as it plays around `centre`: each side scaled down only as far as it needs to fit (spec §9.4).
	[[nodiscard]] int32_t headroom(int32_t offset, int32_t centre, bool energy) const;

	SectionKind kind_ = SectionKind::Intro;
	SectionKind next_ = SectionKind::Groove; ///< Where the Arc goes on its own when this Section ends.
	SectionKind first_ = SectionKind::Intro;
	uint16_t occurrence_ = 0;
	uint16_t started_[kNumSectionKinds] = {}; ///< Sections of each kind begun so far.
	int32_t phraseSteps_ = kStepsPerBar * 8;
	int32_t step_ = -1;
	int32_t stepInSection_ = 0;
	int32_t lengthSteps_ = 0; ///< See sectionSteps().
	int32_t nextBuild_ = 0;   ///< The Build that follows on its own, in steps, or 0.
	int32_t grooveBase_ = 1;  ///< A Groove's base length `b`, in phrases at Motion 25.
	int32_t stepInPhrase_ = 0;
	int32_t phraseInSection_ = 0;
	bool afterDrop_ = false; ///< This Groove started on a Drop.
	bool held_ = false;
	bool forced_ = false; ///< forcedKind_ starts on the next step.
	SectionKind forcedKind_ = SectionKind::Groove;
	int32_t forcedBuild_ = 0;
	int16_t buildFrom_[2] = {};   ///< Energy and Density offsets as they played when the Build started.
	int16_t maxPositive_[2] = {}; ///< Energy's and Density's largest positive offset in the section rules.
	int16_t maxNegative_[2] = {}; ///< Their deepest negative offset, as a magnitude.
	int16_t introFloor_ = 0;
};

} // namespace deluge::generative
