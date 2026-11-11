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

// Perform's pump: the one place its sound swaps, transitions and retiring Outputs are serviced in task context
// (spec §12.9). It also makes sure no UI reference points at an Output when that Output is handed to the free job: it
// asks the UI to let go of that Output, by identity, and an Output the UI still holds waits. So memory safety doesn't
// depend on the order the view's frame does its own things in.
//
// The pump runs from PerformView::graphicsRoutine(), so only while the view's frame runs. UI::graphicsRoutine() calls
// the root UI's only when the UI on top returns true from canSeeViewUnderneath(): under the sample browser while it
// shows a preview or its QWERTY keyboard, under the Slicer, and under the sample marker editor (it has its own
// graphicsRoutine()), nothing here is serviced. No fade starts at a cap, nothing is freed, a Queued transition isn't
// seen as Committed, and the swing interval doesn't change after a start step, until that UI closes. Nothing is freed
// during such a stall, so it is safe.

#pragma once

#include "generative/firmware/perform_song.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// The UI's answer when the pump asks it to let go of an Output.
enum class UiRelease : uint8_t {
	Released, ///< No UI reference is on the Output now.
	Held,     ///< A UI still uses it (a UI over a sound editor open on it): it waits, and the pump asks again.
};
/// Asks the UI to let go of `output`, which is due to be freed: the gold knobs, and what follows them, move off it,
/// and a sound editor open on it closes if it can. Task context.
using ReleaseUi = UiRelease (*)(const PartOutput& output);

class Pump {
public:
	/// Task context, a few times a second: services sound swaps, then transitions, then retiring Outputs. Nothing else
	/// calls those three. `release` is asked for each Output just before it goes to the free job.
	static void service(ReleaseUi release);
};

} // namespace deluge::generative::firmware
