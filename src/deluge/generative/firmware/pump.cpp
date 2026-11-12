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

#include "generative/firmware/pump.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"

namespace deluge::generative::firmware {

void Pump::service(ReleaseUi release) {
	soundSwaps.service();
	transitions.service();
	retirements.service(release);
}

} // namespace deluge::generative::firmware
