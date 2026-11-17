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

#include "generative/core/generator_internal.h"
#include <algorithm>

namespace deluge::generative::generator_internal {

GEN_STEP_CODE int32_t effectiveOn(const Genre& genre, const Arc& arc, const Macros& centre, Macro macro) {
	return std::clamp<int32_t>(centre[macro] + arc.offset(genre, macro, centre[macro]), 0, kMacroFull);
}

DrawKey partKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose) {
	DrawKey key;
	key.seed = seed;
	key.part = part;
	key.lane = static_cast<uint8_t>(lane);
	key.take = take;
	key.purpose = purpose;
	return key;
}

GEN_STEP_CODE RollStage rollStageOn(const ArcSpec& rules, const Arc& arc, int32_t complexity) {
	if (arc.section() != SectionKind::Build || rules.roll.lane == Lane::None) {
		return RollStage::None;
	}
	const int32_t toDrop = arc.stepsToDrop();
	const int32_t gap = gapSteps(rules.gap);
	const int32_t length = arc.sectionSteps();
	const int32_t share =
	    kRollStartAtZero + (1000 - kRollStartAtZero) * std::clamp<int32_t>(complexity, 0, kMacroFull) / kMacroFull;
	const int32_t latest = gap + rollGrid(rollStageAt(rules.roll, gap + 1, length));
	const int32_t start = std::max(length * share / 1000, latest);
	return toDrop > gap && toDrop <= start ? rollStageAt(rules.roll, toDrop, length) : RollStage::None;
}

const PartsAndLanes* keepsFor(const ArcSpec& rules, SectionKind kind) {
	if (kind == SectionKind::Break) {
		return &rules.breakKeeps;
	}
	if (kind == SectionKind::Build) {
		return rules.buildKeeps == kSameAsBreakKeeps ? &rules.breakKeeps : &rules.buildKeeps;
	}
	return nullptr;
}

GEN_STEP_CODE bool kept(const Genre& genre, const PartsAndLanes* keeps, SectionKind kind, int32_t part, Lane lane) {
	if (keeps == nullptr || isFxLane(lane)
	    || (lane != Lane::None && lane == genre.arc.roll.lane && kind == SectionKind::Build)) {
		return true;
	}
	return keeps->has(part, lane);
}

DrawKey onPhrase(DrawKey key, uint16_t groove, uint16_t phrase, uint32_t word2) {
	key.position[0] = groove;
	key.position[1] = phrase;
	key.position[2] = word2;
	return key;
}

GEN_STEP_CODE DrawKey onPick(DrawKey key, const PatternState& state, uint16_t index, uint32_t step) {
	key.position[0] = state.pick.groove;
	key.position[1] = static_cast<uint32_t>(state.pick.phrase) << 16 | index;
	key.position[2] = static_cast<uint32_t>(static_cast<uint8_t>(state.entry)) << 24 | (step & 0xFFFFFFU);
	return key;
}

DrawKey withField(DrawKey key, int32_t field) {
	key.position[2] = fieldWord(key.position[2] >> 8, field);
	return key;
}

} // namespace deluge::generative::generator_internal
