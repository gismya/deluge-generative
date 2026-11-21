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

// The punch-ins on, as the Generator keeps them (spec §8.9).

#pragma once

#include "generative/core/genre.h"
#include "generative/core/steering.h"
#include <cstdint>

namespace deluge::generative {

/// The tier 2 punch-ins on (spec §8.9): performance state, like mutes, so a start begins with none and the driver sends
/// the ones still on again (PerformSteering::instanceStarts()).
struct PunchIns {
	uint16_t lanes = 0;  ///< laneBit() of each Drums lane a LaneMute silences.
	uint8_t effects = 0; ///< bit() of each other PunchIn that is on.
	bool pumped = false; ///< What the last Pump event said: on or off.

	static constexpr uint8_t bit(PunchIn punch) { return static_cast<uint8_t>(1U << static_cast<uint8_t>(punch)); }
	[[nodiscard]] constexpr bool any() const { return lanes != 0 || effects != 0; }
	[[nodiscard]] constexpr bool on(PunchIn punch) const { return (effects & bit(punch)) != 0; }
	/// Whether a LaneMute silences `lane`, a Drums lane.
	[[nodiscard]] constexpr bool muted(Lane lane) const {
		return lane != Lane::None && !isFxLane(lane) && (lanes & laneBit(lane)) != 0;
	}

	/// Turns on or off what `command`, a SetPunch, names. @return false if it names no punch-in, a LaneMute names no
	/// Drums lane or its value isn't 0 or 1: it isn't logged.
	constexpr bool set(const Command& command) {
		if (command.target >= kNumPunchIns || (command.value != 0 && command.value != 1)) {
			return false;
		}
		const bool turnOn = command.value != 0;
		const auto punch = static_cast<PunchIn>(command.target);
		if (punch == PunchIn::LaneMute) {
			if (command.lane == Lane::None || isFxLane(command.lane)) {
				return false;
			}
			const uint16_t laneMask = laneBit(command.lane);
			lanes = static_cast<uint16_t>(turnOn ? lanes | laneMask : lanes & ~laneMask);
			return true;
		}
		effects = static_cast<uint8_t>(turnOn ? effects | bit(punch) : effects & ~bit(punch));
		return true;
	}
};
static_assert(kNumPunchIns <= 8, "PunchIns::effects holds a bit for each");

/// Whether the Flam punch-in flams `lane`'s hits on `drums`, the Drums part (spec §8.9): the backbeat lanes (Snare,
/// Clap, Rim) where the part has any of them, else every lane but the Kick.
constexpr bool flams(const PartSpec& drums, Lane lane) {
	const bool backbeat = lane == Lane::Snare || lane == Lane::Clap || lane == Lane::Rim;
	if (backbeat) {
		return true;
	}
	const bool hasBackbeat = laneIndexOf(drums, Lane::Snare) >= 0 || laneIndexOf(drums, Lane::Clap) >= 0
	                         || laneIndexOf(drums, Lane::Rim) >= 0;
	return !hasBackbeat && lane != Lane::Kick && lane != Lane::None && !isFxLane(lane);
}

} // namespace deluge::generative
