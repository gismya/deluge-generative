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

// The Recipe load browser (spec §16.5): LOAD in Perform or on the Compose screen, and FILE... in the Generative menu's
// source lists (§14.1), open a stock-style load browser on RECIPES/. The QWERTY shows on entry and hides once the
// player scrolls, so the pads underneath show again; a pad press brings it back. SELECT (or LOAD) on a file reads it:
// an unreadable file or an unknown genre is refused and the browser stays; otherwise it closes and the file loads. In
// Perform while playing that is a transition, confirmed with its style (spec §9.7); stopped it rebuilds at once; on the
// Compose screen or COMPOSE FROM it replaces the Current Recipe and the screen shows it; in PERFORM FROM it asks
// DISCARD SONG?, whose OK enters Perform on it. BACK in RECIPES/ leaves with nothing changed. Over Perform, Perform
// plays on: its frame runs and the sidebar stays its own, as under the preset browser.

#pragma once

#include "gui/ui/load/load_ui.h"

class LoadRecipeUI final : public LoadUI {
public:
	LoadRecipeUI() = default;

	/// LOAD in Perform or on the Compose screen. Refused with BUSY while the runner is busy or a card access runs
	/// (spec §12.10); a card error shows the stock error popup. False if it didn't open.
	bool open(bool inCardRoutine);

	bool opened() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult timerCallback() override;
	/// The pads underneath show once the QWERTY has hidden.
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr,
	                    bool drawUndefinedArea = true) override {
		return qwertyVisible;
	}
	/// Over Perform the sidebar is transparent: Perform's shows, live.
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void graphicsRoutine() override;
	UIType getUIType() override { return UIType::GENERATIVE_LOAD_RECIPE; }

protected:
	void enterKeyPress() override;
	void currentFileChanged(int32_t movementDirection) override;
	void folderContentsReady(int32_t entryDirection) override {}

private:
	/// Hides the QWERTY, so the pads underneath show (spec §16.5).
	void hideQwerty();
	/// Closes the browser, its LED back off.
	void closeBrowser();
};

extern LoadRecipeUI loadRecipeUI;
