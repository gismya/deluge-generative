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

#include "gui/views/perform_view.h"
#include "generative/core/genre.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_session.h"
#include "gui/colour/palette.h"
#include "gui/context_menu/clear_song.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "model/mod_controllable/mod_controllable.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include <algorithm>
#include <cstdio>

using deluge::generative::kMacroFull;
using deluge::generative::kMacroUnitsPerKnobStep;
using deluge::generative::kTicksPerStep;
using deluge::generative::Macro;
using deluge::generative::macroToPoints;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performSession;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;
/// The upper gold knob is encoder 1, the lower is 0.
constexpr int32_t kUpperKnob = 1;
constexpr int32_t kTicksPerBeat = 4 * kTicksPerStep;

const char* const kMacroNames[] = {"ENERGY", "DENSITY", "COMPLEXITY", "MOTION"};
const char* const kSectionNames[] = {"INTRO", "GROOVE", "BREAK", "BUILD", "OUTRO"};

uint8_t* songModKnobMode() {
	ModControllable* modControllable = view.activeModControllableModelStack.modControllable;
	return modControllable ? modControllable->getModKnobMode() : nullptr;
}

} // namespace

bool PerformView::opened() {
	focusRegained();
	return true;
}

void PerformView::focusRegained() {
	// PLAY must start session playback: Perform isn't a ClipMinder, and has no arrangement (spec §12.1).
	currentSong->lastClipInstanceEnteredStartPos = -1;
	currentSong->affectEntire = true;
	view.setActiveModControllableTimelineCounter(currentSong);

	indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::CLIP_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::KEYBOARD, false);
	indicator_leds::setLedState(IndicatorLED::BACK, true);
	updateKnobIndicators();

	shownBeat_ = -1;
	shownSection_ = -1;
	uiNeedsRendering(this);
	renderUIsForOled();
}

ActionResult PerformView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;

	if (b == BACK) {
		if (on && currentUIMode == UI_MODE_NONE) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE; // leaving loads a blank song's synth
			}
			leave();
		}
		return ActionResult::DEALT_WITH;
	}
	// PLAY, RECORD, SHIFT and the encoder pushes go to the global handling, which guards RECORD and X+PLAY under
	// Perform (F9). Nothing else reaches stock views from here.
	return ActionResult::NOT_DEALT_WITH;
}

void PerformView::leave() {
	// Until the leave menu lands, BACK keeps nothing: a new blank song in session view, the same as SHIFT+LOAD → NEW
	// (spec §13.8).
	performSession.leave();
	indicator_leds::setLedState(IndicatorLED::BACK, false);
	deluge::gui::context_menu::clearSong.acceptCurrentOption();
}

bool PerformView::onMacroPage() const {
	uint8_t* mode = songModKnobMode();
	return mode != nullptr && *mode == kMacroModButton;
}

void PerformView::modButtonAction(uint8_t whichButton, bool on) {
	if (whichButton == kMacroModButton) {
		if (on) {
			uint8_t* mode = songModKnobMode();
			if (mode == nullptr) {
				return;
			}
			secondMacroSet_ = (*mode == kMacroModButton) && !secondMacroSet_;
			*mode = kMacroModButton;
			view.setModLedStates();
			updateKnobIndicators();
		}
		return;
	}
	if (on && whichButton == 0 && songModKnobMode() != nullptr && *songModKnobMode() == 0) {
		return; // Perform owns the mod buttons: F1 again never turns the VU meter on (spec §13.5)
	}
	view.modButtonAction(whichButton, on);
	updateKnobIndicators();
}

void PerformView::updateKnobIndicators() {
	if (!onMacroPage()) {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton]);
		view.setKnobIndicatorLevels();
		return;
	}
	// The knob rings show the centre (spec §13.5). The F8 LED blinks while Complexity / Motion is on.
	Macro upper = secondMacroSet_ ? Macro::Complexity : Macro::Energy;
	Macro lower = secondMacroSet_ ? Macro::Motion : Macro::Density;
	const auto& macros = currentRecipe.recipe.macros;
	indicator_leds::setKnobIndicatorLevel(1, macros[upper] * 128 / kMacroFull);
	indicator_leds::setKnobIndicatorLevel(0, macros[lower] * 128 / kMacroFull);
	if (secondMacroSet_) {
		indicator_leds::blinkLed(indicator_leds::modLed[kMacroModButton]);
	}
	else {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton], true);
		indicator_leds::setLedState(indicator_leds::modLed[kMacroModButton], true);
	}
}

