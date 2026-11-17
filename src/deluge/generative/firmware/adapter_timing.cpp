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

#if ENABLE_SYSEX_TEST

#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/perform_adapter.h"
#include "memory/general_memory_allocator.h"

namespace deluge::generative::firmware {

unsigned long ticksToNs(uint32_t ticks) {
	return static_cast<unsigned long>(uint64_t{ticks} * 1'000'000'000 / DELUGE_CLOCKS_PER);
}

DecisionTiming decisionTiming;
ControlTiming controlTiming;

const char* culledPartName(const Sound* sound) {
	const Genre* genre = performAdapter.steering().rosterGenre(); // sink.roster's, during a transition too
	const OutputSink& sink = performAdapter.output();
	for (int32_t p = 0; genre != nullptr && p < partCount(*genre); p++) {
		if (soundsOf(sink.roster.parts[p]).contains(sound)) {
			return genre->parts[p].name;
		}
	}
	for (const PartOutput& part : sink.outgoing.parts) {
		if (soundsOf(part).contains(sound)) {
			return "outgoing";
		}
	}
	return "other";
}

/// Free bytes on the heaps Perform uses, for the leave check (spec §13.8: memory after leaving matches memory before
/// entry) and a sound re-roll's. The stealable region holds the sample cache, so it's reported apart.
void reportFreeMemory(const char* when) {
	auto& allocator = GeneralMemoryAllocator::get();
	auto freeIn = [&](int32_t region) {
		uint32_t total = 0;
		auto& spaces = allocator.regions[region].emptySpaces;
		for (int32_t i = 0; i < spaces.getNumElements(); i++) {
			total += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(i))->length;
		}
		return static_cast<unsigned long>(total);
	};
	TEST_EVENT("gen_mem %s internal=%lu external=%lu stealable=%lu", when,
	           freeIn(MEMORY_REGION_INTERNAL) + freeIn(MEMORY_REGION_INTERNAL_SMALL),
	           freeIn(MEMORY_REGION_EXTERNAL) + freeIn(MEMORY_REGION_EXTERNAL_SMALL), freeIn(MEMORY_REGION_STEALABLE));
}

unsigned long msSince(uint32_t start) {
	return static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000));
}

} // namespace deluge::generative::firmware

#endif // ENABLE_SYSEX_TEST
