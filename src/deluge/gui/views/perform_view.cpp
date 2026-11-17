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

#include "gui/views/perform_view_internal.h"

using deluge::generative::firmware::keepFeedbackRunning;
using deluge::generative::firmware::PartOutput;
using deluge::generative::firmware::performAdapter;
using deluge::generative::firmware::Pump;
using deluge::generative::firmware::SoundSwaps;
using deluge::generative::firmware::soundSwaps;
using deluge::generative::firmware::transitions;
using deluge::generative::perform::kThinDriveModButton;
using deluge::gui::refusedWithPopup;
using deluge::gui::context_menu::leavePerform;
using namespace perform_view_shared;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// The CLIP page's faders are read this often, so a ramping live value moves on them without redrawing the grid every
/// frame (tuning).
constexpr uint32_t kFaderRefreshSamples = kSampleRate / 10;

} // namespace

bool PerformView::opened() {
	// A new entry starts on the SONG page with nothing focused (spec §13.1), whatever the view held when Perform last
	// closed.
	page_ = Page::Song;
	thinDrivePart_ = -1;
	macroHolds_ = {};
	fillHolds_ = {};
#if ENABLE_SYSEX_TEST
	shownRings_[0] = '\0';
	shownKeyboard_[0] = '\0';
	shownFaders_[0] = '\0';
	shownClip_[0] = '\0';
	shownSections[0] = '\0';
	for (char* row : shownParts) {
		row[0] = '\0';
	}
#endif
	heldPart_ = -1;
	lockedPart_ = -1;
	lastFocusedPart_ = -1;
	lastTapPart_ = -1;
	focusRegained();
	return true;
}

void PerformView::focusRegained() {
	// PLAY must start session playback: Perform isn't a ClipMinder, and has no arrangement (spec §12.1).
	currentSong->lastClipInstanceEnteredStartPos = -1;
	currentSong->affectEntire = true;
	// Presses a menu or overlay cut short lost their releases: a part row let go under it lets go (a lock stays), a
	// held End pad never arms, BACK's release isn't a tap, FILL let go under the overlay ends (spec §8.7: while held),
	// and so does a KEYBOARD fader's hold. A row still held (BACK or SONG closing the sound editor with a finger on
	// it) keeps its part focused, as releaseLostHolds() keeps a KEYBOARD pad still held.
	if (heldPart_ >= 0 && (heldX_ < 0 || !matrixDriver.isPadPressed(heldX_, yFromRow(heldPart_)))) {
		heldPart_ = -1;
	}
	releaseEndPad();
	backDown_ = false;
	xDown_ = false;
	releaseLostHolds();
	// The song's current Clip is set only while the sound editor is open on a part (openSoundEditor()).
	if (currentSong->getCurrentClip() != nullptr) {
		currentSong->setCurrentClip(nullptr);
	}
	if (editorOpen_) {
		editorOpen_ = false;
		TEST_EVENT("gen_editor closed");
	}
	indicator_leds::setLedState(IndicatorLED::BACK, true);
	bindKnobs();

	// Everything redraws, from caches filled once here.
	refreshSidebar(false);
	arc_.forgetPlan();
	refreshPlan();
	refreshArcMap();
	refreshFaders();
	refreshKeyboardPage();
	compose(screen_);
	uiNeedsRendering(this);
	renderUIsForOled();
}

