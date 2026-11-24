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

#include "gui/ui/load/load_recipe_ui.h"
#include "definitions.h"
#include "generative/core/recipe_file.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/recipe_files.h"
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "generative/perform/genre_menu.h"
#include "generative/perform/recipe_name.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/compose_screen.h"
#include "gui/menu_item/generative/perform_from.h"
#include "gui/menu_item/generative/regenerate_page.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/perform_view.h"
#include "hid/button.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "hid/led/pad_leds.h"
#include "io/midi/test_port.h"
#include "playback/playback_handler.h"
#include "storage/file_item.h"
#include "util/d_string.h"
#include <cstring>

namespace gen = deluge::generative;
using gen::firmware::kRecipeFolder;

LoadRecipeUI loadRecipeUI{};

namespace {

/// The file chosen, read whole, and its name: the transition confirm and the load read them after the browser closes.
struct Chosen {
	gen::RecipeFile file;
	gen::perform::RecipeName name;
};
PLACE_SDRAM_BSS Chosen chosen;

void refusedBusy() {
	display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_GENERATIVE_BUSY));
	TEST_EVENT("gen_recipe_load refused why=busy");
}

/// `name` without a trailing `.XML`, in any case.
std::string_view withoutExtension(const char* name) {
	std::string_view text = name;
	if (text.size() > 4 && gen::equalsIgnoringCase(text.substr(text.size() - 4), ".XML")) {
		text.remove_suffix(4);
	}
	return text;
}

/// The chosen file loads (spec §16.5): in PERFORM FROM, DISCARD SONG? asks first and OK enters Perform on it; in
/// Perform a transition, confirmed with its style while playing and at once while stopped; otherwise (the Compose
/// screen, its FROM, COMPOSE FROM) it replaces the Current Recipe and the Compose screen shows it. The version notice
/// (spec §16.2) shows as it loads: here, or at DISCARD SONG?'s OK, so backing out of that shows none.
void loadChosen() {
	if (deluge::gui::menu_item::generative::performFromShowing()) {
		// PERFORM FROM's FILE... (spec §14.1, §14.3).
		deluge::gui::menu_item::generative::performFromFile(chosen.file, chosen.name);
		return;
	}
	gen::firmware::showVersionNotice(chosen.file);
	if (!rootUIIsGenerativePerform()) {
		deluge::gui::menu_item::generative::loadComposeRecipe(chosen.file, chosen.name);
		return;
	}
	if (playbackHandler.playbackState != 0) {
		// The Regenerate page with only TRANSITION (spec §13.6): the file brings its own variants.
		deluge::gui::menu_item::generative::openLoadPage(chosen.file, chosen.name);
		return;
	}
	// Stopped: it rebuilds at once (it commits when loaded, as a stopped genre change does), so no style is asked.
	const gen::TransitionStyle style = gen::perform::confirmStyle(*chosen.file.recipe.genre);
	const gen::firmware::Transitions::Request request = gen::firmware::pressShowingWork(
	    [&] { return gen::firmware::transitions.load(chosen.file, chosen.name, style); });
	deluge::gui::refusedWithPopup(gen::perform::pressOutcome(request));
	TEST_EVENT("gen_recipe_load stopped result=%d working=%d", static_cast<int>(request),
	           gen::firmware::workingAnimationShown() ? 1 : 0);
}

} // namespace

bool LoadRecipeUI::open(bool inCardRoutine) {
	if (!gen::firmware::recipeCardAccessAllowed(inCardRoutine)) {
		refusedBusy();
		return false;
	}
	display->setNextTransitionDirection(1);
	return openUI(this);
}

bool LoadRecipeUI::opened() {
	Error error = beginSlotSession();
	char searchName[gen::perform::kMaxRecipeNameChars + 5] = {};
	if (error == Error::NONE) {
		title = deluge::l10n::get(deluge::l10n::String::STRING_FOR_LOAD_RECIPE);
		fileIcon = deluge::hid::display::OLED::songIcon;
		fileIconPt2 = nullptr;
		fileIconPt2Width = 0;
		filePrefix = "RECIPE";
		outputTypeToLoad = OutputType::NONE; // a card listing only
		allowedFileExtensions = allowedFileExtensionsXML;
		qwertyAlwaysVisible = false; // it hides once the player scrolls (spec §16.5)
		enteredText.clear();
		// The browser lists RECIPES/, which a card that never had a save lacks: made here, as the save UI makes it.
		error = createFoldersRecursiveIfNotExists(kRecipeFolder);
	}
	if (error == Error::NONE) {
		error = currentDir.set(kRecipeFolder);
	}
	if (error == Error::NONE) {
		// It opens on the file the Current Recipe came from or was saved as, if any.
		const gen::perform::RecipeName& remembered = gen::firmware::currentRecipe.name;
		if (!remembered.empty()) {
			std::strcpy(searchName, remembered.text);
			std::strcat(searchName, ".XML");
		}
		error = arrivedInNewFolder(0, searchName, kRecipeFolder);
	}
	if (error != Error::NONE) {
		display->displayError(error);
		TEST_EVENT("gen_recipe_load failed error=%d", static_cast<int>(error));
		renderingNeededRegardlessOfUI();
		return false;
	}
	qwertyVisible = true; // arrivedInNewFolder() hid it: it shows on entry
	drawKeys();
	focusRegained();
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	TEST_EVENT("gen_recipe_load opened at=%s", item != nullptr ? item->filename.get() : "");
#endif
	return true;
}

