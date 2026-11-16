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

#include "generative/perform/midi_in.h"

namespace deluge::generative::perform {

namespace {
/// The CC that steers Energy; Density, Complexity and Motion follow it (spec §13.7).
constexpr int32_t kFirstMacroCC = 114;
constexpr int32_t kMaxCCValue = 127;
} // namespace

std::optional<Macro> macroForCC(int32_t cc) {
	if (cc < kFirstMacroCC || cc >= kFirstMacroCC + kNumMacros) {
		return std::nullopt;
	}
	return static_cast<Macro>(cc - kFirstMacroCC);
}

int32_t macroCC(Macro macro) {
	return kFirstMacroCC + static_cast<int32_t>(macro);
}

int32_t macroUnitsFromCC(int32_t value) {
	return (value * kMacroFull + kMaxCCValue / 2) / kMaxCCValue;
}

int32_t macroCCValue(int32_t units) {
	return (units * kMaxCCValue + kMacroFull / 2) / kMacroFull;
}

Key transposedKey(Key ahead, int32_t note) {
	return {static_cast<uint8_t>(note % 12), ahead.scale};
}

bool FeedbackFilter::changed(int32_t cc, int32_t value) {
	uint8_t& sent = sent_[cc];
	if (sent == value) {
		return false;
	}
	sent = static_cast<uint8_t>(value);
	return true;
}

void FeedbackFilter::forget() {
	sent_ = noneSent();
}

} // namespace deluge::generative::perform
