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

// Free memory by region: the Compose build's size and SDRAM reserve checks (spec §15.9) and the gen_mem test events.

#pragma once

#include "memory/general_memory_allocator.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// Free bytes in `region`'s empty spaces, counted until `enough`: a check that only needs that much stops there.
inline uint32_t freeInRegion(int32_t region, uint32_t enough = UINT32_MAX) {
	auto& spaces = GeneralMemoryAllocator::get().regions[region].emptySpaces;
	uint32_t total = 0;
	for (int32_t i = 0; i < spaces.getNumElements() && total < enough; i++) {
		total += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(i))->length;
	}
	return total;
}

} // namespace deluge::generative::firmware
