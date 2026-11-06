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

// The screen over a modal runner job (spec §12.10): Perform entry, and tearing Perform's song down on leave. The
// current song keeps playing untouched with its pads and knobs ignored, the stock working animation shows, and only
// BACK acts: it cancels the job at once, if the job can still be cancelled. Every handler acts at once, inside a card
// wait too, so none ever returns REMIND_ME_OUTSIDE_CARD_ROUTINE.

#pragma once

#include "gui/ui/ui.h"

class GenerativeModalUI final : public UI {
public:
	GenerativeModalUI();

	/// Opens over the current UI, with the working animation, for the job about to be submitted. With `backCancels`,
	/// BACK cancels the runner's active job, which is then that job.
	void open(bool backCancels);
	/// The job ended: closes, if BACK hasn't already.
	void jobEnded();

	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override { return ActionResult::DEALT_WITH; }
	ActionResult horizontalEncoderAction(int32_t offset) override { return ActionResult::DEALT_WITH; }
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override { return ActionResult::DEALT_WITH; }
	void selectEncoderAction(int8_t offset) override {}
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override {}
	void modButtonAction(uint8_t whichButton, bool on) override {}
	void modEncoderButtonAction(uint8_t whichModEncoder, bool on) override {}
	ActionResult exitUI() override { return ActionResult::DEALT_WITH; }
	bool canSeeViewUnderneath() override { return true; }
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override {}
	UIType getUIType() override { return UIType::GENERATIVE_MODAL; }

private:
	void dismiss();
	bool backCancels_ = false;
};

extern GenerativeModalUI generativeModalUI;
