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

#include "gui/ui/save/save_recipe_ui.h"
#include "definitions_cxx.hpp"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/recipe_files.h"
#include "gui/context_menu/overwrite_file.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/perform_view.h"
#include "hid/button.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include "playback/playback_handler.h"
#include "util/d_string.h"
#include <cstring>

namespace gen = deluge::generative;
using gen::firmware::kRecipeFolder;

SaveRecipeUI saveRecipeUI{};

namespace {

void refusedBusy(const char* how) {
	display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_GENERATIVE_BUSY));
	TEST_EVENT("gen_recipe_save %s refused why=busy", how);
}

/// The quick save (spec §16.4): saves at once under the suggested name; a popup shows it, and the grid stays put.
void quickSave() {
	gen::perform::RecipeName saved;
	Error error = Error::NONE;
	switch (gen::firmware::quickSaveRecipe(saved, error)) {
	case gen::firmware::QuickSave::Saved:
		display->displayPopup(saved.text);
		TEST_EVENT("gen_recipe_save quick saved name=%s", saved.text);
		return;
	case gen::firmware::QuickSave::NoFreeName:
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_NO_FREE_NAME));
		TEST_EVENT("gen_recipe_save quick refused why=no_free_name");
		return;
	case gen::firmware::QuickSave::Failed:
		display->displayError(error);
		TEST_EVENT("gen_recipe_save quick failed error=%d", static_cast<int>(error));
		return;
	}
}

} // namespace

void recipeSavePress(bool fullUI, bool inCardRoutine) {
	const bool quick = rootUIIsGenerativePerform() && !fullUI && playbackHandler.playbackState != 0;
	if (!gen::firmware::recipeSaveAllowed(inCardRoutine)) {
		refusedBusy(quick ? "quick" : "full");
		return;
	}
	if (quick) {
		quickSave();
		return;
	}
	saveRecipeUI.open(inCardRoutine);
}

SaveRecipeUI::SaveRecipeUI() {
	filePrefix = "RECIPE";
}

bool SaveRecipeUI::open(bool inCardRoutine) {
	if (!gen::firmware::recipeSaveAllowed(inCardRoutine)) {
		refusedBusy("full");
		return false;
	}
	Error error = Error::NONE;
	noneFree_ = false;
	if (!gen::firmware::suggestRecipeName(suggestion_, error)) {
		if (error != Error::NONE) {
			display->displayError(error);
			TEST_EVENT("gen_recipe_save full failed error=%d", static_cast<int>(error));
			return false;
		}
		// No letter variation free: offer the remembered name, or the default one if none is remembered. Its file
		// exists, so saving under it asks to overwrite (spec §16.4).
		const gen::firmware::CurrentRecipe& current = gen::firmware::currentRecipe;
		if (!current.name.empty()) {
			suggestion_ = current.name;
		}
		else {
			gen::perform::defaultRecipeName(*current.recipe.genre, current.seed, suggestion_);
		}
		noneFree_ = true;
	}
	display->setNextTransitionDirection(1);
	return openUI(this);
}

bool SaveRecipeUI::opened() {
	outputTypeToLoad = OutputType::NONE;
	if (!SaveUI::opened()) { // clears enteredText; an error has already shown
		renderingNeededRegardlessOfUI();
		return false;
	}
	allowedFileExtensions = allowedFileExtensionsXML;
	title = deluge::l10n::get(deluge::l10n::String::STRING_FOR_SAVE_RECIPE);
	fileIcon = deluge::hid::display::OLED::songIcon;
	fileIconPt2 = nullptr;
	fileIconPt2Width = 0;
	currentFolderIsEmpty = false;

	// RECIPES/ is made on the first save (spec §16.3); the browser lists it, so it is made here, as stock's pattern
	// save makes its folders.
	Error error = createFoldersRecursiveIfNotExists(kRecipeFolder);
	char searchName[gen::perform::kMaxRecipeNameChars + 5] = {};
	if (error == Error::NONE) {
		error = currentDir.set(kRecipeFolder);
	}
	if (error == Error::NONE && !suggestion_.empty()) {
		// A name no file has is kept as the nominated one (Browser::arrivedInNewFolder()).
		std::strcpy(searchName, suggestion_.text);
		std::strcat(searchName, ".XML");
		error = enteredText.set(suggestion_.text);
	}
	if (error == Error::NONE) {
		error = arrivedInNewFolder(0, searchName, kRecipeFolder);
	}
	if (error == Error::NONE && noneFree_) {
		// The browser found the file and offered stock's numeric variation of it (`<name> 2`): offer the name itself,
		// on the file it selected.
		error = enteredText.set(suggestion_.text);
	}
	if (error != Error::NONE) {
		display->displayError(error);
		TEST_EVENT("gen_recipe_save full failed error=%d", static_cast<int>(error));
		renderingNeededRegardlessOfUI();
		return false;
	}
	focusRegained();
	enteredTextEditPos = 0;
	if (rootUIIsGenerativePerform()) {
		// SaveUI::opened() blacked the sidebar out; Perform redraws only the rows that change, so ask for all of them.
		uiNeedsRendering(this, 0, 0xFFFFFFFF);
	}
	TEST_EVENT("gen_recipe_save full opened name=%s", enteredText.get());
	return true;
}

