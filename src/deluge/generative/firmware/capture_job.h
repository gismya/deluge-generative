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

// Capture's build on leave (spec §17.7): the pieces the leave menu kept, each replayed privately the way Compose builds
// (spec §17.1), swapped in stopped and saved to the card, one song in RAM at a time.

#pragma once

#include "generative/perform/capture.h"
#include <cstdint>

namespace deluge::generative {
class SteeringLog;
}

namespace deluge::generative::firmware {

/// Starts Capture as a modal runner job, under its own screen, behind the leave's teardown: the pieces of `log` that
/// `keep` keeps, built in set order. It frees the log (PerformAdapter::freeLog()) when it ends, and lands on the last
/// song it saved in arranger view, or the blank session.
void beginCapture(SteeringLog& log, perform::LeaveOption keep);

/// The Capture screen (spec §17.7): its title (`BUILDING 2 OF 3`), and the bars replayed of the piece's span.
struct CaptureScreen {
	char title[perform::kCaptureTitleChars + 1] = {};
	int32_t bars = 0;
	int32_t expected = 0;
};
void captureScreen(CaptureScreen& out);

} // namespace deluge::generative::firmware