void PerformView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	if (!onMacroPage()) {
		view.modEncoderAction(whichModEncoder, offset);
		return;
	}
	Macro macro = whichModEncoder == kUpperKnob ? (secondMacroSet_ ? Macro::Complexity : Macro::Energy)
	                                            : (secondMacroSet_ ? Macro::Motion : Macro::Density);
	int32_t value = currentRecipe.recipe.macros[macro] + offset * kMacroUnitsPerKnobStep;
	performSession.setMacro(macro, value);
	updateKnobIndicators();

	char popup[24];
	snprintf(popup, sizeof(popup), "%s %d", kMacroNames[static_cast<int32_t>(macro)],
	         static_cast<int>(macroToPoints(currentRecipe.recipe.macros[macro])));
	display->displayPopup(popup);
	renderUIsForOled();
}

void PerformView::graphicsRoutine() {
	// Not UI::graphicsRoutine(): it calls the root UI's, which is this one.
	const auto& output = performSession.output();
	int32_t beat = performSession.playing() ? performSession.clock() / kTicksPerBeat : -1;
	int32_t section = output.sectionKnown ? static_cast<int32_t>(output.section) : -1;
	if (beat != shownBeat_ || section != shownSection_ || output.occurrence != shownOccurrence_) {
		shownBeat_ = beat;
		shownSection_ = section;
		shownOccurrence_ = output.occurrence;
		renderUIsForOled();
	}
}

void PerformView::notifyPlaybackBegun() {
	renderUIsForOled();
}

void PerformView::playbackEnded() {
	performSession.playbackEnded();
	renderUIsForOled();
}

bool PerformView::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                 uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) {
	if (image == nullptr) {
		return true;
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		if (whichRows & (1 << y)) {
			std::fill_n(image[y], kDisplayWidth, deluge::gui::colours::black);
			if (occupancyMask != nullptr) {
				std::fill_n(occupancyMask[y], kDisplayWidth, 0);
			}
		}
	}
	return true;
}

bool PerformView::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (image == nullptr) {
		return true;
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		if (whichRows & (1 << y)) {
			image[y][kDisplayWidth] = deluge::gui::colours::black;
			image[y][kDisplayWidth + 1] = deluge::gui::colours::black;
		}
	}
	return true;
}

void PerformView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	const auto* genre = currentRecipe.recipe.genre;
	canvas.drawScreenTitle(genre ? genre->oledName : "PERFORM");

	char line[32];
	int32_t y = OLED_MAIN_TOPMOST_PIXEL + 14;
	const auto& output = performSession.output();
	if (!performSession.playing()) {
		snprintf(line, sizeof(line), "STOPPED");
	}
	else if (output.sectionKnown) {
		int32_t beat = performSession.clock() / kTicksPerBeat;
		snprintf(line, sizeof(line), "%s %d  %d.%d", kSectionNames[static_cast<int32_t>(output.section)],
		         static_cast<int>(output.occurrence + 1), static_cast<int>(beat / 4 + 1),
		         static_cast<int>(beat % 4 + 1));
	}
	else {
		snprintf(line, sizeof(line), "STARTING");
	}
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
	y += kTextSpacingY + 2;

	const auto& macros = currentRecipe.recipe.macros;
	snprintf(line, sizeof(line), "E%d D%d C%d M%d", static_cast<int>(macroToPoints(macros[Macro::Energy])),
	         static_cast<int>(macroToPoints(macros[Macro::Density])),
	         static_cast<int>(macroToPoints(macros[Macro::Complexity])),
	         static_cast<int>(macroToPoints(macros[Macro::Motion])));
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
}