bool SaveRecipeUI::performSave(bool mayOverwrite) {
	// The runner may have started a preload since the UI opened (a queued transition's): refused, and the UI stays.
	if (!gen::firmware::recipeSaveAllowed(false)) {
		refusedBusy("full");
		return false;
	}
	String filePath;
	Error error = getCurrentFilePath(&filePath);
	if (error == Error::NONE) {
		error = gen::firmware::saveCurrentRecipe(filePath.get(), enteredText.get(), suggestion_, mayOverwrite);
	}
	if (error == Error::FILE_ALREADY_EXISTS) {
		deluge::gui::context_menu::overwriteFile.currentSaveUI = this;
		if (deluge::gui::context_menu::overwriteFile.setupAndCheckAvailability()) {
			display->setNextTransitionDirection(1);
			openUI(&deluge::gui::context_menu::overwriteFile);
			TEST_EVENT("gen_recipe_save full overwrite? name=%s", enteredText.get());
			return true;
		}
		error = Error::UNSPECIFIED;
	}
	if (error != Error::NONE) {
		display->displayError(error);
		TEST_EVENT("gen_recipe_save full failed error=%d", static_cast<int>(error));
		return false;
	}
	display->displayPopup(enteredText.get());
	TEST_EVENT("gen_recipe_save full saved name=%s overwrite=%d", enteredText.get(), mayOverwrite ? 1 : 0);
	close();
	return true;
}

ActionResult SaveRecipeUI::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	const bool overPerform = rootUIIsGenerativePerform();
	if (overPerform && b == SYNC_SCALING) {
		return performView.buttonAction(b, on, inCardRoutine); // FILL while held, as under Perform's menus (spec §8.7)
	}
	if (b == SYNTH || b == KIT || b == LOAD || b == MIDI || b == CV) {
		return ActionResult::DEALT_WITH; // not stock's type switches or load
	}
	if (inCardRoutine && (b == SAVE || b == SELECT_ENC)) {
		// A save inside a card wait (a preload's, over Perform) is refused, not deferred: stock's
		// REMIND_ME_OUTSIDE_CARD_ROUTINE would hold every later press back until the wait ends (spec §12.10).
		if (on) {
			refusedBusy("full");
		}
		return ActionResult::DEALT_WITH;
	}
	if (on && b == BACK && !std::strcmp(currentDir.get(), kRecipeFolder)) {
		TEST_EVENT("gen_recipe_save full closed");
		close(); // BACK in RECIPES/ leaves with nothing saved
		return ActionResult::DEALT_WITH;
	}
	return SaveUI::buttonAction(b, on, inCardRoutine);
}

ActionResult SaveRecipeUI::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth && rootUIIsGenerativePerform()) {
		return performView.padAction(x, y, velocity); // the sidebar stays Perform's (spec §13.5)
	}
	return SaveUI::padAction(x, y, velocity);
}

bool SaveRecipeUI::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                 uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (rootUIIsGenerativePerform()) {
		return false;
	}
	return SaveUI::renderSidebar(whichRows, image, occupancyMask);
}

ActionResult SaveRecipeUI::timerCallback() {
	if (currentUIMode == UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS) {
		// Stock's long press offers a song's options (collect media, create folder): a Recipe has none. The press is
		// a save on release, as a short one is.
		return ActionResult::DEALT_WITH;
	}
	return SaveUI::timerCallback();
}

void SaveRecipeUI::graphicsRoutine() {
	if (rootUIIsGenerativePerform()) {
		performView.graphicsRoutine(); // Perform's frame: swaps, transitions and retirements go on underneath
		return;
	}
	SaveUI::graphicsRoutine();
}
