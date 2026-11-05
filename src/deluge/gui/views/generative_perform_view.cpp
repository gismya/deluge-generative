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

#include "gui/views/generative_perform_view.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/debug/hw_probe.h"
#include "generative/debug/poc_probe.h"
#include "generative/perform/perform_driver.h"
#include "gui/colour/palette.h"
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "hid/led/pad_leds.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

using deluge::generative::kMacroMax;
using deluge::generative::kStepsPerBar;
using deluge::generative::Recipe;
using deluge::generative::perform::performDriver;

PLACE_SDRAM_BSS GenerativePerformView generativePerformView{};

namespace {
constexpr int32_t kEnergyKnob = 1;  // Upper gold knob.
constexpr int32_t kDensityKnob = 0; // Lower gold knob.
constexpr int32_t kStepsPerBeat = 4;

uint8_t clampMacro(int32_t value) {
	return static_cast<uint8_t>(std::clamp<int32_t>(value, 0, kMacroMax));
}
} // namespace

void GenerativePerformView::enterFromSession() {
	if (display->haveOLED()) {
		deluge::hid::display::OLED::displayWorkingAnimation("Loading");
	}
	GEN_PROBE(double enterStart = getSystemTime();)
	Error error = performDriver.enter(currentSong);
	GEN_PROBE(deluge::generative::probe::entered(getSystemTime() - enterStart, static_cast<int32_t>(error));)
	display->removeWorkingAnimation();
	if (error != Error::NONE) {
		display->displayError(error);
		return;
	}
	changeRootUI(this);
}

bool GenerativePerformView::opened() {
	focusRegained();
	return true;
}

void GenerativePerformView::focusRegained() {
	// PLAY must start session playback, not arrangement playback, from a root UI that isn't a ClipMinder.
	currentSong->lastClipInstanceEnteredStartPos = -1;
	lastShownStep_ = -1;

	indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::CLIP_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::KEYBOARD, false);
	indicator_leds::setLedState(IndicatorLED::CROSS_SCREEN_EDIT, false);
	indicator_leds::setLedState(IndicatorLED::SCALE_MODE, false);
	indicator_leds::setLedState(IndicatorLED::BACK, true);
	updateKnobIndicators();

	PadLEDs::clearTickSquares(false);
	uiNeedsRendering(this);
	renderUIsForOled();
}

void GenerativePerformView::exitToSession() {
	PadLEDs::clearTickSquares();
	indicator_leds::setLedState(IndicatorLED::BACK, false);
	performDriver.exit(currentSong);
	changeRootUI(&sessionView);
}

ActionResult GenerativePerformView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;

	if (b == BACK) {
		if (on && currentUIMode == UI_MODE_NONE) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			exitToSession();
		}
		return ActionResult::DEALT_WITH;
	}

	// Everything else falls through to the global handling: PLAY, SHIFT, RECORD and the encoder pushes work as
	// everywhere; other buttons do nothing here.
	return ActionResult::NOT_DEALT_WITH;
}

void GenerativePerformView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	if (getCurrentUI() != this) {
		return;
	}
	Recipe recipe = performDriver.recipe();
	if (whichModEncoder == kEnergyKnob) {
		recipe.energy = clampMacro(recipe.energy + offset);
	}
	else if (whichModEncoder == kDensityKnob) {
		recipe.density = clampMacro(recipe.density + offset);
	}
	else {
		return;
	}
	performDriver.setRecipe(recipe);
	updateKnobIndicators();
	renderUIsForOled();
}

void GenerativePerformView::updateKnobIndicators() {
	const Recipe& recipe = performDriver.recipe();
	indicator_leds::setKnobIndicatorLevel(kEnergyKnob, (recipe.energy * 128) / kMacroMax);
	indicator_leds::setKnobIndicatorLevel(kDensityKnob, (recipe.density * 128) / kMacroMax);
}

void GenerativePerformView::graphicsRoutine() {
	// Must not fall back to UI::graphicsRoutine(), which calls the root UI's, i.e. this one, again.
	GEN_PROBE(deluge::generative::probe::uiTick();)
	int32_t step = performDriver.currentStep();
	if (step != lastShownStep_) {
		updateStepDisplay(step);
	}
#if ENABLE_SYSEX_TEST
	// H7 redraw test: on each 16th grid line (not the decided step, which runs up to 12 ticks ahead), redraw the
	// test rows, and the OLED each beat or each step.
	if (deluge::generative::hwprobe::redrawTestOn()) {
		bool oledThisStep = false;
		uint32_t testRows =
		    deluge::generative::hwprobe::stepShown(deluge::generative::hwprobe::gridStep(), &oledThisStep);
		if (testRows != 0) {
			uiNeedsRendering(this, testRows, deluge::generative::hwprobe::redrawSidebarRows());
		}
		if (oledThisStep) {
			renderUIsForOled();
		}
	}
#endif
}

