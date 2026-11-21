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

#include "generative/firmware/working_animation.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/restart_began.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"

namespace deluge::generative::firmware {

bool preloadRunning() {
	return transitions.preloading() || soundSwaps.preloadRunning() || restartBegan.preloading();
}

bool workingAnimationShown() {
	return display->haveOLED() && hid::display::OLED::isWorkingAnimationPresent();
}

namespace {

/// Takes the working animation down, unless Perform is being left: then only LeaveJob::finished() takes it down, after
/// a preload the leave cancelled has ended (working_animation.h).
void removeUnlessLeaving() {
	if (!performAdapter.leaving()) {
		display->removeWorkingAnimation();
	}
}

} // namespace

namespace working_animation_detail {

void beforePress() {
	// A running preload has it up already.
	if (!preloadRunning()) {
		display->displayLoadingAnimationText(l10n::get(l10n::String::STRING_FOR_LOADING));
	}
}

void afterPress() {
	// The press's own preload, or one it was refused beside, keeps it; that preload's end takes it down. Leaving keeps
	// it too, as the preload ends do (unreachable today: the leave modal takes every press).
	if (!preloadRunning()) {
		removeUnlessLeaving();
	}
}

} // namespace working_animation_detail

void soundPreloadEnded() {
	// The sound job is still the runner's active one here, so only a transition's preload counts.
	if (!transitions.preloading()) {
		removeUnlessLeaving();
	}
}

void transitionPreloadEnded() {
	removeUnlessLeaving();
}

} // namespace deluge::generative::firmware
