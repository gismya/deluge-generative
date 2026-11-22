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

#include "generative/core/draw.h"

namespace deluge::generative {

GEN_STEP_CODE uint32_t draw(const DrawKey& key) {
	uint32_t h = detail::mix(key.seed ^ 0x9e3779b9U);
	h = detail::mix(h
	                ^ (static_cast<uint32_t>(key.part) | static_cast<uint32_t>(key.lane) << 8
	                   | static_cast<uint32_t>(key.take) << 16 | static_cast<uint32_t>(key.purpose) << 24));
	for (uint32_t word : key.position) {
		h = detail::mix(h ^ word);
	}
	return h;
}

GEN_STEP_CODE DrawKey partKey(uint32_t seed, uint8_t part, Lane lane, uint8_t take, Purpose purpose) {
	DrawKey key;
	key.seed = seed;
	key.part = part;
	key.lane = static_cast<uint8_t>(lane);
	key.take = take;
	key.purpose = purpose;
	return key;
}

GEN_STEP_CODE DrawKey onPhrase(DrawKey key, uint16_t groove, uint16_t phrase, uint32_t word2) {
	key.position[0] = groove;
	key.position[1] = phrase;
	key.position[2] = word2;
	return key;
}

GEN_STEP_CODE DrawKey withField(DrawKey key, int32_t field) {
	key.position[2] = fieldWord(key.position[2] >> 8, field);
	return key;
}

} // namespace deluge::generative