ActionResult PerformView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;

	if (b == BACK) {
		// A tap cancels the newest queued action (spec §13.5). A hold opens the leave menu instead (exitUI()). A
		// release whose press went to another UI isn't a tap.
		if (on) {
			backDown_ = true;
			backHeld_ = false;
		}
		else {
			if (backDown_ && !backHeld_) {
				// A preload still running is the newest action (spec §12.10); otherwise the newest queued one.
				if (transitions.preloading() && transitions.cancel()) {
					TEST_EVENT("gen_back_tap transition");
				}
				else if (soundSwaps.cancelPreload()) {
					TEST_EVENT("gen_back_tap preload");
				}
				else {
					refusedWithPopup(performAdapter.steering().cancelNewest());
					TEST_EVENT("gen_back_tap");
				}
			}
			backDown_ = false;
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == X_ENC) {
		// Tapping X returns the Arc map to now. A turn or a solo (X held + MUTE) while it's down isn't a tap.
		if (on) {
			xDown_ = true;
			xUsed_ = false;
		}
		else {
			if (xDown_ && !xUsed_ && arc_.scrollToNow()) {
				TEST_EVENT("gen_arc_scroll 0");
				refreshArcMap();
				refreshScreen();
			}
			xDown_ = false;
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == AFFECT_ENTIRE) {
		// On the CLIP page AFFECT ENTIRE locks the part shown, or unlocks it; elsewhere it unlocks a locked part.
		// Unlocked, the knobs go back to the Recipe level (spec §13.5).
		if (!on) {
			return ActionResult::DEALT_WITH;
		}
		const int32_t shown = page_ == Page::Clip ? shownPart() : -1;
		if (shown >= 0) {
			toggleLock(shown);
		}
		else if (lockedPart_ >= 0) {
			unlock();
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == SESSION_VIEW || b == CLIP_VIEW || b == KEYBOARD) {
		// SONG, CLIP and KEYBOARD switch the grid page; the gold knobs stay where they are (spec §13.5).
		if (on) {
			page_ = b == CLIP_VIEW ? Page::Clip : b == KEYBOARD ? Page::Keyboard : Page::Song;
			TEST_EVENT("gen_page %s", page_ == Page::Clip ? "CLIP" : page_ == Page::Keyboard ? "KEYBOARD" : "SONG");
			assertPageLeds();
			refreshArcMap();
			refreshFaders();
			refreshKeyboardPage();
			refreshScreen();
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == SYNTH || b == KIT) {
		// The focused part's new sound at the phrase line; SHIFT, its previous sound. SYNTH on a synth part opens the
		// preset browser instead (spec §13.5).
		if (on && focusedPart() >= 0) {
			if (b == SYNTH && !Buttons::isShiftButtonPressed() && SoundSwaps::synthPart(focusedPart())) {
				if (inCardRoutine) {
					return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
				}
				openPresetBrowser(focusedPart());
			}
			else {
				soundPress(focusedPart(), Buttons::isShiftButtonPressed());
			}
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == CROSS_SCREEN_EDIT) {
		// The focused part's freeze, on the next undecided step (spec §13.5). Its LED shows the part frozen.
		if (on && focusedPart() >= 0) {
			freezePress(focusedPart());
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == Y_ENC) {
		if (on) {
			showKeyAndSeed();
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == SELECT_ENC && !Buttons::isShiftButtonPressed()) {
		// SELECT push: the Perform menu (spec §13.6). SHIFT+SELECT stays stock.
		return on ? openMenu(&deluge::gui::menu_item::generative::performMenu, "PERFORM", inCardRoutine)
		          : ActionResult::DEALT_WITH;
	}
	if (b == SCALE_MODE) {
		// SCALE: the key and scale editor (spec §13.5). Its LED blinks while a key change waits (assertLeds()).
		return on ? openMenu(&deluge::gui::menu_item::generative::keyScaleMenu, "KEY_SCALE", inCardRoutine)
		          : ActionResult::DEALT_WITH;
	}
	if (b == SYNC_SCALING) {
		holdFill(deluge::generative::perform::FillHolds::kButton, on);
		return ActionResult::DEALT_WITH;
	}
	if (b == TAP_TEMPO) {
		// Stock's TAP handling (SHIFT+TAP = metronome, MIDI learn); the tap itself sets the Recipe tempo (F4).
		return view.buttonAction(b, on, inCardRoutine);
	}
	// PLAY, RECORD, SHIFT and the encoder pushes go to the global handling, which guards RECORD and X+PLAY under
	// Perform (F9). Nothing else reaches stock views from here.
	return ActionResult::NOT_DEALT_WITH;
}

void PerformView::holdFill(int32_t holder, bool on) {
	// FILL while held, whatever the community setting; SHIFT plays a fresh pick (spec §8.7, §13.5). It moves the song's
	// fill mode, as the MIDI-learnable FILL command does, and the tick sends it to the core from there. SYNC-SCALING
	// and the KEYBOARD page's FILL pads hold it together: it ends with the last one let go.
	if (!fillHolds_.set(holder, on)) {
		return;
	}
	if (on) {
		performAdapter.steering().latchFillShift(Buttons::isShiftButtonPressed());
	}
	currentSong->changeFillMode(on);
	indicator_leds::setLedState(IndicatorLED::SYNC_SCALING, on);
	TEST_EVENT("gen_fill_button %s", on ? "down" : "up");
}

ActionResult PerformView::openMenu(MenuItem* menu, const char* name, bool inCardRoutine) {
	if (currentUIMode != UI_MODE_NONE) {
		return ActionResult::DEALT_WITH;
	}
	if (inCardRoutine) {
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	}
	letGoForOverlay(); // the gold knobs stay Perform's under it (F16)
	display->setNextTransitionDirection(1);
	if (soundEditor.setup(nullptr, menu)) {
		openUI(&soundEditor);
		TEST_EVENT("gen_menu %s", name);
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::assertLeds() {
	assertPageLeds();
#if ENABLE_SYSEX_TEST
	{
		// The page LEDs as the PIC was last told, after the assert: a stock refresh this frame that stayed would show.
		const auto led = [](IndicatorLED which) {
			return indicator_leds::ledStates[static_cast<int32_t>(which)] ? 1 : 0;
		};
		char text[sizeof(shownPageLeds_)];
		snprintf(text, sizeof(text), "song=%d clip=%d keyboard=%d affect=%d cross=%d", led(IndicatorLED::SESSION_VIEW),
		         led(IndicatorLED::CLIP_VIEW), led(IndicatorLED::KEYBOARD), led(IndicatorLED::AFFECT_ENTIRE),
		         led(IndicatorLED::CROSS_SCREEN_EDIT));
		if (strcmp(text, shownPageLeds_) != 0) {
			strcpy(shownPageLeds_, text);
			TEST_EVENT("gen_page_leds %s", text);
		}
	}
#endif
	// SCALE blinks while a key change waits for its line (spec §13.5). A menu's LED setup turns it off as it opens.
	const bool keyWaits = performAdapter.steering().keyChangePending();
	const bool scaleBlinking = indicator_leds::getLedBlinkerIndex(IndicatorLED::SCALE_MODE) != 255;
	if (keyWaits && !scaleBlinking) {
		indicator_leds::blinkLed(IndicatorLED::SCALE_MODE);
	}
	else if (!keyWaits && scaleBlinking) {
		indicator_leds::setLedState(IndicatorLED::SCALE_MODE, false);
	}
	if (keyWaits != scaleBlinkShown_) {
		scaleBlinkShown_ = keyWaits;
		TEST_EVENT("gen_scale_led %s", keyWaits ? "blink" : "off");
	}
	// View::setModLedStates, which a menu runs as it opens and moves, sets F7 and F8 steady: under it the knobs are
	// still on Thin / Drive or Complexity / Motion (F16), so their LEDs blink again.
	const auto f7 = indicator_leds::modLed[kThinDriveModButton];
	if (onThinDrivePage() && indicator_leds::getLedBlinkerIndex(f7) == 255) {
		indicator_leds::blinkLed(f7);
	}
	const auto f8 = indicator_leds::modLed[kMacroModButton];
	if (onMacroPage() && secondMacroSet_ && indicator_leds::getLedBlinkerIndex(f8) == 255) {
		indicator_leds::blinkLed(f8);
	}
}

ActionResult PerformView::exitUI() {
	// Without Capture there's only LEAVE PERFORM? (spec §13.8). Playback goes on under it. Only a BACK hold whose
	// press reached Perform: one whose press closed an overlay (a menu, the preset browser) leaves it at that.
	if (!backDown_) {
		return ActionResult::DEALT_WITH;
	}
	backHeld_ = true;
	if (currentUIMode == UI_MODE_NONE && leavePerform.setupAndCheckAvailability()) {
		// The knobs stay Perform's under it (F16), so they leave the held part now, not when the menu closes.
		letGoForOverlay();
		openUI(&leavePerform);
		TEST_EVENT("gen_leave_menu %s held_ms=%ld", getCurrentUI() == &leavePerform ? "open" : "refused",
		           static_cast<long>(TestPort::injectedHoldMs(deluge::hid::button::BACK)));
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::leave() {
	// Focus is lost on exit (spec §13.1), stopped or not: the knobs leave the part's clip before its song goes.
	dropFocus();
	// The screen reacts at once; the teardown runs behind the working animation, once the runner is free (spec
	// §12.10).
	indicator_leds::setLedState(IndicatorLED::BACK, false);
	// LEAVE stops the transport, which drops a waiting key change (spec §13.2): SCALE goes dark with it.
	indicator_leds::setLedState(IndicatorLED::SCALE_MODE, false);
	scaleBlinkShown_ = false;
	generativeModalUI.open(performAdapter.leaveJob());
	performAdapter.leave([](deluge::generative::JobOutcome outcome) {
		generativeModalUI.jobEnded();
		if (outcome != deluge::generative::JobOutcome::Done) {
			// The teardown's task couldn't start: Perform carries on.
			indicator_leds::setLedState(IndicatorLED::BACK, true);
			display->displayError(Error::UNSPECIFIED);
			return;
		}
		TEST_EVENT("gen_perform_left");
	});
}

namespace {
/// Sets `led` to `on` unless it already is: each write costs UART to the PIC, and this runs every frame.
void setLedIfChanged(IndicatorLED led, bool on) {
	if (indicator_leds::ledStates[static_cast<int32_t>(led)] != on) {
		indicator_leds::setLedState(led, on);
	}
}
} // namespace

void PerformView::assertPageLeds() {
	setLedIfChanged(IndicatorLED::SESSION_VIEW, page_ == Page::Song);
	setLedIfChanged(IndicatorLED::CLIP_VIEW, page_ == Page::Clip);
	setLedIfChanged(IndicatorLED::KEYBOARD, page_ == Page::Keyboard);
	setLedIfChanged(IndicatorLED::AFFECT_ENTIRE, focusedPart() < 0);
	// CROSS-SCREEN: lit while the focused part is frozen (spec §13.5).
	setLedIfChanged(IndicatorLED::CROSS_SCREEN_EDIT, partFrozen(focusedPart()));
	// The F7 LED blinks while Thin / Drive is on, as F8's does for Complexity / Motion. setModLedStates() stops the
	// blink, so ending the set needs nothing here; starting it again would restart the blink's phase.
	const auto f7 = indicator_leds::modLed[kThinDriveModButton];
	if (onThinDrivePage() && indicator_leds::getLedBlinkerIndex(f7) == 255) {
		indicator_leds::blinkLed(f7);
	}
}

ActionResult PerformView::padAction(int32_t x, int32_t y, int32_t velocity) {
	// SHIFT + a main pad: the sound editor at the printed param, on the focused part's Sound or the song FX (spec
	// §13.7), from Perform or under the sound editor and Perform's menus, which hand their pads here (F7).
	if (velocity != 0 && x < kDisplayWidth && Buttons::isShiftButtonPressed()
	    && deluge::generative::perform::shiftOpensSoundEditor(page_, x, rowFromY(y))) {
		return openSoundEditorAtPad(focusedPart(), x, y);
	}
	if (getCurrentUI() == &soundEditor && !editorOpen_) {
		// A Perform menu: a pad press closes it, and does nothing more (spec §13.6).
		if (velocity != 0) {
			soundEditor.exitCompletely();
		}
		return ActionResult::DEALT_WITH;
	}
	if (x >= kDisplayWidth) {
		sidebarPadAction(x, y, velocity != 0);
		refreshSidebar();
	}
	else if (velocity == 0 && heldPart_ >= 0 && x == heldX_ && rowFromY(y) == heldPart_) {
		// The pad holding a part, let go on any page: focus lasts only while it's held (spec §13.5). The CLIP page
		// still shows that part, the last one focused.
		songPadAction(y, false);
		refreshArcMap();
		refreshFaders();
	}
	else if (page_ == Page::Keyboard || (velocity == 0 && keyboardPadHeld(x, y))) {
		// A KEYBOARD pad held when the page changed lets go there, as a part row does.
		keyboardPadAction(x, y, velocity != 0);
	}
	else if (page_ == Page::Clip) {
		return clipPadAction(x, y, velocity != 0);
	}
	else {
		songPadAction(y, velocity != 0);
		if (velocity != 0 && heldPart_ == rowFromY(y)) {
			heldX_ = x;
		}
		refreshArcMap();
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::showMainPads(const RGB pads[kDisplayHeight][kDisplayWidth]) {
	uint32_t changed = 0;
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			if (pads[y][x] != mainPads_[y][x]) {
				mainPads_[y][x] = pads[y][x];
				changed |= 1U << y;
			}
		}
	}
	// Any main-pad change sends the whole grid (21–22 ms of UART, spec §13.5), so redraw only on a change.
	if (changed != 0) {
		uiNeedsRendering(this, changed, 0);
		mainRedraws_++;
	}
}

void PerformView::graphicsRoutine() {
	checkEndHold();
	// Not UI::graphicsRoutine(): it calls the root UI's, which is this one.
	if (peek_ != Peek::None && static_cast<int32_t>(peekUntil_ - AudioEngine::audioSampleTimer) <= 0) {
		peek_ = Peek::None;
	}
	// What the player sees at once when a part's Output is replaced. These are display reactions only: what keeps the
	// UI off an Output being freed is releaseOutput(), which the pump asks by identity (pump.h), whatever the order
	// here.
	bool react = true;
#if ENABLE_SYSEX_TEST
	react = !TestPort::reactionsHeld(); // test hook: the pump's release then does it all, late
#endif
	if (react && soundSwaps.swaps() != seenSwaps_) {
		// A sound re-roll replaced a part's clip: the gold knobs move onto the new one.
		seenSwaps_ = soundSwaps.swaps();
		bindKnobs();
		refreshFaders();
	}
	if (react && transitions.starts() != seenStarts_) {
		// A new roster plays (spec §12.9): focus and a lock drop to song level.
		seenStarts_ = transitions.starts();
		dropFocus();
		refreshFaders();
		TEST_EVENT("gen_focus part=-1 locked=-1 new_roster");
	}
	if (performAdapter.playing()) {
		keepFeedbackRunning(); // the live values' MIDI feedback (spec §13.7)
	}
	if (react) {
		// A sound editor on a part a swap or a new roster replaced closes (spec §12.9, §13.7), once it is on top.
		closeEditorOnRetiredPart();
	}
	// Sound swaps, transitions and retiring Outputs, in task context (spec §12.9).
	Pump::service([](const PartOutput& output) { return performView.releaseOutput(output); });
	refreshPlan();
	refreshSidebar();
	refreshArcMap();
	refreshKeyboardPage();
	if (AudioEngine::audioSampleTimer - fadersAt_ >= kFaderRefreshSamples) {
		refreshFaders();
	}
	else {
		refreshClipPage(); // the queued take and a waiting sound blink
	}
	assertLeds();
	if (getCurrentUI() == &soundEditor) {
		deluge::gui::menu_item::generative::refreshPerformMenu();
	}
	refreshScreen();
#if ENABLE_SYSEX_TEST
	checkKnobRings();
#endif
}

void PerformView::notifyPlaybackBegun() {
	arc_.scrollToNow();
	refreshScreen();
}

void PerformView::playbackEnded() {
	performAdapter.playbackEnded();
	// Focus is performance state, lost on stop (spec §13.1).
	arc_.scrollToNow();
	dropFocus();
	refreshPlan();
	refreshSidebar();
	refreshArcMap();
	refreshScreen();
}

bool PerformView::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                 uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) {
	if (image == nullptr) {
		return true;
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		if (whichRows & (1 << y)) {
			std::copy_n(mainPads_[y], kDisplayWidth, image[y]);
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
			for (int32_t x = 0; x < kSideBarWidth; x++) {
				image[y][kDisplayWidth + x] = sidebar_[y][x];
			}
		}
	}
	return true;
}
