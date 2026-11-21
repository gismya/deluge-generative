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

class InstrumentClip;
class Output;

namespace deluge::generative::firmware {

class SidechainPump {
public:
	/// Ducks every part of `roster` but the Drums, each Output once: a part whose Output is new since (a sound swap, a
	/// transition's roster) ducks too.
	void duck(const Roster& roster);
	/// Every part ducked goes back to what it had, on the Output it ducked, so it reaches an outgoing roster too, and a
	/// sidechain → volume cable the pump made is deleted: a Pump off, a stop, and each instance start (the steering
	/// sends a pump still on again from there, spec §8.9).
	void release();
	/// Perform's song is gone, its Outputs with it.
	void forget() { *this = {}; }
	[[nodiscard]] bool on() const { return on_; }

private:
	/// What a ducked part had before: the Output and clip it ducked, so a part swapped since isn't touched, the value,
	/// and whether the pump made the cable.
	struct Saved {
		Output* output = nullptr;
		InstrumentClip* clip = nullptr;
		int32_t value = 0;
		bool made = false;
	};
	void report(bool on);
	Saved saved_[kMaxParts] = {};
	bool on_ = false;
};

extern SidechainPump sidechainPump;

} // namespace deluge::generative::firmware
