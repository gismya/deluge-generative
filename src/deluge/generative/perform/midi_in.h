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

// MIDI in Perform (spec §13.7): the macro CCs, TRANSPOSE's key and the live-value feedback filter. The firmware's
// MIDI routes (`generative/firmware/perform_midi.cpp`) call these.

#pragma once

#include "generative/core/pitch.h"
#include "generative/core/recipe.h"
#include <array>
#include <cstdint>
#include <optional>

namespace deluge::generative::perform {

/// The macro a CC on a MIDI-follow channel steers: CCs 114–117 are Energy, Density, Complexity and Motion.
std::optional<Macro> macroForCC(int32_t cc);
/// The CC that steers `macro`, and carries its feedback.
int32_t macroCC(Macro macro);
/// A macro CC's value, 0–127, in macro units over 0–50 points.
int32_t macroUnitsFromCC(int32_t value);
/// The CC value that sends `units` back: the value that set them, for any value macroUnitsFromCC() made.
int32_t macroCCValue(int32_t units);

/// The key a TRANSPOSE note queues: the note's pitch class, with the scale of the key `ahead`.
Key transposedKey(Key ahead, int32_t note);

/// Live-value feedback sends a CC only when its value moved since it was last sent (spec §13.7, H17).
class FeedbackFilter {
public:
	/// Whether `value` (a knob position offset to 0–128) differs from what `cc` last sent, which it then becomes.
	bool changed(int32_t cc, int32_t value);
	/// Forgets every value sent, so each one is sent again.
	void forget();

private:
	static constexpr uint8_t kNone = 0xFF;
	std::array<uint8_t, 128> sent_ = noneSent();
	static constexpr std::array<uint8_t, 128> noneSent() {
		std::array<uint8_t, 128> all{};
		all.fill(kNone);
		return all;
	}
};

} // namespace deluge::generative::perform