ActionResult LoadRecipeUI::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	const bool overPerform = rootUIIsGenerativePerform();
	if (overPerform && b == SYNC_SCALING) {
		return performView.buttonAction(b, on, inCardRoutine); // FILL while held, as under Perform's menus (spec §8.7)
	}
	if (b == SYNTH || b == KIT || b == SAVE || b == MIDI || b == CV || b == KEYBOARD) {
		return ActionResult::DEALT_WITH; // not stock's type switches, file delete or QWERTY toggle
	}
	if (b == LOAD || b == SELECT_ENC) {
		if (inCardRoutine) {
			// Refused, not deferred: stock's REMIND_ME_OUTSIDE_CARD_ROUTINE would hold every later press back until a
			// preload's card wait ends (spec §12.10). A press made outside the wait set the long-press mode and its
			// timer (Browser::mainButtonAction()): a release inside it ends them too, as the save UI's does.
			if (on) {
				refusedBusy();
			}
			else if (currentUIMode == UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS) {
				currentUIMode = UI_MODE_NONE;
				uiTimerManager.unsetTimer(TimerName::UI_SPECIFIC);
				refusedBusy();
			}
			return ActionResult::DEALT_WITH;
		}
		if (b == LOAD) {
			return mainButtonAction(on); // LOAD chooses, as SELECT does (stock)
		}
	}
	if (on && b == BACK && !std::strcmp(currentDir.get(), kRecipeFolder)) {
		TEST_EVENT("gen_recipe_load closed");
		closeBrowser(); // BACK in RECIPES/ leaves with nothing changed
		return ActionResult::DEALT_WITH;
	}
	return LoadUI::buttonAction(b, on, inCardRoutine);
}

ActionResult LoadRecipeUI::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth) {
		if (rootUIIsGenerativePerform()) {
			return performView.padAction(x, y, velocity); // the sidebar stays Perform's (spec §13.5)
		}
		return ActionResult::DEALT_WITH;
	}
	if (!qwertyVisible) {
		// The pads underneath show: a press brings the QWERTY back and types nothing, as stock's song browser does.
		if (velocity != 0) {
			qwertyVisible = true;
			drawKeys();
			PadLEDs::sendOutMainPadColours();
		}
		return ActionResult::DEALT_WITH;
	}
	return LoadUI::padAction(x, y, velocity);
}

ActionResult LoadRecipeUI::timerCallback() {
	if (currentUIMode == UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS) {
		return ActionResult::DEALT_WITH; // no stock context menu: the release chooses, held or not
	}
	return LoadUI::timerCallback();
}

bool LoadRecipeUI::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                 uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (rootUIIsGenerativePerform()) {
		return false;
	}
	return LoadUI::renderSidebar(whichRows, image, occupancyMask);
}

void LoadRecipeUI::graphicsRoutine() {
	if (rootUIIsGenerativePerform()) {
		performView.graphicsRoutine(); // Perform's frame: swaps, transitions and retirements go on underneath
		return;
	}
	LoadUI::graphicsRoutine();
}

void LoadRecipeUI::currentFileChanged(int32_t movementDirection) {
	if (movementDirection != 0) {
		hideQwerty();
	}
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	TEST_EVENT("gen_recipe_load at=%s qwerty=%d", item != nullptr ? item->filename.get() : "", qwertyVisible ? 1 : 0);
#endif
}

void LoadRecipeUI::hideQwerty() {
	qwertyVisible = false;
	uiNeedsRendering(this, 0xFFFFFFFF, 0); // the pads underneath, drawn by the UIs below
}

void LoadRecipeUI::enterKeyPress() {
	FileItem* item = getCurrentFileItem();
	if (item == nullptr) {
		return;
	}
	if (item->isFolder) {
		Error error = goIntoFolder(item->filename.get());
		if (error != Error::NONE) {
			display->displayError(error);
			TEST_EVENT("gen_recipe_load failed error=%d", static_cast<int>(error));
			closeBrowser();
		}
		return;
	}
	// A preload that started meanwhile holds the card: the file isn't read under it (spec §12.10).
	if (!gen::firmware::recipeCardAccessAllowed(false)) {
		refusedBusy();
		return;
	}
	String path;
	Error error = path.set(currentDir.get());
	if (error == Error::NONE) {
		error = path.concatenate("/");
	}
	if (error == Error::NONE) {
		error = path.concatenate(item->filename.get());
	}
	if (error != Error::NONE) {
		display->displayError(error);
		return;
	}
	const gen::RecipeFileRead read = gen::firmware::readRecipeFile(path.get(), chosen.file, error);
	if (read == gen::RecipeFileRead::UnknownGenre) {
		// Refused, and the browser stays to choose another (spec §16.2).
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_UNKNOWN_GENRE));
		TEST_EVENT("gen_recipe_load refused why=unknown_genre file=%s", item->filename.get());
		return;
	}
	if (read == gen::RecipeFileRead::Unreadable) {
		display->displayError(error != Error::NONE ? error : Error::FILE_CORRUPTED);
		TEST_EVENT("gen_recipe_load refused why=unreadable error=%d file=%s", static_cast<int>(error),
		           item->filename.get());
		return;
	}
	// A name too long to remember loads with none: the next save suggests the default name (spec §16.3).
	chosen.name.set(withoutExtension(item->filename.get()));
	TEST_EVENT("gen_recipe_load chose name=%s genre=%s version=%d", chosen.name.text, chosen.file.recipe.genre->name,
	           static_cast<int>(chosen.file.genreVersion));
	closeBrowser();
	loadChosen();
}

void LoadRecipeUI::closeBrowser() {
	indicator_leds::setLedState(IndicatorLED::LOAD, false);
	close();
}
