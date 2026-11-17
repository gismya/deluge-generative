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

// Perform's preset browser (spec §13.5, §10.7): SYNTH on a focused synth part opens it on the part's current pick, over
// every preset on the card. It is the stock browser's file list, folder navigation and QWERTY search, but scrolling
// loads nothing: the part plays on with its sound. SELECT on a preset hands its path to Perform, which preloads it as a
// re-roll and swaps it in at the next phrase line; BACK at the top folder closes it with nothing changed. Playback,
// the gold knobs and the sidebar stay Perform's meanwhile (End aside, as under every overlay), and Perform's frame
// runs underneath, so swaps, transitions and retirements are serviced.

#pragma once

#include "gui/ui/browser/slot_browser.h"

class PerformPresetBrowser final : public SlotBrowser {
public:
	PerformPresetBrowser() = default;

	/// Opens over Perform on `part`'s pick, the Current Recipe's (SYNTHS/ when it names no folder). False if the card
	/// or the folder can't be read: the stock error shows, and Perform stays on top.
	bool open(int32_t part);

	bool opened() override;
	void focusRegained() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult timerCallback() override;
	void graphicsRoutine() override;
	UIType getUIType() override { return UIType::GENERATIVE_PRESET_BROWSER; }

protected:
	void enterKeyPress() override;
	void currentFileChanged(int32_t movementDirection) override;
	void folderContentsReady(int32_t entryDirection) override;
	void exitAction() override;

private:
	/// Closes the browser, its LED back off.
	void closeBrowser();

	int32_t part_ = -1;
};

extern PerformPresetBrowser performPresetBrowser;
