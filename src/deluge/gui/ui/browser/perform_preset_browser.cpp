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

#include "gui/ui/browser/perform_preset_browser.h"
#include "definitions_cxx.hpp"
#include "generative/core/recipe.h"
#include "gui/l10n/l10n.h"
#include "gui/views/perform_view.h"
#include "hid/button.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "io/midi/test_port.h"
#include "storage/file_item.h"
#include <cstring>

PerformPresetBrowser performPresetBrowser{};

namespace {
/// The folder a preset browser opens on when the pick has none, and falls back to when the pick's is gone.
constexpr const char* kSynthsFolder = "SYNTHS";
} // namespace

bool PerformPresetBrowser::open(int32_t part, const char* pick) {
	part_ = part;
	pick_ = pick != nullptr ? pick : "";
	display->setNextTransitionDirection(1);
	return openUI(this);
}

bool PerformPresetBrowser::opened() {
	Error error = beginSlotSession();
	if (error == Error::NONE) {
		title = deluge::l10n::get(deluge::l10n::String::STRING_FOR_CHOOSE_SOUND);
		fileIcon = deluge::hid::display::OLED::synthIcon;
		fileIconPt2 = nullptr;
		fileIconPt2Width = 0;
		filePrefix = "SYNT";
		// A card listing only: the stock browser also lists the song's Instruments, and Perform's are its parts.
		outputTypeToLoad = OutputType::NONE;
		qwertyVisible = true;
		enteredText.clear();

		// Start in the pick's folder, on the pick (spec §13.5).
		const char* slash = strrchr(pick_, '/');
		const char* name = nullptr;
		if (slash != nullptr) {
			error = currentDir.set(pick_, static_cast<int32_t>(slash - pick_));
			name = slash + 1;
		}
		else {
			error = currentDir.set(kSynthsFolder);
		}
		if (error == Error::NONE) {
			error = arrivedInNewFolder(0, name, kSynthsFolder);
		}
	}
	if (error != Error::NONE) {
		display->displayError(error);
		TEST_EVENT("gen_browser closed error=%d", static_cast<int>(error));
		return false;
	}
	focusRegained();
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	TEST_EVENT("gen_browser open part=%ld pick=%s at=%s/%s", static_cast<long>(part_), pick_, currentDir.get(),
	           item != nullptr ? item->filename.get() : "");
#endif
	return true;
}

void PerformPresetBrowser::focusRegained() {
	indicator_leds::blinkLed(IndicatorLED::SYNTH);
	SlotBrowser::focusRegained();
}

ActionResult PerformPresetBrowser::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (b == SYNC_SCALING) {
		return performView.buttonAction(b, on, inCardRoutine); // FILL while held, as under Perform's menus (spec §8.7)
	}
	if (b == SYNTH || b == KIT || b == SAVE || b == LOAD) {
		// Not the stock browser's type switch, file delete or load: Perform's own meanings wait for it to close.
		return ActionResult::DEALT_WITH;
	}
	return SlotBrowser::buttonAction(b, on, inCardRoutine);
}

ActionResult PerformPresetBrowser::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth) {
		// The sidebar stays Perform's: mutes, Section forces and End (spec §13.5).
		return performView.padAction(x, y, velocity);
	}
	return SlotBrowser::padAction(x, y, velocity);
}

ActionResult PerformPresetBrowser::timerCallback() {
	if (currentUIMode == UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS) {
		return ActionResult::DEALT_WITH; // no stock context menu: SELECT's release chooses, held or not
	}
	return SlotBrowser::timerCallback();
}

void PerformPresetBrowser::graphicsRoutine() {
	// Perform's frame: swaps, transitions and retirements are serviced while browsing, and the sidebar is live.
	performView.graphicsRoutine();
}

void PerformPresetBrowser::currentFileChanged(int32_t movementDirection) {
	// Nothing loads: the part plays on with its sound until a preset is chosen (spec §13.5).
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	TEST_EVENT("gen_browser at=%s/%s", currentDir.get(), item != nullptr ? item->filename.get() : "");
#endif
}

void PerformPresetBrowser::folderContentsReady(int32_t entryDirection) {
	currentFileChanged(0);
}

void PerformPresetBrowser::enterKeyPress() {
	FileItem* item = getCurrentFileItem();
	if (item == nullptr) {
		return;
	}
	if (item->isFolder) {
		Error error = goIntoFolder(item->filename.get());
		if (error != Error::NONE) {
			display->displayError(error);
			TEST_EVENT("gen_browser closed error=%d", static_cast<int>(error));
			closeBrowser();
		}
		return;
	}
	deluge::generative::SoundPath path;
	if (!path.set(currentDir.get(), item->filename.get())) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_PATH_TOO_LONG));
		return; // a pick holds no longer path (spec §10.1): choose another
	}
	const int32_t part = part_;
	TEST_EVENT("gen_browser chose part=%ld path=%s", static_cast<long>(part), path.c_str());
	closeBrowser();
	performView.chooseSound(part, path.c_str());
}

void PerformPresetBrowser::exitAction() {
	TEST_EVENT("gen_browser closed");
	closeBrowser();
}

void PerformPresetBrowser::closeBrowser() {
	indicator_leds::setLedState(IndicatorLED::SYNTH, false);
	part_ = -1;
	pick_ = "";
	close();
}
