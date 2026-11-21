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

// The Drums and FX lanes' names (spec §5.4), as their Kit rows are named and the Compose screen's lane list shows
// them: `KICK`, `CLOSED HAT`, unique within a Kit.

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

constexpr const char* laneName(Lane lane) {
	constexpr const char* kNames[kNumLanes] = {"KICK",     "SNARE", "CLAP",  "RIM",    "CLOSED HAT",
	                                           "OPEN HAT", "RIDE",  "CRASH", "TOM",    "PERC 1",
	                                           "PERC 2",   "RISER", "FALL",  "IMPACT", "SIREN"};
	const auto index = static_cast<int32_t>(lane);
	return index < kNumLanes ? kNames[index] : "?";
}

} // namespace deluge::generative