void GenerativePerformView::updateStepDisplay(int32_t step) {
	bool beatChanged = (step < 0) || (lastShownStep_ < 0) || (step / kStepsPerBeat != lastShownStep_ / kStepsPerBeat);
	lastShownStep_ = step;

	// The playhead: one column per 16th across the main grid, sent as tick squares (only changed columns go out).
	uint8_t squares[kDisplayHeight];
	uint8_t colours[kDisplayHeight];
	std::memset(squares, 255, sizeof(squares));
	std::memset(colours, 0, sizeof(colours));
	if (step >= 0) {
		std::memset(squares, step % kStepsPerBar, sizeof(squares));
	}
	PadLEDs::setTickSquares(squares, colours);

#if ENABLE_SYSEX_TEST
	if (deluge::generative::hwprobe::redrawTestOn()) {
		return; // The H7 redraw test draws the OLED from the grid line instead (graphicsRoutine).
	}
#endif
	if (beatChanged) {
		renderUIsForOled();
	}
}

void GenerativePerformView::notifyPlaybackBegun() {
	lastShownStep_ = -1;
	renderUIsForOled();
}

void GenerativePerformView::playbackEnded() {
	performDriver.transportStopped();
	lastShownStep_ = -1;
	PadLEDs::clearTickSquares();
	renderUIsForOled();
}

bool GenerativePerformView::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                           uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth],
                                           bool drawUndefinedArea) {
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
#if ENABLE_SYSEX_TEST
	// H7 redraw test: the changed rows show a moving column, so each redraw really changes them.
	uint32_t testRows = deluge::generative::hwprobe::redrawRows();
	int32_t testStep = deluge::generative::hwprobe::redrawStep();
	if (testStep >= 0) {
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			if (whichRows & testRows & (1 << y)) {
				image[y][(testStep + y) % kDisplayWidth] = deluge::gui::colours::green;
			}
		}
	}
#endif
	return true;
}

bool GenerativePerformView::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
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
#if ENABLE_SYSEX_TEST
	// H7 redraw test: one lit sidebar pad that moves each 16th.
	int32_t testStep = deluge::generative::hwprobe::redrawStep();
	if (deluge::generative::hwprobe::redrawSidebarRows() != 0 && testStep >= 0) {
		image[testStep % kDisplayHeight][kDisplayWidth] = deluge::gui::colours::red;
	}
#endif
	return true;
}

void GenerativePerformView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.drawScreenTitle("Techno");

	const Recipe& recipe = performDriver.recipe();
	char line[32];
	int32_t y = OLED_MAIN_TOPMOST_PIXEL + 14;

	snprintf(line, sizeof(line), "Energy  %d", recipe.energy);
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
	y += kTextSpacingY;

	snprintf(line, sizeof(line), "Density %d", recipe.density);
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
	y += kTextSpacingY;

	int32_t step = performDriver.currentStep();
	if (playbackHandler.isEitherClockActive() && step >= 0) {
		snprintf(line, sizeof(line), "Bar %d.%d", static_cast<int>(step / kStepsPerBar + 1),
		         static_cast<int>((step % kStepsPerBar) / kStepsPerBeat + 1));
	}
	else if (playbackHandler.isEitherClockActive()) {
		snprintf(line, sizeof(line), "Starting");
	}
	else {
		snprintf(line, sizeof(line), "Stopped");
	}
#if ENABLE_SYSEX_TEST
	int32_t gridStep = deluge::generative::hwprobe::redrawStep();
	if (deluge::generative::hwprobe::redrawTestOn() && gridStep >= 0) {
		if (deluge::generative::hwprobe::redrawOledEveryStep()) {
			snprintf(line, sizeof(line), "Bar %d.%d.%d", static_cast<int>(gridStep / kStepsPerBar + 1),
			         static_cast<int>((gridStep % kStepsPerBar) / kStepsPerBeat + 1),
			         static_cast<int>(gridStep % kStepsPerBeat + 1));
		}
		else {
			snprintf(line, sizeof(line), "Bar %d.%d", static_cast<int>(gridStep / kStepsPerBar + 1),
			         static_cast<int>((gridStep % kStepsPerBar) / kStepsPerBeat + 1));
		}
	}
#endif
	canvas.drawString(line, 0, y, kTextSpacingX, kTextSpacingY);
}
