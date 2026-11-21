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
#include "generative/firmware/capture_job.h"
#include "generative/firmware/compose_job.h"
#include "generative/firmware/runner.h"
#include "gui/l10n/l10n.h"
#include "hid/button.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include <algorithm>

using deluge::generative::firmware::runner;

GenerativeModalUI generativeModalUI{};

GenerativeModalUI::GenerativeModalUI() {
	oledShowsUIUnderneath = true; // the song's own screen stays up, with the animation over it
}

void GenerativeModalUI::open(const deluge::generative::Job& job) {
	job_ = &job;
	composing_ = false;
	oledShowsUIUnderneath = true;
	openUI(this);
	display->displayLoadingAnimationText(deluge::l10n::get(deluge::l10n::String::STRING_FOR_LOADING));
}

void GenerativeModalUI::openCapture(const deluge::generative::Job& job) {
	openComposing(job);
	capturing_ = true;
	renderUIsForOled();
}

void GenerativeModalUI::openComposing(const deluge::generative::Job& job) {
	job_ = &job;
	composing_ = true;
	capturing_ = false;
	oledShowsUIUnderneath = false; // the screen is COMPOSING's own
	openUI(this);
	renderUIsForOled();
}

void GenerativeModalUI::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	if (!composing_) {
		return;
	}
	using deluge::l10n::String;
	deluge::generative::firmware::ComposeProgress progress;
	if (capturing_) {
		// The song's title, and the bars replayed of its piece's span (spec §17.7).
		deluge::generative::firmware::CaptureScreen screen;
		deluge::generative::firmware::captureScreen(screen);
		canvas.drawScreenTitle(screen.title);
		progress = {screen.bars, screen.expected};
	}
	else {
		canvas.drawScreenTitle(deluge::l10n::get(String::STRING_FOR_COMPOSING));
		// Bars rendered against those expected at the target plus one Outro phrase: it may jump at the end (spec
		// §14.5).
		progress = deluge::generative::firmware::composeProgress();
	}
	constexpr int32_t kLeft = 4;
	constexpr int32_t kRight = OLED_MAIN_WIDTH_PIXELS - 5;
	constexpr int32_t kTop = OLED_MAIN_TOPMOST_PIXEL + 17;
	constexpr int32_t kBottom = kTop + 6;
	canvas.drawRectangle(kLeft, kTop, kRight, kBottom);
	if (progress.expected > 0) {
		const int32_t width = kRight - kLeft - 3;
		const int32_t filled = std::min(width, width * progress.bars / progress.expected);
		if (filled > 0) {
			canvas.invertArea(kLeft + 2, filled, kTop + 2, kBottom - 2);
		}
	}
	canvas.drawStringCentred(
	    deluge::l10n::get(capturing_ ? String::STRING_FOR_BACK_STOPS : String::STRING_FOR_BACK_CANCELS), kBottom + 5,
	    kTextSpacingX, kTextSpacingY);
}

void GenerativeModalUI::jobEnded() {
	if (isUIOpen(this)) {
		dismiss();
		return;
	}
	// BACK dismissed it already, or the job replaced the UI stack (a finished Compose build's song).
	job_ = nullptr;
	composing_ = false;
	capturing_ = false;
}

void GenerativeModalUI::dismiss() {
	job_ = nullptr;
	if (!composing_) {
		display->removeLoadingAnimation();
	}
	composing_ = false;
	capturing_ = false;
	closeUI(this);
}

ActionResult GenerativeModalUI::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (b == BACK) {
		// The screen goes back at once; the job unwinds behind it (spec §12.10).
		if (on && runner.active() == job_ && runner.cancel()) {
			dismiss();
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == SHIFT) {
		return ActionResult::NOT_DEALT_WITH; // SHIFT's held state stays right; it changes nothing on its own
	}
	return ActionResult::DEALT_WITH;
}
