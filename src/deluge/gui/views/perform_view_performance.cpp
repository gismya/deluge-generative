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

// PerformView's second KEYBOARD page (spec §13.5): stock Performance view, hosted. Its song FX pads are stock
// PerformanceView's own: its layout (the card's PerformanceView.XML, or stock's default), its pad logic (hold, latch,
// depth rows, stutter) and its render, on Perform's song. Perform stays the root UI around it: its sidebar, buttons,
// OLED and frame run on, so the piece plays on underneath. Its column editing stays in stock's own view, outside
// Perform: a layout saved there is the one this page loads.

#include "gui/views/perform_view_internal.h"
#include "gui/views/performance_view.h"

using deluge::generative::firmware::performAdapter;
using namespace perform_view_shared;

namespace {

/// Runs `act` with the knobs' mod controllable on the song. Stock's pads set a param, and start and end stutter, only
/// through it, and its layout reads stutter's options there, as stock's own view keeps it on the song. Song FX act on
/// the whole mix whatever is focused (spec §13.5): with a part's clip on the knobs (`clip`), they move to the song for
/// `act` and back after it.
template <typename Act>
void onSong(InstrumentClip* clip, Act act) {
	if (clip != nullptr) {
		view.setActiveModControllableTimelineCounter(currentSong, false);
	}
	act();
	if (clip != nullptr) {
		view.setActiveModControllableTimelineCounter(clip, false);
	}
}

} // namespace

void PerformView::enterPerformancePage() {
	if (stockLayoutLoaded_) {
		return;
	}
	// Nothing stock held on another song carries into Perform's: a held column's release would set Perform's song
	// FX to the other song's values. Stock's editing modes stay stock's own. The fields are those stock's private
	// initFXPress() resets, to the values it gives them.
	for (FXColumnPress& press : performanceView.fxPress) {
		press.previousKnobPosition = kNoSelection;
		press.currentKnobPosition = kNoSelection;
		press.yDisplay = kNoSelection;
		press.timeLastPadPress = 0;
		press.padPressHeld = false;
	}
	performanceView.defaultEditingMode = false;
	performanceView.editingParam = false;
	onSong(partClip(focusedPart()), [] {
		performanceView.loadPerformanceViewLayout(); // reads the card the first time since power-on
	});
	updateKnobIndicators();
	stockLayoutLoaded_ = true;
	TEST_EVENT("gen_performance layout");
}

void PerformView::performancePadAction(int32_t x, int32_t y, bool on) {
	const auto bit = static_cast<uint8_t>(1U << y);
	if (on) {
		stockHeld_[x] |= bit;
	}
	else if ((stockHeld_[x] & bit) == 0) {
		return; // a release whose press this page never saw
	}
	else {
		stockHeld_[x] &= static_cast<uint8_t>(~bit);
	}
	InstrumentClip* clip = partClip(focusedPart());
	onSong(clip, [&] {
		performanceView.padAction(x, y, on ? 1 : 0);
		if (on && performanceView.lastPadPress.isActive && performanceView.lastPadPress.xDisplay == x) {
			// Stock's view shows the value on its OLED; over Perform's screen it is the knob popup.
			view.displayModEncoderValuePopup(performanceView.lastPadPress.paramKind,
			                                 performanceView.lastPadPress.paramID,
			                                 performanceView.fxPress[x].currentKnobPosition);
		}
	});
	if (clip != nullptr) {
		updateKnobIndicators();
	}
	TEST_EVENT("gen_performance_pad x=%ld y=%ld %s value=%ld held=%d", static_cast<long>(x), static_cast<long>(y),
	           on ? "down" : "up", static_cast<long>(performanceView.fxPress[x].currentKnobPosition),
	           performanceView.fxPress[x].padPressHeld ? 1 : 0);
	refreshPerformancePage();
}

void PerformView::refreshPerformancePage() {
	if (page_ != Page::Performance) {
		return;
	}
	performanceAt_ = AudioEngine::audioSampleTimer;
	// Stock renders into a whole grid image with the sidebar; only its main pads are kept, and only changed rows
	// redraw.
	RGB image[kDisplayHeight][kDisplayWidth + kSideBarWidth] = {};
	uint8_t occupancy[kDisplayHeight][kDisplayWidth + kSideBarWidth] = {};
	performanceView.renderMainPads(0xFF, image, occupancy);
	RGB pads[kDisplayHeight][kDisplayWidth];
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		std::copy_n(image[y], kDisplayWidth, pads[y]);
	}
	showMainPads(pads);
}
