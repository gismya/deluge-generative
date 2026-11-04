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

// Generative mode POC: the Perform view. SHIFT+SONG in session view enters it, BACK returns to session view.
// The upper gold knob sets Energy, the lower one Density. PLAY, SHIFT and RECORD pass through.

#pragma once

#include "definitions_cxx.hpp"
#include "gui/ui/root_ui.h"
#include "hid/button.h"

class GenerativePerformView final : public RootUI {
public:
	GenerativePerformView() = default;

	/// Builds the Perform roster into the current song and makes this the root UI. Shows an error and stays put
	/// if the roster can't be built. Call outside the card routine.
	void enterFromSession();

	bool opened() override;
	void focusRegained() override;
	void graphicsRoutine() override;

	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override { return ActionResult::DEALT_WITH; }
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override;
	void modButtonAction(uint8_t whichButton, bool on) override {}
	void modEncoderButtonAction(uint8_t whichModEncoder, bool on) override {}

	void notifyPlaybackBegun() override;
	void playbackEnded() override;

	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;

	UIType getUIType() override { return UIType::GENERATIVE_PERFORM; }
	UIModControllableContext getUIModControllableContext() override { return UIModControllableContext::NONE; }
	[[nodiscard]] bool supportsTriplets() const override { return false; }

private:
	void exitToSession();
	void updateKnobIndicators();
	void updateStepDisplay(int32_t step);

	int32_t lastShownStep_ = -1;
};

extern GenerativePerformView generativePerformView;
