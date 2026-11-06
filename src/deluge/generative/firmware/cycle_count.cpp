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

#include "generative/firmware/cycle_count.h"

extern "C" {
#include "RZA1/ostm/ostm.h"
}

namespace deluge::generative::firmware {

namespace {
/// OSTM channel 0. Not TIMER_SYSTEM_SUPERFAST: that names an MTU timer, which getTimerValue() doesn't read.
constexpr int kSchedulerTimer = 0;
} // namespace

uint32_t cycleCount() {
	return getTimerValue(kSchedulerTimer);
}

} // namespace deluge::generative::firmware
