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

// The stock working animation over the Perform page while a preload runs (spec §12.10): a sound re-roll's, or a
// transition's incoming roster. One rule, here, for every press that may start one and for a sound preload's end:
// the animation is up while a preload runs. A press puts it up before it submits (a job whose task can't start ends
// inside the submit) and takes it down after only if no preload runs then; a sound preload's end takes it down only
// if no transition preload runs on (a confirm cancels a running sound preload and starts its own).
//
// A transition's own ends (Transitions::adoptIncoming(), preloadEnded(), cancelPreload()) take it down directly: the
// lifecycle calls them only for its newest request, and no sound preload can run then, since a sound press is refused
// while a transition is Preloading (spec §12.9).

#pragma once

#include <type_traits>

namespace deluge::generative::firmware {

/// Whether a preload runs: a sound's job, or a transition in Preloading (its job running, or waiting in the runner's
/// next slot while a sound preload it cancelled unwinds).
[[nodiscard]] bool preloadRunning();

/// Whether the working animation is up (test events).
[[nodiscard]] bool workingAnimationShown();

namespace working_animation_detail {
void beforePress();
void afterPress();
} // namespace working_animation_detail

/// Runs `press` (a NEW SOUND press, a transition's confirm) under the working animation, and returns what it returns.
template <typename Press>
std::invoke_result_t<Press> pressShowingWork(Press press) {
	working_animation_detail::beforePress();
	const auto result = press();
	working_animation_detail::afterPress();
	return result;
}

/// In a sound preload job's finished(): the animation comes down unless a transition's preload runs on.
void soundPreloadEnded();

} // namespace deluge::generative::firmware
