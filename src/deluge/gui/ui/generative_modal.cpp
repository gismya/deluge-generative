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

#include "gui/ui/generative_modal.h"
#include "generative/firmware/runner.h"
#include "gui/l10n/l10n.h"
#include "hid/button.h"
#include "hid/display/display.h"

using deluge::generative::firmware::runner;

GenerativeModalUI generativeModalUI{};

GenerativeModalUI::GenerativeModalUI() {
	oledShowsUIUnderneath = true; // the song's own screen stays up, with the animation over it
}

void GenerativeModalUI::open(bool backCancels) {
	backCancels_ = backCancels;
	openUI(this);
	display->displayLoadingAnimationText(deluge::l10n::get(deluge::l10n::String::STRING_FOR_LOADING));
}

void GenerativeModalUI::jobEnded() {
	if (isUIOpen(this)) {
		dismiss();
	}
}

void GenerativeModalUI::dismiss() {
	display->removeLoadingAnimation();
	closeUI(this);
}

ActionResult GenerativeModalUI::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (b == BACK) {
		// The screen goes back at once; the job unwinds behind it (spec §12.10).
		if (on && backCancels_ && runner.cancel()) {
			dismiss();
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == SHIFT) {
		return ActionResult::NOT_DEALT_WITH; // SHIFT's held state stays right; it changes nothing on its own
	}
	return ActionResult::DEALT_WITH;
}
