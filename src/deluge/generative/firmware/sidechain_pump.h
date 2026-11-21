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

// The Pump punch-in's sound (spec §8.9): while on, every part but the Drums ducks under the Kick, which sends to the
// sidechain at full (spec §10.4). A synth part through its sidechain → volume cable (stock's VOLUME DUCKING), a Kit
// part through its kit-wide sidechain level. The core sends Event::pump(); the sink plays it here.

#pragma once

#include "generative/core/types.h"
#include "generative/firmware/perform_song.h"
#include <cstdint>

class Output;

namespace deluge::generative::firmware {

class SidechainPump {
public:
	/// Ducks (`on`) every part of `roster` but the Drums, or lets each go back to what it had. Ducking again ducks only
	/// a part whose Output is new since (a sound swap, a transition's roster).
	void set(const Roster& roster, bool on);
	/// A stop: every part ducked lets go, so the next start's pump (the steering sends it again) ducks from there.
	void release(const Roster& roster) { set(roster, false); }
	/// Perform's song is gone, its Outputs with it.
	void forget() { *this = {}; }
	[[nodiscard]] bool on() const { return on_; }

private:
	/// What a ducked part had before: its Output, so a part swapped since isn't touched, and the value.
	struct Saved {
		Output* output = nullptr;
		int32_t value = 0;
	};
	Saved saved_[kMaxParts] = {};
	bool on_ = false;
};

extern SidechainPump sidechainPump;

} // namespace deluge::generative::firmware
