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

// The Recipe file save UI (spec §16.4): QWERTY on the pads, then the browser on RECIPES/, as a stock save. It opens on
// the name SAVE suggests (spec §16.3); overwriting needs a pick and the stock confirm. Over Perform, Perform plays on:
// its frame runs and the sidebar stays its own, as under the preset browser.
#pragma once

#include "generative/perform/recipe_name.h"
#include "gui/ui/save/save_ui.h"

class SaveRecipeUI final : public SaveUI {
public:
	SaveRecipeUI();
	/// Opens over Perform or the Compose screen. Refused with BUSY while the runner is busy or a card access runs
	/// (spec §12.10); a card error shows the stock error popup. False if it didn't open.
	bool open(bool inCardRoutine);
	bool opened() override;
	bool performSave(bool mayOverwrite) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult timerCallback() override;
	/// Over Perform the sidebar is transparent: Perform's shows, live (spec §16.4).
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void graphicsRoutine() override;
	UIType getUIType() override { return UIType::GENERATIVE_SAVE_RECIPE; }

private:
	/// The name open() suggested, kept so a save under it keeps its stem (adoptSavedName()).
	deluge::generative::perform::RecipeName suggestion_;
	/// No letter variation was free: suggestion_ names a file that exists, and saving under it asks to overwrite.
	bool noneFree_ = false;
};

extern SaveRecipeUI saveRecipeUI;

/// SAVE in Perform or on the Compose screen (spec §16.4): a quick save in Perform while playing (unless `fullUI`, SHIFT
/// held), else the full save UI.
void recipeSavePress(bool fullUI, bool inCardRoutine);
