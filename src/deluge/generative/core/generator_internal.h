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

#pragma once

// The rules and draw keys the Generator's files share (generator.cpp and generator_*.cpp, one per area): the Arc's
// rules on a step, which the play path and the Arc plan both read, and the draw keys every area positions its draws
// with. Only those files include this.
//
// The functions are defined in generator_internal.cpp, not inline here: an `inline` changes what LTO inlines into the
// step, and moved several of the step's functions in or out of line (implementation-notes/runner.md, "The step's
// cost in the tick"). The constexpr ones are inline as they were when they were file-local.

#include "generative/core/draw.h"
#include "generative/core/generator.h"

namespace deluge::generative::generator_internal {

/// `macro` on the Arc's step: the Recipe's `centre` plus the Section's offset, within 0..kMacroFull (spec §9.4). The
/// play path (Generator::effectiveMacro()) and the Arc plan, on its copy of the Arc, both read it here.
int32_t effectiveOn(const Genre& genre, const Arc& arc, const Macros& centre, Macro macro);

/// A draw key for a part, or one of its lanes (Lane::None for a part-wide draw), before its position words.
DrawKey partKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose);

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

/// The Build roll's stage on `arc`'s step, or None outside a Build, before the roll starts and in the Gap (spec §8.3).
/// The roll climbs through its stages towards the Gap. The start and the stage fractions are measured from the Drop,
/// so with Build lengths powers of two every change lands on the grid, and the Gap then takes the end. Complexity sets
/// only how early it starts: one that starts late joins at the stage already reached. It starts no later than the last
/// grid step before the Gap, so a short Build with a Gap still rolls. sectionRules() plays it, and the Arc plan counts
/// the roll lane as sounding while it rolls.
RollStage rollStageOn(const ArcSpec& rules, const Arc& arc, int32_t complexity);

/// What still plays in a Section of `kind` by its keeps (spec §9.5): Break or Build keeps, or nullptr (everything).
const PartsAndLanes* keepsFor(const ArcSpec& rules, SectionKind kind);

/// Whether a Section's keeps let a part's notes on `lane` (Lane::None for a pitched part) play. FX lanes are the
/// section rules' own, and the roll lane always plays in a Build.
bool kept(const Genre& genre, const PartsAndLanes* keeps, SectionKind kind, int32_t part, Lane lane);

/// `key` positioned on a Groove and a phrase within it: word 0 and word 1. Word 2 is the purpose's.
DrawKey onPhrase(DrawKey key, uint16_t groove, uint16_t phrase, uint32_t word2 = 0);

/// `key` positioned on a pick: its Groove in word 0, its phrase in word 1's top half and its pool entry in word 2's
/// top byte. The purpose fills the rest: an `index` in word 1's low half (passes since a roll, a rule) and a `step`
/// in word 2's low 24 bits (a step within the pattern or line, with any field in its low byte).
DrawKey onPick(DrawKey key, const PatternState& state, uint16_t index, uint32_t step);

/// A position word holding `value` with a draw's field in its low byte, for purposes that draw several fields.
constexpr uint32_t fieldWord(uint32_t value, int32_t field) {
	return value << 8 | static_cast<uint32_t>(field);
}

/// `key` for another field of the same draw: the field is position[2]'s low byte.
DrawKey withField(DrawKey key, int32_t field);

} // namespace deluge::generative::generator_internal
