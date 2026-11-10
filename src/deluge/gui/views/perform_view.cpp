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
#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/perform_midi.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"
#include "generative/perform/control_faders.h"
#include "generative/perform/queue_text.h"
#include "gui/colour/palette.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/colour.h"
#include "gui/menu_item/generative/perform_menu.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "hid/matrix/matrix_driver.h"
#include "io/midi/midi_engine.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/mod_controllable/mod_controllable.h"
#include "model/model_stack.h"
#include "model/output.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "util/d_stringbuf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

using deluge::generative::Generator;
using deluge::generative::Genre;
using deluge::generative::kMacroFull;
using deluge::generative::kMacroUnitsPerKnobStep;
using deluge::generative::kNumMacros;
using deluge::generative::kStepsPerBar;
using deluge::generative::kTicksPerBar;
using deluge::generative::kTicksPerBeat;
using deluge::generative::kTicksPerStep;
using deluge::generative::Macro;
using deluge::generative::macroToPoints;
using deluge::generative::SectionKind;
using deluge::generative::SoundControl;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::keepFeedbackRunning;
using deluge::generative::firmware::performAdapter;
using deluge::generative::firmware::retirements;
using deluge::generative::firmware::soundControls;
using deluge::generative::firmware::SoundSwaps;
using deluge::generative::firmware::soundSwaps;
using deluge::generative::firmware::transitions;
using deluge::generative::perform::ArcMap;
using deluge::generative::perform::ArcMapCell;
using deluge::gui::popupIfRefused;
using deluge::gui::refusedWhileChangePending;
using deluge::gui::context_menu::leavePerform;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;
/// F7: pressed again on a focused part, Thin / Drive (spec §13.5).
constexpr uint8_t kThinDriveModButton = 6;
// The sidebar (spec §13.5). The spec counts rows from the top; firmware pad y 0 is the bottom row.
constexpr int32_t kMuteColumn = kDisplayWidth;
constexpr int32_t kSectionColumn = kDisplayWidth + 1;
/// A pad's row counted from the top from 0: MUTE/LAUNCH row 1 + p is part p.
constexpr int32_t rowFromY(int32_t y) {
	return kDisplayHeight - 1 - y;
}
/// AUDITION/SECTION rows 1–3 force these; row 5 holds; row 8 is End.
constexpr SectionKind kForceRows[] = {SectionKind::Groove, SectionKind::Break, SectionKind::Build};
constexpr int32_t kHoldPadY = kDisplayHeight - 5;
/// The End pad. Held this long, it arms End.
constexpr int32_t kEndPadY = 0;
constexpr uint32_t kEndHoldSamples = kSampleRate * 6 / 10;
/// Queued actions blink with this half-period.
constexpr uint32_t kBlinkSamples = kSampleRate * 2 * kFlashTime / 1000;
/// The SELECT-turn peek lasts this long (spec §13.5).
constexpr uint32_t kPeekSamples = kSampleRate * 4;
/// Two presses on a part row this close together lock its focus (the control-layout prototype's).
constexpr uint32_t kDoubleTapSamples = kSampleRate * 35 / 100;
/// The CLIP page's faders are read this often, so a ramping live value moves on them without redrawing the grid every
/// frame (tuning).
constexpr uint32_t kFaderRefreshSamples = kSampleRate / 10;
/// The CLIP page's FREEZE pad, bright while the part is frozen, and NEW SOUND (the control-layout prototype's).
constexpr RGB kFrozenColour = RGB(0, 200, 255);
constexpr RGB kNewSoundColour = RGB(120, 60, 0);
constexpr RGB kSoundEditorColour = RGB(90, 40, 150);
/// A knob position's span on a PART page bar, in pixels.
constexpr int32_t kBarPixels = 38;
/// Each macro's colour on the KEYBOARD page's faders (the control-layout prototype's), indexed by Macro: Energy,
/// Density, Complexity, Motion.
constexpr RGB kMacroColours[] = {RGB(255, 70, 30), RGB(255, 200, 0), RGB(0, 180, 255), RGB(180, 90, 255)};
/// The KEYBOARD page's FILL pads while FILL is up (the control-layout prototype's); white while it is held.
constexpr RGB kFillColour = RGB::monochrome(70);
/// The MACROS page: a row per macro, in the 7 px font with a pixel less between rows than the other pages, so four
/// fit; each bar's left edge and width in pixels.
constexpr int32_t kMacroRowPixels = 8;
constexpr int32_t kMacroBarX = 24;
constexpr int32_t kMacroBarPixels = OLED_MAIN_WIDTH_PIXELS - kMacroBarX - 2;
/// A KEYBOARD fader press let go sooner than this is a tap, a new centre; held longer, it springs back (spec §13.5).
constexpr uint32_t kMacroTapSamples = kSampleRate * deluge::generative::perform::kMacroTapMs / 1000;

/// Each role's colour on the Arc map's part rows (the control-layout prototype's), indexed by Role: Drums, Bass,
/// Chords, Pad, Lead, FX.
constexpr RGB kRoleColours[] = {RGB(255, 40, 40),  RGB(255, 140, 0), RGB(0, 200, 255),
                                RGB(150, 80, 255), RGB(40, 255, 90), RGB(255, 60, 220)};
/// Row 7, Energy against the centre: green at it (within this many points), blue below, orange above.
constexpr int32_t kEnergyAtCentre = 2;
constexpr RGB kEnergyAt = RGB(0, 120, 40);
constexpr RGB kEnergyBelow = RGB(40, 90, 255);
constexpr RGB kEnergyAbove = RGB(255, 130, 0);
/// A colour at `amount` of `range`, from a sixth of it up.
constexpr RGB scaled(RGB colour, int32_t amount, int32_t range) {
	const int32_t level = std::clamp<int32_t>(amount * 256 / range, 40, 256);
	return RGB(colour.r * level / 256, colour.g * level / 256, colour.b * level / 256);
}

/// Each Section kind's colour on the AUDITION/SECTION column (the control-layout prototype's), indexed by
/// SectionKind: Intro, Groove, Break, Build, Outro.
constexpr RGB kSectionColours[] = {RGB(140, 140, 140), RGB(0, 220, 70), RGB(40, 90, 255), RGB(255, 150, 0),
                                   RGB(170, 60, 210)};
constexpr RGB sectionColour(SectionKind kind, bool bright) {
	const RGB colour = kSectionColours[static_cast<int32_t>(kind)];
	return bright ? colour : colour.dim(2);
}

constexpr int32_t kStatusY = OLED_MAIN_TOPMOST_PIXEL;
constexpr int32_t kBodyY = OLED_MAIN_TOPMOST_PIXEL + 12;
constexpr int32_t kMaxChars = OLED_MAIN_WIDTH_PIXELS / kTextSpacingX;
/// A coarse tempo step: 16 an octave, as stock's metronome values (2^(1/16)).
constexpr float kCoarseTempoStep = 1.0442738f;
/// The gold knobs: the upper is encoder 1, the lower 0.
constexpr int32_t kGoldKnobs = 2;
constexpr int32_t kUpperKnob = 1;

/// Indexed by Macro.
constexpr deluge::l10n::String kMacroNames[] = {
    deluge::l10n::String::STRING_FOR_ENERGY,
    deluge::l10n::String::STRING_FOR_DENSITY,
    deluge::l10n::String::STRING_FOR_COMPLEXITY,
    deluge::l10n::String::STRING_FOR_MOTION,
};
/// A macro's initial, as the MACROS page and the KEYBOARD page's test event show it: its l10n name's first letter.
char macroInitial(int32_t macro) {
	return deluge::l10n::get(kMacroNames[macro])[0];
}

/// A param value as a gold knob's position (kKnobMin..kKnobMax) for the param `stack` addresses.
int32_t knobPos(ModelStackWithAutoParam& stack, int32_t value) {
	return stack.paramCollection->paramValueToKnobPos(value, &stack);
}
/// The param `stack` addresses, as a gold knob's position now.
int32_t liveKnobPos(ModelStackWithAutoParam& stack) {
	return knobPos(stack, stack.autoParam->getCurrentValue());
}

/// The F page of whatever the gold knobs are on: the song, or a focused part's Output.
uint8_t* knobMode() {
	ModControllable* modControllable = view.activeModControllableModelStack.modControllable;
	return modControllable ? modControllable->getModKnobMode() : nullptr;
}

} // namespace

#if ENABLE_SYSEX_TEST
namespace {
/// Test builds: the Arc map as last reported (gen_arcmap, gen_arcrow), so only changes are. Forgotten on each entry,
/// so a script's first map reports in full whatever the last script left.
char shownSections[2 * ArcMap::kColumns + 8] = "";
char shownParts[deluge::generative::kMaxParts][ArcMap::kColumns + 1] = {};
} // namespace
#endif

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
	// Presses a menu or overlay cut short lost their releases: a held part row lets go (a lock stays), a held End
	// pad never arms, BACK's release isn't a tap, FILL let go under the overlay ends (spec §8.7: while held), and so
	// does a KEYBOARD fader's hold.
	heldPart_ = -1;
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
	const SidebarState state = readSidebar();
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kSideBarWidth; x++) {
			sidebar_[y][x] = sidebarColour(state, kDisplayWidth + x, y);
		}
	}
	planKey_ = PlanKey{.decidedPhrase = -2}; // never a key refreshPlan() makes, so it works the plan out
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
					popupIfRefused(performAdapter.cancelNewest());
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
			if (xDown_ && !xUsed_ && scroll_ != 0) {
				scroll_ = 0;
				TEST_EVENT("gen_arc_scroll 0");
				refreshArcMap();
				refreshScreen();
			}
			xDown_ = false;
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == AFFECT_ENTIRE) {
		// AFFECT ENTIRE unlocks a locked part: the knobs go back to the Recipe level (spec §13.5).
		if (on && lockedPart_ >= 0) {
			lockedPart_ = -1;
			bindKnobs();
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
		// The focused part's new sound at the phrase line; SHIFT, its previous sound (spec §13.5).
		if (on && focusedPart() >= 0) {
			soundPress(focusedPart(), Buttons::isShiftButtonPressed());
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
		performAdapter.latchFillShift(Buttons::isShiftButtonPressed());
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
	// An overlay opening over a held row lets go of the hold, and a held End pad never arms (spec §13.5). The gold
	// knobs stay Perform's under it (F16).
	releaseEndPad();
	letGoOfHeldPart();
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
	const bool keyWaits = performAdapter.keyChangePending();
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
	// Without Capture there's only LEAVE PERFORM? (spec §13.8). Playback goes on under it.
	backHeld_ = true;
	if (currentUIMode == UI_MODE_NONE && leavePerform.setupAndCheckAvailability()) {
		releaseEndPad();
		// An overlay opening over a held row lets go of the hold; a lock stays (spec §13.5). The knobs stay Perform's
		// under it (F16), so they leave the held part now, not when the menu closes.
		letGoOfHeldPart();
		openUI(&leavePerform);
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
	generativeModalUI.open(false);
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

bool PerformView::onMacroPage() const {
	// F8 holds the macros only at the Recipe level; a focused part's F8 is its own stock page (spec §13.5).
	uint8_t* mode = knobMode();
	return focusedPart() < 0 && mode != nullptr && *mode == kMacroModButton;
}

void PerformView::modButtonAction(uint8_t whichButton, bool on) {
	if (whichButton == kMacroModButton && focusedPart() < 0) {
		if (on) {
			uint8_t* mode = knobMode();
			if (mode == nullptr) {
				return;
			}
			secondMacroSet_ = (*mode == kMacroModButton) && !secondMacroSet_;
			*mode = kMacroModButton;
			view.setModLedStates();
			assertPageLeds();
			updateKnobIndicators();
		}
		return;
	}
	if (on && whichButton == 0 && knobMode() != nullptr && *knobMode() == 0) {
		return; // Perform owns the mod buttons: F1 again never turns the VU meter on (spec §13.5)
	}
	if (on && focusedPart() >= 0) {
		// A focused part's F7 pressed again switches between its stock stutter / CUSTOM 1 and Thin / Drive; any other
		// F page, or F7 pressed from another page, is stock (spec §13.5).
		const bool again = whichButton == kThinDriveModButton && knobMode() != nullptr && *knobMode() == whichButton;
		thinDrivePart_ = again && thinDrivePart_ != focusedPart() ? focusedPart() : -1;
		TEST_EVENT("gen_knobs page=%u thin_drive=%d", static_cast<unsigned>(whichButton + 1),
		           thinDrivePart_ >= 0 ? 1 : 0);
	}
	view.modButtonAction(whichButton, on);
	if (on && onThinDrivePage()) {
		// Stock's popup names stutter / CUSTOM 1, F7's own page: say what the knobs turn now.
		using deluge::generative::firmware::controlName;
		char text[32];
		snprintf(text, sizeof(text), "%s / %s", deluge::l10n::get(controlName(SoundControl::Thin)),
		         deluge::l10n::get(controlName(SoundControl::Drive)));
		display->displayPopup(text);
	}
	assertPageLeds();
	updateKnobIndicators();
}

void PerformView::songPadAction(int32_t y, bool on) {
	const int32_t part = rowFromY(y);
	if (part >= performAdapter.rosterParts()) {
		return;
	}
	if (!on) {
		if (heldPart_ == part) {
			letGoOfHeldPart();
		}
		return;
	}
	// Held, the part is on the knobs, like holding a clip; a double tap locks it, or unlocks it (spec §13.5).
	const uint32_t now = AudioEngine::audioSampleTimer;
	if (lastTapPart_ == part && now - lastTapAt_ < kDoubleTapSamples) {
		lockedPart_ = lockedPart_ == part ? -1 : part;
		heldPart_ = -1;
		lastTapPart_ = -1;
	}
	else {
		heldPart_ = part;
		lastTapPart_ = part;
		lastTapAt_ = now;
	}
	lastFocusedPart_ = part;
	bindKnobs();
}

void PerformView::bindKnobs() {
	const int32_t part = focusedPart();
	if (thinDrivePart_ >= 0 && part != thinDrivePart_) {
		// Focus moved away, or a stop dropped it: Thin / Drive ends, and a part focused again starts on its stock
		// pages (spec §13.5).
		thinDrivePart_ = -1;
		TEST_EVENT("gen_knobs thin_drive=0 refocus");
#if ENABLE_SYSEX_TEST
		// The faders are sent again after it, so a script reads them in a known order: at a stop, the 100 ms
		// refresh can send the homes before this event (drift keeps them moving up to the stop).
		shownFaders_[0] = '\0';
#endif
	}
	InstrumentClip* clip =
	    part >= 0 && performAdapter.active() ? performAdapter.output().roster.parts[part].clip : nullptr;
	if (clip != nullptr) {
		// A Kit part's whole Kit, not one drum: its kit-level pages.
		if (clip->output != nullptr && clip->output->type == OutputType::KIT) {
			clip->affectEntire = true;
		}
		view.setActiveModControllableTimelineCounter(clip);
	}
	else {
		view.setActiveModControllableTimelineCounter(currentSong);
	}
	assertPageLeds();
	updateKnobIndicators();
	TEST_EVENT("gen_focus part=%ld locked=%ld", static_cast<long>(clip != nullptr ? part : -1),
	           static_cast<long>(lockedPart_));
}

void PerformView::letGoOfHeldPart() {
	// Focus lasts only while the row is held (spec §13.5); a lock stays.
	if (heldPart_ >= 0) {
		heldPart_ = -1;
		bindKnobs();
	}
}

void PerformView::dropFocus() {
	heldPart_ = -1;
	lockedPart_ = -1;
	bindKnobs();
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

SoundControl PerformView::thinDriveControl(int32_t whichModEncoder) {
	return whichModEncoder == kUpperKnob ? SoundControl::Thin : SoundControl::Drive;
}

int32_t PerformView::thinDriveRing(int32_t whichModEncoder) const {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(focusedPart(), thinDriveControl(whichModEncoder), memory);
	return stack != nullptr ? liveKnobPos(*stack) - deluge::generative::perform::kKnobMin : 0;
}

int32_t PerformView::macroRing(int32_t whichModEncoder) const {
	return currentRecipe.recipe.macros[knobMacro(whichModEncoder)] / kMacroUnitsPerKnobStep;
}

bool PerformView::takeKnobRings() {
	// Under overlays too: F16 sends their gold knobs here, so they turn Thin and Drive, or the macros, as well.
	const bool thinDrive = onThinDrivePage();
	if (!thinDrive && !onMacroPage()) {
		return false;
	}
	for (int32_t knob = 0; knob < kGoldKnobs; knob++) {
		indicator_leds::setKnobIndicatorLevel(knob, thinDrive ? thinDriveRing(knob) : macroRing(knob));
	}
	return true;
}

Macro PerformView::knobMacro(int32_t whichModEncoder) const {
	if (whichModEncoder == kUpperKnob) {
		return secondMacroSet_ ? Macro::Complexity : Macro::Energy;
	}
	return secondMacroSet_ ? Macro::Motion : Macro::Density;
}

void PerformView::updateKnobIndicators() {
	// Thin and Drive's params, or the macros' centres (spec §13.5); otherwise stock's.
	if (!takeKnobRings()) {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton]);
		view.setKnobIndicatorLevels();
		return;
	}
	if (!onMacroPage()) {
		return;
	}
	// The F8 LED blinks while Complexity / Motion is on.
	if (secondMacroSet_) {
		indicator_leds::blinkLed(indicator_leds::modLed[kMacroModButton]);
	}
	else {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton], true);
		indicator_leds::setLedState(indicator_leds::modLed[kMacroModButton], true);
	}
}

bool PerformView::onThinDrivePage() const {
	const uint8_t* mode = knobMode();
	return focusedPart() >= 0 && focusedPart() == thinDrivePart_ && mode != nullptr && *mode == kThinDriveModButton;
}

void PerformView::thinDriveTurn(int32_t whichModEncoder, int32_t offset) {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(focusedPart(), thinDriveControl(whichModEncoder), memory);
	if (stack == nullptr) {
		return; // the part's preset doesn't use the control: nothing on this knob
	}
	// As a stock knob turns a param (View::modEncoderAction_existentParam), so F3 sees a player edit.
	using deluge::generative::perform::kKnobMax;
	using deluge::generative::perform::kKnobMin;
	const int32_t live = liveKnobPos(*stack);
	const int32_t newKnobPos = std::clamp<int32_t>(live + offset, kKnobMin, kKnobMax);
	if (newKnobPos == live) {
		view.displayModEncoderValuePopup(stack->paramCollection->getParamKind(), stack->paramId, live);
		return;
	}
	setLeadParam(*stack, newKnobPos, false);
}

void PerformView::setLeadParam(ModelStackWithAutoParam& stack, int32_t knobPos, bool asHome) {
	view.displayModEncoderValuePopup(stack.paramCollection->getParamKind(), stack.paramId, knobPos);
	const int32_t value = stack.paramCollection->knobPosToParamValue(knobPos, &stack);
	if (asHome) {
		soundControls.setHome(stack, value);
	}
	else {
		stack.autoParam->setCurrentValueInResponseToUserInput(value, &stack);
	}
	// MIDI-follow feedback of the live value, as stock's knob turn sends it (spec §13.7); only for the part on the gold
	// knobs, which sendFeedback() checks. Follow maps no patch cable, and stock may have deleted one set to 0.
	if (stack.paramCollection->getParamKind() != deluge::modulation::params::Kind::PATCH_CABLE) {
		view.sendMidiFollowFeedback(&stack, liveKnobPos(stack));
	}
	updateKnobIndicators();
	refreshFaders();
	refreshScreen();
}

void PerformView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	if (onThinDrivePage()) {
		thinDriveTurn(whichModEncoder, offset);
		return;
	}
	if (!onMacroPage()) {
		// A focused part's own pages, or the song FX: stock (spec §13.5).
		view.modEncoderAction(whichModEncoder, offset);
		return;
	}
	Macro macro = knobMacro(whichModEncoder);
	int32_t value = currentRecipe.recipe.macros[macro] + offset * kMacroUnitsPerKnobStep;
	performAdapter.setMacro(macro, value);
	TEST_EVENT("gen_macro_knob macro=%d", static_cast<int>(macro));
	updateKnobIndicators();
	showMacroPopup(macro, currentRecipe.recipe.macros[macro]);
}

void PerformView::steerMacroFromMidi(Macro macro, int32_t units) {
	performAdapter.setMacro(macro, units);
	TEST_EVENT("gen_macro_midi macro=%d units=%ld", static_cast<int>(macro),
	           static_cast<long>(currentRecipe.recipe.macros[macro]));
	updateKnobIndicators();
	if (midiEngine.midiFollowDisplayParam) {
		showMacroPopup(macro, currentRecipe.recipe.macros[macro]);
	}
}

void PerformView::showMacroPopup(Macro macro, int32_t units) {
	char popup[24];
	snprintf(popup, sizeof(popup), "%s %d", deluge::l10n::get(kMacroNames[static_cast<int32_t>(macro)]),
	         static_cast<int>(macroToPoints(units)));
	display->displayPopup(popup);
	renderUIsForOled();
}

ActionResult PerformView::padAction(int32_t x, int32_t y, int32_t velocity) {
	// SHIFT + a main pad: the sound editor at the printed param, on the focused part's Sound or the song FX (spec
	// §13.7), from Perform or under the sound editor and Perform's menus, which hand their pads here (F7).
	if (velocity != 0 && x < kDisplayWidth && Buttons::isShiftButtonPressed()
	    && deluge::generative::perform::shiftOpensSoundEditor(page_, x, rowFromY(y))) {
		return openSoundEditor(focusedPart(), x, y);
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
		if (velocity != 0 && rowFromY(y) == deluge::generative::perform::kActionsRow
		    && x == deluge::generative::perform::kSoundEditorPad) {
			// The shown part's root menu; with no part shown, nothing (spec §13.7)
			return lastFocusedPart_ >= 0 ? openSoundEditor(lastFocusedPart_, -1, -1) : ActionResult::DEALT_WITH;
		}
		clipPadAction(x, y, velocity != 0);
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

ActionResult PerformView::openSoundEditor(int32_t part, int32_t x, int32_t y) {
	if (sdRoutineLock) {
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	}
	InstrumentClip* clip = nullptr;
	if (part >= 0) {
		if (!performAdapter.active() || part >= performAdapter.rosterParts()) {
			return ActionResult::DEALT_WITH;
		}
		clip = performAdapter.output().roster.parts[part].clip;
		if (clip == nullptr) {
			return ActionResult::DEALT_WITH;
		}
		// A Kit part's whole Kit, its kit-level FX, as on the gold knobs (spec §13.5).
		if (clip->output != nullptr && clip->output->type == OutputType::KIT) {
			clip->affectEntire = true;
		}
	}
	// The editor anywhere in the UI stack: a shortcut can open a UI over it (OSC 1's file browser opens the sample
	// browser), and the current Clip must stay its part's under that UI.
	const bool wasOpen = isUIOpen(&soundEditor);
	Clip* const wasClip = currentSong->getCurrentClip();
	ModControllableAudio* const wasEditing = soundEditor.currentModControllable;
	MenuItem* const wasItem = wasOpen ? soundEditor.getCurrentMenuItem() : nullptr;
	const bool wasKitFX = soundEditor.setupKitGlobalFXMenu;
	// The stock editor reads the song's current Clip throughout, and Perform has none: while the editor is open on a
	// part, that part's clip is current, which F6 reads as a clip context. focusRegained() clears it.
	currentSong->setCurrentClip(clip);
	soundEditor.setupKitGlobalFXMenu = SoundEditor::shouldEditKitAffectEntire();
	ActionResult result = ActionResult::DEALT_WITH;
	if (x < 0) {
		if (soundEditor.setup(clip)) {
			soundEditor.enterOrUpdateSoundEditor(true);
		}
	}
	else {
		result = soundEditor.potentialShortcutPadAction(x, y, true);
	}
	const bool open = isUIOpen(&soundEditor);
	const bool targetMoved = !wasOpen || soundEditor.currentModControllable != wasEditing;
	if (!open || !targetMoved) {
		// It didn't open, or it's still on the Sound it was on (the param doesn't apply to the new target, or the pad
		// has none): so is the current Clip.
		currentSong->setCurrentClip(open ? wasClip : nullptr);
		soundEditor.setupKitGlobalFXMenu = wasKitFX;
	}
	if (!open || (!targetMoved && soundEditor.getCurrentMenuItem() == wasItem)) {
		// Nothing opened: a Perform menu under it stays a menu, closed by a pad press (spec §13.6).
		TEST_EVENT("gen_editor %s", !open ? "none" : wasClip != nullptr ? "stayed part" : "stayed song");
		return result;
	}
	editorOpen_ = true;
#if ENABLE_SYSEX_TEST
	uint32_t cols = 0;
	uint32_t rows = 0;
	const bool greyout = soundEditor.getGreyoutColsAndRows(&cols, &rows);
	TEST_EVENT("gen_editor open part=%ld kit=%d greyout=%d", static_cast<long>(clip != nullptr ? part : -1),
	           soundEditor.editingKitAffectEntire() ? 1 : 0, greyout ? 1 : 0);
#endif
	return result;
}

void PerformView::closeEditorOnRetiredPart() {
	Clip* const clip = currentSong->getCurrentClip(); // set only while the editor is open on a part
	if (clip == nullptr || !isUIOpen(&soundEditor)) {
		return;
	}
	if (performAdapter.active()) {
		const auto& roster = performAdapter.output().roster;
		for (int32_t p = 0; p < performAdapter.rosterParts(); p++) {
			if (roster.parts[p].clip == clip) {
				return;
			}
		}
	}
	if (getCurrentUI() != &soundEditor) {
		// A UI over the editor (a browser) may still use the Sound: Retirements keeps its Output until the editor is
		// on top again and closes, every frame till then.
		return;
	}
	TEST_EVENT("gen_editor retired");
	soundEditor.exitCompletely();
}

ModelStackWithAutoParam* PerformView::leadParam(int32_t part, SoundControl control, char* memory) {
	const auto* genre = performAdapter.rosterGenre();
	if (!performAdapter.active() || genre == nullptr) {
		return nullptr;
	}
	return soundControls.leadParam(performAdapter.output().roster, *genre, part, control, memory);
}

void PerformView::readControls(int32_t part, ControlLevel levels[deluge::generative::kNumSoundControls]) {
	for (int32_t c = 0; c < deluge::generative::kNumSoundControls; c++) {
		char memory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* stack = leadParam(part, static_cast<SoundControl>(c), memory);
		levels[c] = ControlLevel{};
		if (stack != nullptr) {
			// The live value is the param now: home plus every layer (spec §13.5).
			levels[c].has = true;
			levels[c].home = static_cast<int8_t>(knobPos(*stack, soundControls.homeOf(*stack)));
			levels[c].live = static_cast<int8_t>(liveKnobPos(*stack));
		}
	}
}

void PerformView::clipPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::generative::perform;
	const int32_t row = rowFromY(y);
	const int32_t part = lastFocusedPart_;
	if (!on || part < 0 || part >= performAdapter.rosterParts()) {
		return;
	}
	if (row == kTakesRow) {
		// Tapping a lit take queues it for the next phrase line; the one queued, or the one playing, cancels it.
		if (takeTappable(performAdapter.takeRow(part), x) && !refusedWhileChangePending()) {
			popupIfRefused(performAdapter.pressTake(static_cast<uint8_t>(part), x));
			TEST_EVENT("gen_press take part=%ld take=%ld", static_cast<long>(part), static_cast<long>(x));
		}
		return;
	}
	if (row == kActionsRow) {
		if (x == kFreezePad) {
			freezePress(part);
		}
		else if (x == kNewSoundPad) {
			soundPress(part, Buttons::isShiftButtonPressed());
		}
		return; // SOUND EDITOR: padAction()
	}
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(part, faderControl(row), memory);
	if (stack == nullptr) {
		return;
	}
	// A tap sets home (spec §13.5): the param plays it plus the control's offset now, and the edit is logged.
	setLeadParam(*stack, faderKnobPos(x), true);
}

void PerformView::soundPress(int32_t part, bool previous) {
	using deluge::l10n::String;
	// The stock working animation while it loads, so a refused card action makes sense (spec §12.10). Up before the
	// press: a job whose task can't start ends inside it, and its end takes the animation down.
	const bool alreadyLoading = soundSwaps.preloadRunning();
	if (!alreadyLoading) {
		display->displayLoadingAnimationText(deluge::l10n::get(String::STRING_FOR_LOADING));
	}
	const SoundSwaps::Press press = soundSwaps.press(part, previous);
	if (press != SoundSwaps::Press::Preloading && !alreadyLoading) {
		display->removeWorkingAnimation();
	}
	switch (press) {
	case SoundSwaps::Press::Preloading:
		break;
	case SoundSwaps::Press::Busy:
		display->displayPopup(deluge::l10n::get(String::STRING_FOR_GENERATIVE_BUSY));
		break;
	case SoundSwaps::Press::NoPrevious:
		display->displayPopup(deluge::l10n::get(String::STRING_FOR_NO_PREVIOUS_SOUND));
		break;
	case SoundSwaps::Press::Pending:
		display->displayPopup(deluge::l10n::get(String::STRING_FOR_CHANGE_PENDING));
		break;
	case SoundSwaps::Press::Cancelled:
	case SoundSwaps::Press::Ignored:
		break;
	}
	TEST_EVENT("gen_press sound part=%ld previous=%d result=%d", static_cast<long>(part), previous ? 1 : 0,
	           static_cast<int>(press));
	refreshClipPage();
}

void PerformView::freezePress(int32_t part) {
	popupIfRefused(performAdapter.pressFreeze(static_cast<uint8_t>(part)));
	TEST_EVENT("gen_press freeze part=%ld", static_cast<long>(part));
}

void PerformView::showKeyAndSeed() {
	// The key and scale playing, and the seed code (spec §6.5).
	using namespace deluge::generative::perform;
	const auto& recipe = currentRecipe.recipe;
	char seed[kSeedCodeChars + 1];
	seedCode(currentRecipe.seed, seed);
	char text[40];
	snprintf(text, sizeof(text), "%s %s %s", keyName(recipe.keyRoot), scaleName(recipe.scale), seed);
	display->displayPopup(text);
	TEST_EVENT("gen_seed_popup %s", text);
}

bool PerformView::partFrozen(int32_t part) {
	const Generator* piece = performAdapter.playingGenerator();
	return piece != nullptr && part >= 0 && part < performAdapter.rosterParts() && piece->partFrozen(part);
}

ActionResult PerformView::verticalEncoderAction(int32_t offset, bool /*inCardRoutine*/) {
	// Y turn: the focused part's take back or forward, from the one queued or else the one playing (spec §13.5).
	const int32_t part = focusedPart();
	if (part < 0 || offset == 0 || refusedWhileChangePending()) {
		return ActionResult::DEALT_WITH;
	}
	bool accepted = true;
	const int32_t take =
	    performAdapter.stepTake(static_cast<uint8_t>(part), performAdapter.takeRow(part), offset, &accepted);
	popupIfRefused(accepted);
	if (take >= 0) {
		TEST_EVENT("gen_press take part=%ld take=%ld y=%ld", static_cast<long>(part), static_cast<long>(take),
		           static_cast<long>(offset));
		refreshClipPage();
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

void PerformView::refreshFaders() {
	using namespace deluge::generative::perform;
	fadersAt_ = AudioEngine::audioSampleTimer;
#if !ENABLE_SYSEX_TEST
	if (page_ != Page::Clip) {
		return;
	}
#endif
	readControls(lastFocusedPart_, faderLevels_);
#if ENABLE_SYSEX_TEST
	{
		// Test builds read the faders on every page, for the probe scripts.
		// Each fader's home and live pads (1–16), from row 3 down: Brightness, Thin, Decay, Space, Drive, Wobble.
		// "12=" when the live value is on home's pad, "12/11" when it isn't; " at_home" at the end when every fader's
		// live value is on its home's pad.
		char text[sizeof(shownFaders_)];
		char* at = text + snprintf(text, sizeof(text), "part=%ld", static_cast<long>(lastFocusedPart_));
		bool atHome = true;
		for (int32_t row = kFirstFaderRow; row < kDisplayHeight; row++) {
			const ControlLevel& level = faderLevels_[static_cast<int32_t>(faderControl(row))];
			const int32_t home = faderPad(level.home) + 1;
			const int32_t live = faderPad(level.live) + 1;
			atHome = atHome && (!level.has || live == home);
			at += !level.has     ? snprintf(at, text + sizeof(text) - at, " -")
			      : live == home ? snprintf(at, text + sizeof(text) - at, " %ld=", static_cast<long>(home))
			                     : snprintf(at, text + sizeof(text) - at, " %ld/%ld", static_cast<long>(home),
			                                static_cast<long>(live));
		}
		if (atHome) {
			snprintf(at, text + sizeof(text) - at, " at_home");
		}
		if (strcmp(text, shownFaders_) != 0) {
			strcpy(shownFaders_, text);
			TEST_EVENT("gen_faders %s", text);
		}
	}
#endif
	refreshClipPage();
}

void PerformView::refreshClipPage() {
	using namespace deluge::generative::perform;
	const int32_t part = lastFocusedPart_;
	const auto* genre = performAdapter.rosterGenre();
	const bool shown = part >= 0 && part < performAdapter.rosterParts();
	const TakeRow takes = performAdapter.takeRow(part);
	const bool frozen = partFrozen(part);
	const bool soundBusy = shown && (soundSwaps.preloading(part) || soundSwaps.waiting(part));
#if ENABLE_SYSEX_TEST
	{
		// Test builds describe the CLIP page's rows 1 and 2 on every page: each take pad (P playing, e earlier, n the
		// next new take, q queued, . off), the freeze, and the part's sound (p preloading, w waiting for its line).
		constexpr char kLooks[] = {'.', 'P', 'e', 'n', 'q'}; // TakeLook order
		char looks[deluge::generative::kMaxTakes + 1] = {};
		for (int32_t t = 0; t < deluge::generative::kMaxTakes; t++) {
			looks[t] = shown ? kLooks[static_cast<int32_t>(takeLook(takes, t))] : '.';
		}
		char text[sizeof(shownClip_)];
		snprintf(text, sizeof(text), "part=%ld takes=%s freeze=%d sound=%c", static_cast<long>(part), looks,
		         frozen ? 1 : 0,
		         !shown                        ? '-'
		         : soundSwaps.preloading(part) ? 'p'
		         : soundSwaps.waiting(part)    ? 'w'
		                                       : '-');
		if (strcmp(text, shownClip_) != 0) {
			strcpy(shownClip_, text);
			TEST_EVENT("gen_clip %s", text);
		}
	}
#endif
	if (page_ != Page::Clip) {
		return;
	}
	RGB pads[kDisplayHeight][kDisplayWidth] = {};
	if (shown) {
		const RGB colour = kRoleColours[static_cast<int32_t>(genre->parts[part].role)];
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		// Row 1, the takes: bright playing, dim earlier, grey the next new take, the queued one blinking.
		RGB* takePads = pads[kDisplayHeight - 1 - kTakesRow];
		for (int32_t t = 0; t < deluge::generative::kMaxTakes; t++) {
			switch (takeLook(takes, t)) {
			case TakeLook::Playing:
				takePads[t] = colour;
				break;
			case TakeLook::Earlier:
				takePads[t] = colour.dim(3);
				break;
			case TakeLook::New:
				takePads[t] = RGB::monochrome(40);
				break;
			case TakeLook::Queued:
				takePads[t] = blinkOff ? deluge::gui::colours::black : colour;
				break;
			case TakeLook::Off:
				break;
			}
		}
		// Row 2: FREEZE (bright while frozen), NEW SOUND (blinking while it preloads or waits for its line), SOUND
		// EDITOR.
		RGB* actionPads = pads[kDisplayHeight - 1 - kActionsRow];
		actionPads[kFreezePad] = frozen ? kFrozenColour : kFrozenColour.dim(3);
		actionPads[kNewSoundPad] = soundBusy && blinkOff ? deluge::gui::colours::black : kNewSoundColour;
		actionPads[kSoundEditorPad] = kSoundEditorColour;
		// Rows 3–8, the faders, as refreshFaders() last read them.
		for (int32_t row = kFirstFaderRow; row < kDisplayHeight; row++) {
			const ControlLevel& level = faderLevels_[static_cast<int32_t>(faderControl(row))];
			if (!level.has) {
				continue; // the part's preset doesn't use the control: dark
			}
			const int32_t y = kDisplayHeight - 1 - row;
			for (int32_t x = 0; x < kFaderPads; x++) {
				switch (faderLook(x, faderPad(level.home), faderPad(level.live))) {
				case FaderLook::Live:
					pads[y][x] = RGB(255, 255, 255);
					break;
				case FaderLook::Home:
					pads[y][x] = colour.dim(1);
					break;
				case FaderLook::Off:
					pads[y][x] = colour.dim(4);
					break;
				}
			}
		}
	}
	showMainPads(pads);
}

void PerformView::keyboardPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::generative::perform;
	const KeyboardPadAt at = resolveKeyboardPad(x, rowFromY(y));
	switch (at.kind) {
	case KeyboardPad::Fader: {
		if (!on) {
			releaseMacroPad(at.macro, at.pad);
			break;
		}
		const int32_t units = macroLevelUnits(at.level);
		macroHolds_.press(at.macro, at.pad, units, AudioEngine::audioSampleTimer);
		performAdapter.holdMacro(at.macro, units);
		TEST_EVENT("gen_macro_pad macro=%d hold units=%ld", static_cast<int>(at.macro), static_cast<long>(units));
		showMacroPopup(at.macro, units);
		break;
	}
	case KeyboardPad::Fill:
		holdFill(at.pad, on);
		break;
	case KeyboardPad::None:
		break;
	}
	refreshKeyboardPage();
	refreshScreen();
}

void PerformView::releaseMacroPad(Macro macro, int32_t pad) {
	using deluge::generative::perform::MacroFaderHolds;
	const int32_t units = macroHolds_.units(macro);
	// A tap keeps the value as the new centre; a hold springs back to the centre (spec §13.5).
	switch (macroHolds_.release(macro, pad, AudioEngine::audioSampleTimer, kMacroTapSamples)) {
	case MacroFaderHolds::Release::Keep:
		performAdapter.keepMacroCentre(macro, units);
		TEST_EVENT("gen_macro_pad macro=%d keep units=%ld", static_cast<int>(macro),
		           static_cast<long>(currentRecipe.recipe.macros[macro]));
		break;
	case MacroFaderHolds::Release::SpringBack:
		performAdapter.springMacroBack(macro);
		TEST_EVENT("gen_macro_pad macro=%d spring units=%ld", static_cast<int>(macro),
		           static_cast<long>(currentRecipe.recipe.macros[macro]));
		break;
	case MacroFaderHolds::Release::None:
		return;
	}
	updateKnobIndicators(); // the rings show the centre
}

bool PerformView::keyboardPadHeld(int32_t x, int32_t y) const {
	using namespace deluge::generative::perform;
	const KeyboardPadAt at = resolveKeyboardPad(x, rowFromY(y));
	switch (at.kind) {
	case KeyboardPad::Fader:
		return macroHolds_.held(at.macro, at.pad);
	case KeyboardPad::Fill:
		return fillHolds_.held(at.pad);
	case KeyboardPad::None:
		break;
	}
	return false;
}

void PerformView::releaseLostHolds() {
	using deluge::generative::perform::FillHolds;
	if (fillHolds_.held(FillHolds::kButton) && !Buttons::isButtonPressed(deluge::hid::button::SYNC_SCALING)) {
		holdFill(FillHolds::kButton, false);
	}
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			if (keyboardPadHeld(x, y) && !matrixDriver.isPadPressed(x, y)) {
				keyboardPadAction(x, y, false);
			}
		}
	}
}

void PerformView::refreshKeyboardPage() {
	using namespace deluge::generative::perform;
	if (page_ != Page::Keyboard) {
		return;
	}
	const Generator* piece = performAdapter.playingGenerator();
	// Each fader shows the value in effect, a held one while held, and where the Arc has the macro now.
	int32_t value[kNumMacros];
	int32_t arc[kNumMacros];
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		value[m] = macroLevel(macroHolds_.held(macro) ? macroHolds_.units(macro) : currentRecipe.recipe.macros[macro]);
		arc[m] = piece != nullptr ? macroLevel(piece->effectiveMacro(macro)) : -1;
	}
	// The phrase position, in the playing Section's colour.
	const auto& output = performAdapter.output();
	const bool phraseShown = piece != nullptr && output.sectionKnown;
	const int32_t phraseSteps = performAdapter.phraseSteps();
	const int32_t cell =
	    phraseShown
	        ? phraseCell(std::max<int32_t>(performAdapter.clock(), 0) / kTicksPerStep % phraseSteps, phraseSteps)
	        : -1;
	const bool fill = currentSong->isFillModeActive(); // MIDI FILL too
#if ENABLE_SYSEX_TEST
	// Each fader's value level and the Arc's (1–8 from the bottom; h while held, - when stopped), the phrase cell
	// (1–4 from the top, - when stopped) and FILL.
	char text[sizeof(shownKeyboard_)];
	char* at = text;
	for (int32_t m = 0; m < kNumMacros; m++) {
		at += snprintf(at, text + sizeof(text) - at, "%c=%ld%s/", macroInitial(m), static_cast<long>(value[m] + 1),
		               macroHolds_.held(static_cast<Macro>(m)) ? "h" : "");
		at += arc[m] >= 0 ? snprintf(at, text + sizeof(text) - at, "%ld ", static_cast<long>(arc[m] + 1))
		                  : snprintf(at, text + sizeof(text) - at, "- ");
	}
	if (cell >= 0) {
		snprintf(at, text + sizeof(text) - at, "phrase=%ld fill=%d", static_cast<long>(cell + 1), fill ? 1 : 0);
	}
	else {
		snprintf(at, text + sizeof(text) - at, "phrase=- fill=%d", fill ? 1 : 0);
	}
	if (strcmp(text, shownKeyboard_) != 0) {
		strcpy(shownKeyboard_, text);
		TEST_EVENT("gen_keyboard %s", text);
	}
#endif
	RGB pads[kDisplayHeight][kDisplayWidth] = {};
	for (int32_t x = 0; x < kMacroFaderColumns; x++) {
		const auto macro = faderMacro(x);
		const int32_t m = static_cast<int32_t>(macro);
		const RGB colour = kMacroColours[m];
		for (int32_t level = 0; level < kMacroFaderLevels; level++) {
			switch (macroPadLook(level, value[m], arc[m])) {
			case MacroPadLook::Arc:
				pads[level][x] = RGB(255, 255, 255);
				break;
			case MacroPadLook::Lit:
				pads[level][x] = macroHolds_.held(macro) ? colour : colour.dim(1); // brighter while held
				break;
			case MacroPadLook::Off:
				pads[level][x] = colour.dim(4);
				break;
			}
		}
	}
	if (phraseShown) {
		for (int32_t c = 0; c < kPhraseCells; c++) {
			const RGB colour = sectionColour(output.section, true);
			pads[kDisplayHeight - 1 - c][kPhraseColumn] = c == cell ? colour : c < cell ? colour.dim(1) : colour.dim(4);
		}
	}
	for (int32_t row = kFillFirstRow; row < kFillFirstRow + kFillRows; row++) {
		for (int32_t x = kFillFirstColumn; x < kFillFirstColumn + kFillColumns; x++) {
			pads[kDisplayHeight - 1 - row][x] = fill ? RGB(255, 255, 255) : kFillColour;
		}
	}
	showMainPads(pads);
}

ActionResult PerformView::horizontalEncoderAction(int32_t offset) {
	using namespace deluge::hid::button;
	xUsed_ = Buttons::isButtonPressed(X_ENC) || xUsed_;
	const auto range = deluge::generative::perform::arcMapScrollRange(arcMapInput());
	const int32_t scroll = std::clamp(scroll_ + offset, range.back, range.ahead);
	if (scroll != scroll_) {
		scroll_ = scroll;
		TEST_EVENT("gen_arc_scroll %ld", static_cast<long>(scroll_));
		refreshArcMap();
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::sidebarPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::hid::button;
	if (x == kSectionColumn && y == kEndPadY) {
		if (on) {
			// Held 0.6 s, it arms End; the pad brightens while held (spec §13.5).
			endHeld_ = true;
			endArmedThisHold_ = false;
			endPressedAt_ = AudioEngine::audioSampleTimer; // graphicsRoutine() arms it once held long enough
		}
		else if (endHeld_) {
			endHeld_ = false;
			if (!endArmedThisHold_) {
				// A tap cancels End while it waits.
				performAdapter.cancelEnd();
				TEST_EVENT("gen_end tap");
			}
		}
		return;
	}
	if (!on) {
		return;
	}
	const bool now = Buttons::isShiftButtonPressed();
	if (x == kMuteColumn) {
		const int32_t part = rowFromY(y);
		if (part >= performAdapter.rosterParts()) {
			return;
		}
		// X held + MUTE = solo, as stock (spec §13.5). That X's release doesn't return the Arc map to now.
		const bool solo = Buttons::isButtonPressed(X_ENC);
		xUsed_ = xUsed_ || solo;
		popupIfRefused(solo ? performAdapter.pressSolo(part, now) : performAdapter.pressMute(part, now));
		TEST_EVENT("gen_press %s part=%ld now=%d", solo ? "solo" : "mute", static_cast<long>(part), now ? 1 : 0);
		return;
	}
	if (y == kHoldPadY) {
		popupIfRefused(performAdapter.pressHold());
		TEST_EVENT("gen_press hold");
		return;
	}
	const int32_t row = rowFromY(y);
	if (row < static_cast<int32_t>(std::size(kForceRows))) {
		popupIfRefused(performAdapter.forceSection(kForceRows[row]));
		TEST_EVENT("gen_press force %s", deluge::generative::sectionName(kForceRows[row]));
	}
}

void PerformView::checkEndHold() {
	if (!endHeld_ || endArmedThisHold_) {
		return;
	}
	if (getCurrentUI() != this) {
		releaseEndPad(); // an overlay opened over the hold: its release would never reach us
		return;
	}
	if (AudioEngine::audioSampleTimer - endPressedAt_ >= kEndHoldSamples) {
		endArmedThisHold_ = true;
		if (refusedWhileChangePending()) {
			TEST_EVENT("gen_end refused");
			return;
		}
		performAdapter.armEnd();
		TEST_EVENT("gen_end armed");
	}
}

void PerformView::releaseEndPad() {
	endHeld_ = false;
	endArmedThisHold_ = false;
}

PerformView::SidebarState PerformView::readSidebar() {
	SidebarState state;
	static_assert(std::size(kForceRows) == std::size(state.forced));
	const Generator* piece = performAdapter.playingGenerator();
	state.parts = performAdapter.rosterParts();
	const auto& output = performAdapter.output();
	state.sounding = piece != nullptr && output.sectionKnown;
	state.playing = output.section;
	if (piece == nullptr) {
		return state; // every start is fresh: nothing muted, queued or held
	}
	for (int32_t p = 0; p < state.parts; p++) {
		// Solo first: a soloed-out muted part stays red (spec §13.5).
		state.look[p] = piece->partSoloed(p)                           ? PartLook::Solo
		                : piece->partMuted(p)                          ? PartLook::Muted
		                : piece->anySoloed() || piece->partSilenced(p) ? PartLook::Dim
		                                                               : PartLook::Active;
		state.partQueued[p] = piece->mutePending(p) || piece->soloPending(p);
	}
	for (size_t row = 0; row < std::size(kForceRows); row++) {
		state.forced[row] = piece->sectionForced(kForceRows[row]);
	}
	state.held = piece->arc().held();
	state.endQueued = piece->endQueued();
	return state;
}

RGB PerformView::sidebarColour(const SidebarState& state, int32_t x, int32_t y) const {
	using namespace deluge::gui;
	const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
	const int32_t row = rowFromY(y);

	if (x == kMuteColumn) {
		if (row >= state.parts) {
			return colours::black;
		}
		// Stock colours: green active, red muted, blue solo, dim = silenced by the Arc or soloed out (spec §13.5).
		RGB colour = menu_item::activeColourMenu.getRGB();
		switch (state.look[row]) {
		case PartLook::Solo:
			colour = menu_item::soloColourMenu.getRGB();
			break;
		case PartLook::Muted:
			colour = menu_item::stoppedColourMenu.getRGB();
			break;
		case PartLook::Dim:
			colour = colour.dull();
			break;
		case PartLook::Active:
			break;
		}
		return state.partQueued[row] && blinkOff ? colours::black : colour;
	}

	if (y == kEndPadY) {
		// Bright while held or while the Outro plays; queued, it blinks at the level it has (the prototype's).
		const bool bright = endHeld_ || (state.sounding && state.playing == SectionKind::Outro);
		return state.endQueued && blinkOff ? colours::black : sectionColour(SectionKind::Outro, bright);
	}
	if (y == kHoldPadY) {
		return state.held ? colours::white : RGB::monochrome(40);
	}
	if (row < static_cast<int32_t>(std::size(kForceRows))) {
		const SectionKind kind = kForceRows[row];
		return state.forced[row] && blinkOff ? colours::black
		                                     : sectionColour(kind, state.sounding && state.playing == kind);
	}
	return colours::black;
}

#if ENABLE_SYSEX_TEST
namespace {
/// The sidebar's state as text, for the test port: a letter per part (a active, m muted, s solo, d dim), followed by *
/// while a press waits; the forced Section (G, B, U or -); the hold; End (q while queued). A template: SidebarState is
/// the view's private type.
template <typename State>
void describeSidebar(const State& state, char* out, size_t size) {
	constexpr char kLooks[] = {'a', 'm', 's', 'd'}; // PartLook order
	char mute[2 * deluge::generative::kMaxParts + 1] = {};
	int32_t n = 0;
	for (int32_t p = 0; p < state.parts; p++) {
		mute[n++] = kLooks[static_cast<int32_t>(state.look[p])];
		if (state.partQueued[p]) {
			mute[n++] = '*';
		}
	}
	constexpr char kForceLetters[] = {'G', 'B', 'U'}; // kForceRows order
	char force = '-';
	for (size_t row = 0; row < std::size(kForceRows); row++) {
		force = state.forced[row] ? kForceLetters[row] : force;
	}
	snprintf(out, size, "mute=%s force=%c hold=%d end=%c", mute, force, state.held ? 1 : 0,
	         state.endQueued ? 'q' : '-');
}
char shownSidebarState[48] = "";
} // namespace
#endif

void PerformView::refreshSidebar() {
	const SidebarState state = readSidebar();
#if ENABLE_SYSEX_TEST
	{
		char text[sizeof(shownSidebarState)];
		describeSidebar(state, text, sizeof(text));
		if (strcmp(text, shownSidebarState) != 0) {
			strcpy(shownSidebarState, text);
			TEST_EVENT("gen_sidebar %s", text);
		}
	}
#endif
	uint32_t changed = 0;
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kSideBarWidth; x++) {
			const RGB colour = sidebarColour(state, kDisplayWidth + x, y);
			if (colour != sidebar_[y][x]) {
				sidebar_[y][x] = colour;
				changed |= 1U << y;
			}
		}
	}
	// Redraw only changed rows: a full redraw costs 21–22 ms of UART (spec §13.5).
	if (changed != 0) {
		uiNeedsRendering(this, 0, changed);
	}
}

void PerformView::tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed) {
	using namespace deluge::hid::button;
	auto& recipe = currentRecipe.recipe;

	if (Buttons::isButtonPressed(TAP_TEMPO)) {
		// The swing interval, as stock: the first turn shows it.
		if (display->hasPopupOfType(PopupType::SWING)) {
			if (refusedWhileChangePending()) {
				return;
			}
			int32_t interval = std::clamp<int32_t>(recipe.swingInterval + (offset > 0 ? 1 : -1), 0,
			                                       deluge::generative::kMaxSwingInterval);
			performAdapter.setSwingInterval(interval);
		}
		showSwing();
		return;
	}
	offset = std::clamp<int8_t>(offset, -1, 1);
	if (Buttons::isButtonPressed(X_ENC)) {
		xUsed_ = true;                             // a nudge, so X's release doesn't return the Arc map to now
		playbackHandler.commandNudgeClock(offset); // stock (spec §13.7)
		return;
	}
	if (shiftButtonPressed) {
		if (display->hasPopupOfType(PopupType::SWING)) {
			if (refusedWhileChangePending()) {
				return;
			}
			performAdapter.setSwing(std::clamp<int32_t>(recipe.swing + offset, -deluge::generative::kMaxSwing,
			                                            deluge::generative::kMaxSwing));
		}
		showSwing();
		return;
	}
	if (Buttons::isButtonPressed(LEARN)) {
		return; // F4: the clock-out scale can't change inside Perform (spec §13.7)
	}
	if (playbackHandler.isExternalClockActive()) {
		showTempo(); // marked EXT; the master owns tempo
		return;
	}
	if (display->hasPopupOfType(PopupType::TEMPO)) {
		// Coarse or fine as stock's truth table: the FineTempoKnob setting flips what a push does.
		bool fineSetting =
		    runtimeFeatureSettings.get(RuntimeFeatureSettingType::FineTempoKnob) == RuntimeFeatureStateToggle::On;
		int32_t tempo = recipe.tempo;
		if (fineSetting == encoderButtonPressed) {
			// Coarse: 16 steps an octave, as stock's metronome values.
			tempo = static_cast<int32_t>(
			    static_cast<float>(tempo) * (offset > 0 ? kCoarseTempoStep : 1.0f / kCoarseTempoStep) + 0.5f);
		}
		else {
			tempo = ((tempo + 50) / 100 + offset) * 100; // whole BPM
		}
		setRecipeTempo(tempo);
		return;
	}
	showTempo();
}

void PerformView::setRecipeTempo(int32_t tempo) {
	if (refusedWhileChangePending()) {
		TEST_EVENT("gen_tempo refused");
		return;
	}
	performAdapter.setTempo(std::clamp(tempo, deluge::generative::kMinTempo, deluge::generative::kMaxTempo));
	showTempo();
}

void PerformView::showTempo() {
	// The stock tempo popup, with the Recipe tempo the song glides to; under an external clock the tracked tempo,
	// marked EXT (spec §13.7).
	DEF_STACK_STRING_BUF(text, 30);
	text.append("Tempo: ");
	if (playbackHandler.isExternalClockActive()) {
		playbackHandler.getTempoStringForOLED(playbackHandler.calculateBPMForDisplay(), text);
		text.append(" EXT");
	}
	else {
		playbackHandler.getTempoStringForOLED(static_cast<float>(currentRecipe.recipe.tempo) / 100.f, text);
	}
	display->popupTextTemporary(text.c_str(), PopupType::TEMPO);
	TEST_EVENT("gen_tempo_popup %s", text.c_str());
}

void PerformView::showSwing() {
	// Stock's swing popup, with the Recipe's amount and interval.
	const auto& recipe = currentRecipe.recipe;
	playbackHandler.displaySwingPopup(recipe.swing, deluge::generative::firmware::swingSyncLevel(recipe.swingInterval),
	                                  deluge::generative::kGeneratorTickMagnitude);
}

void PerformView::graphicsRoutine() {
	checkEndHold();
	// Not UI::graphicsRoutine(): it calls the root UI's, which is this one.
	if (peek_ != Peek::None && static_cast<int32_t>(peekUntil_ - AudioEngine::audioSampleTimer) <= 0) {
		peek_ = Peek::None;
	}
	if (soundSwaps.swaps() != boundSwaps_) {
		// A sound re-roll replaced a part's clip: the gold knobs move onto the new one before the old one is freed.
		boundSwaps_ = soundSwaps.swaps();
		bindKnobs();
		refreshFaders();
	}
	if (transitions.starts() != boundStarts_) {
		// A new roster plays (spec §12.9): focus and a lock drop to song level, and a sound editor open on a part
		// closes, before the outgoing Outputs are freed.
		boundStarts_ = transitions.starts();
		dropFocus();
		refreshFaders();
		TEST_EVENT("gen_focus part=-1 locked=-1 new_roster");
	}
	if (performAdapter.playing()) {
		keepFeedbackRunning(); // the live values' MIDI feedback (spec §13.7)
	}
	soundSwaps.service();
	transitions.service();
	// A sound editor on a part a swap or a new roster replaced closes before its Output is freed (spec §12.9, §13.7).
	// Under another UI it can't yet, so its Output waits.
	closeEditorOnRetiredPart();
	retirements.service(static_cast<InstrumentClip*>(currentSong->getCurrentClip()));
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

#if ENABLE_SYSEX_TEST
namespace indicator_leds {
extern uint8_t knobIndicatorLevels[]; // what the rings show (indicator_leds.cpp)
}

void PerformView::checkKnobRings() {
	// Whether the rings show what the knobs turn, read back from the LEDs' state (overlays included): on Thin / Drive
	// "ok", or "stale" if a stock refresh put the stutter / CUSTOM 1 levels back; on the macros "macros ok" or
	// "macros stale" (stock's song-level levels).
	char text[sizeof(shownRings_)] = "";
	const bool thinDrive = onThinDrivePage();
	if (thinDrive || onMacroPage()) {
		bool ok = true;
		for (int32_t knob = 0; knob < kGoldKnobs; knob++) {
			ok = ok && indicator_leds::knobIndicatorLevels[knob] == (thinDrive ? thinDriveRing(knob) : macroRing(knob));
		}
		snprintf(text, sizeof(text), "%s%s upper=%ld lower=%ld", thinDrive ? "" : "macros ", ok ? "ok" : "stale",
		         static_cast<long>(indicator_leds::knobIndicatorLevels[kUpperKnob]),
		         static_cast<long>(indicator_leds::knobIndicatorLevels[1 - kUpperKnob]));
	}
	if (strcmp(text, shownRings_) != 0) {
		strcpy(shownRings_, text);
		if (text[0] != '\0') {
			TEST_EVENT("gen_rings %s", text);
		}
	}
}
#endif

void PerformView::refreshPlan() {
	using namespace deluge::generative;
	const Generator* piece = performAdapter.playingGenerator();
	PlanKey key;
	key.rosters = transitions.starts(); // a new roster's phrases count from 0 again, maybe the same index as before
	if (piece != nullptr) {
		// A phrase line passing on the decided side, or anything queued, held, muted or soloed changing.
		key.playing = true;
		key.decidedPhrase = std::max<int32_t>(piece->nextStep() - 1, 0) / piece->recipe().genre->phraseSteps();
		uint32_t hash = 2166136261U;
		const auto mix = [&](uint32_t value) { hash = (hash ^ value) * 16777619U; };
		for (int32_t i = 0; i < piece->queuedCount(); i++) {
			const Command& command = piece->queued(i);
			mix(static_cast<uint32_t>(command.kind) << 24 | static_cast<uint32_t>(command.target) << 16);
			mix(static_cast<uint32_t>(command.value));
		}
		uint32_t parts = piece->arc().held() ? 1U : 0U;
		for (int32_t p = 0; p < partCount(*piece->recipe().genre); p++) {
			parts |= (piece->partMuted(p) ? 2U : 0U) << (2 * p);
			parts |= (piece->partSoloed(p) ? 4U : 0U) << (2 * p);
		}
		mix(parts);
		mix(static_cast<uint32_t>(piece->queuedCount()));
		key.steering = hash;
	}
	if (key == planKey_) {
		return;
	}
	planKey_ = key;
	if (piece == nullptr) {
		plan_ = ArcPlan{};
		queuedCount_ = 0;
		return;
	}
	[[maybe_unused]] const uint32_t start = getSystemTicks();
	piece->planArc(plan_);
	queuedCount_ = perform::queuedByTime(*piece, queued_);
	TEST_EVENT("gen_arcplan first=%ld us=%lu", static_cast<long>(plan_.first),
	           static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000000)));
}

deluge::generative::perform::ArcMapInput PerformView::arcMapInput() const {
	using namespace deluge::generative;
	perform::ArcMapInput in;
	const Generator* piece = performAdapter.playingGenerator();
	if (piece == nullptr) {
		return in;
	}
	in.playing = true;
	in.clock = std::max<int32_t>(performAdapter.clock(), 0);
	in.phraseSteps = piece->recipe().genre->phraseSteps();
	in.parts = partCount(*piece->recipe().genre);
	in.scroll = scroll_;
	in.focus = static_cast<int8_t>(focusedPart());
	in.hitFrom = hitFrom_;
	in.hitTo = hitTo_;
	in.history = &performAdapter.history();
	in.plan = &plan_;
	in.queued = std::span<const perform::QueuedAction>(queued_, queuedCount_);
	return in;
}

RGB PerformView::arcMapColour(const ArcMapCell& cell, bool blinkOff) const {
	using namespace deluge::gui;
	RGB colour = colours::black;
	const bool planned = cell.when == ArcMapCell::When::Planned;
	switch (cell.look) {
	case ArcMapCell::Look::Off:
		break;
	case ArcMapCell::Look::Part: {
		// Bright = sounded, the now column flashing on each note, dim = planned to sound (spec §13.5).
		const auto* genre = performAdapter.playingGenerator() != nullptr
		                        ? performAdapter.playingGenerator()->recipe().genre
		                        : currentRecipe.recipe.genre;
		const RGB role =
		    genre != nullptr ? kRoleColours[static_cast<int32_t>(genre->parts[cell.part].role)] : colours::white;
		colour = cell.hit ? role : planned ? role.dim(4) : role.dim(1);
		break;
	}
	case ArcMapCell::Look::Muted:
		colour = RGB(30, 0, 0);
		break;
	case ArcMapCell::Look::Focus:
		colour = colours::white;
		break;
	case ArcMapCell::Look::Energy:
		colour = std::abs(cell.energy) < kEnergyAtCentre ? kEnergyAt
		         : cell.energy < 0                       ? scaled(kEnergyBelow, -cell.energy, 20)
		                                                 : scaled(kEnergyAbove, cell.energy, 14);
		colour = planned ? colour.dim(1) : colour;
		break;
	case ArcMapCell::Look::Section:
		colour = cell.when == ArcMapCell::When::Now ? sectionColour(cell.kind, true)
		         : planned                          ? sectionColour(cell.kind, cell.drop).dim(cell.drop ? 1 : 2)
		                                            : sectionColour(cell.kind, false);
		break;
	}
	if (cell.blink) {
		// A queued action blinks where it lands; on a dark pad it blinks grey.
		return blinkOff ? colours::black : colour == colours::black ? RGB::monochrome(40) : colour;
	}
	return colour;
}

#if ENABLE_SYSEX_TEST
namespace {
/// The Arc map as text for the test port, one character per column: the Section row (I G B U O, D for a Drop, . for
/// nothing) and where queued actions blink (x), then a part's row (. off, h heard, n now, p planned, m muted, f
/// focused).
void describeSections(const ArcMap& map, char* out) {
	constexpr char kKinds[] = {'I', 'G', 'B', 'U', 'O'};
	char* blink = out + ArcMap::kColumns + 3;
	strcpy(out, "s=");
	for (int32_t c = 0; c < ArcMap::kColumns; c++) {
		const ArcMapCell& cell = map.cell[ArcMap::kSectionRow][c];
		out[2 + c] = cell.look == ArcMapCell::Look::Off ? '.'
		             : cell.drop                        ? 'D'
		                                                : kKinds[static_cast<int32_t>(cell.kind)];
		bool blinks = false;
		for (int32_t r = 0; r < ArcMap::kRows; r++) {
			blinks = blinks || map.cell[r][c].blink;
		}
		blink[2 + c] = blinks ? 'x' : '.';
	}
	out[2 + ArcMap::kColumns] = ' ';
	blink[0] = 'q';
	blink[1] = '=';
	blink[2 + ArcMap::kColumns] = '\0';
}
void describePart(const ArcMap& map, int32_t row, char* out) {
	for (int32_t c = 0; c < ArcMap::kColumns; c++) {
		const ArcMapCell& cell = map.cell[row][c];
		switch (cell.look) {
		case ArcMapCell::Look::Part:
			out[c] = cell.when == ArcMapCell::When::Played ? 'h' : cell.when == ArcMapCell::When::Now ? 'n' : 'p';
			break;
		case ArcMapCell::Look::Muted:
			out[c] = 'm';
			break;
		case ArcMapCell::Look::Focus:
			out[c] = 'f';
			break;
		default:
			out[c] = '.';
			break;
		}
	}
	out[ArcMap::kColumns] = '\0';
}
} // namespace
#endif

void PerformView::refreshArcMap() {
	// The flash changes only when the heard step does: notes in the step before and in this one so far.
	const int32_t clock = std::max<int32_t>(performAdapter.clock(), 0);
	if (clock / kTicksPerStep != hitStep_) {
		hitStep_ = clock / kTicksPerStep;
		hitFrom_ = (hitStep_ - 1) * kTicksPerStep;
		hitTo_ = clock;
	}
	// The plan can get shorter (End queued) and the history moves on at each phrase line: keep the scroll inside
	// what's known, every refresh.
	auto in = arcMapInput();
	const auto range = deluge::generative::perform::arcMapScrollRange(in);
	const int32_t scroll = std::clamp(scroll_, range.back, range.ahead);
	if (scroll != scroll_) {
		scroll_ = scroll;
		in.scroll = scroll;
		TEST_EVENT("gen_arc_scroll %ld", static_cast<long>(scroll_));
	}
	ArcMap map;
	deluge::generative::perform::composeArcMap(in, map);
	if (page_ == Page::Song) {
		// Redrawn only on a change: at most each step (the now column's flash), each blink and each phrase line.
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		RGB pads[kDisplayHeight][kDisplayWidth];
		for (int32_t row = 0; row < ArcMap::kRows; row++) {
			const int32_t y = kDisplayHeight - 1 - row; // firmware y 0 is the bottom row
			for (int32_t x = 0; x < kDisplayWidth; x++) {
				pads[y][x] = arcMapColour(map.cell[row][x], blinkOff);
			}
		}
		showMainPads(pads);
	}
#if ENABLE_SYSEX_TEST
	{
		const int32_t phrase = in.playing ? in.clock / (in.phraseSteps * kTicksPerStep) : -1;
		if (phrase != redrawsPhrase_) {
			if (redrawsPhrase_ >= 0) {
				TEST_EVENT("gen_pad_redraws phrase=%ld main=%lu", static_cast<long>(redrawsPhrase_),
				           static_cast<unsigned long>(mainRedraws_));
			}
			redrawsPhrase_ = phrase;
			mainRedraws_ = 0;
		}
		char text[sizeof(shownSections)];
		describeSections(map, text);
		if (strcmp(text, shownSections) != 0) {
			strcpy(shownSections, text);
			TEST_EVENT("gen_arcmap %s", text);
		}
		for (int32_t p = 0; p < in.parts; p++) {
			char row[ArcMap::kColumns + 1];
			describePart(map, p, row);
			if (strcmp(row, shownParts[p]) != 0) {
				strcpy(shownParts[p], row);
				TEST_EVENT("gen_arcrow %ld %s", static_cast<long>(p), row);
			}
		}
	}
#endif
}

void PerformView::refreshScreen() {
	Screen screen;
	compose(screen);
	if (!(screen == screen_)) {
#if ENABLE_SYSEX_TEST
		if (strcmp(screen.left, screen_.left) != 0 || strcmp(screen.right, screen_.right) != 0) {
			TEST_EVENT("gen_status %s|%s", screen.left, screen.right);
		}
		for (int32_t i = 0; i < Screen::kLines; i++) {
			if (peek_ != Peek::None
			    && (strcmp(screen.lines[i], screen_.lines[i]) != 0
			        || strcmp(screen.lineRight[i], screen_.lineRight[i]) != 0)) {
				TEST_EVENT("gen_peek %ld %s|%s", static_cast<long>(i), screen.lines[i], screen.lineRight[i]);
			}
		}
		if (screen.arc) {
			// The ARC page without its blink phase: each box's tag (-- once stopped), * on a Drop, then the next
			// change.
			char boxes[3 * deluge::generative::perform::ArcPage::kBoxes + 1] = {};
			char* at = boxes;
			for (const auto& box : screen.arcPage.box) {
				// A box with nothing known (past the plan, not ended) has an empty tag: '.' keeps the text whole.
				*at++ = box.ended ? '-' : box.tag[0] != '\0' ? box.tag[0] : '.';
				*at++ = box.ended ? '-' : box.tag[1] != '\0' ? box.tag[1] : '.';
				*at++ = box.drop ? '*' : ' ';
			}
			static char shown[sizeof(boxes) + sizeof(screen.arcPage.next) + 1] = "";
			char text[sizeof(shown)];
			snprintf(text, sizeof(text), "%s|%s", boxes, screen.arcPage.next);
			if (strcmp(text, shown) != 0) {
				strcpy(shown, text);
				TEST_EVENT("gen_arcpage %s", text);
			}
		}
#endif
#if ENABLE_SYSEX_TEST
		if (screen.macros
		    && !(screen_.macros
		         && std::equal(std::begin(screen.macroCentre), std::end(screen.macroCentre),
		                       std::begin(screen_.macroCentre))
		         && std::equal(std::begin(screen.macroArc), std::end(screen.macroArc), std::begin(screen_.macroArc)))) {
			// The MACROS page in points: each macro's centre, then where the Arc has it (- when stopped).
			char text[48];
			char* at = text;
			for (int32_t m = 0; m < kNumMacros; m++) {
				at += snprintf(at, text + sizeof(text) - at, m == 0 ? "%d/" : " %d/",
				               static_cast<int>(macroToPoints(screen.macroCentre[m])));
				at += screen.macroArc[m] >= 0 ? snprintf(at, text + sizeof(text) - at, "%d",
				                                         static_cast<int>(macroToPoints(screen.macroArc[m])))
				                              : snprintf(at, text + sizeof(text) - at, "-");
			}
			TEST_EVENT("gen_macropage %s", text);
		}
		if (screen.part && (strcmp(screen.sound, screen_.sound) != 0 || strcmp(screen.take, screen_.take) != 0)) {
			TEST_EVENT("gen_partpage %s|%s", screen.sound, screen.take);
		}
#endif
		screen_ = screen;
		renderUIsForOled();
	}
}

void PerformView::selectEncoderAction(int8_t offset) {
	// QUEUE, RECIPE, round and round; each turn shows the peek for 4 s more (spec §13.5).
	if (peek_ == Peek::None) {
		peek_ = offset > 0 ? Peek::Queue : Peek::Recipe;
	}
	else {
		peek_ = peek_ == Peek::Queue ? Peek::Recipe : Peek::Queue;
	}
	peekUntil_ = AudioEngine::audioSampleTimer + kPeekSamples;
	TEST_EVENT("gen_peek_page %s", peek_ == Peek::Queue ? "QUEUE" : "RECIPE");
	refreshScreen();
}

void PerformView::compose(Screen& screen) {
	using namespace deluge::generative;
	screen = Screen{};
	const Generator* piece = performAdapter.playingGenerator();
	const auto& output = performAdapter.output();
	const Genre* genre = currentRecipe.recipe.genre;
	const int32_t phraseTicks = performAdapter.phraseSteps() * kTicksPerStep;
	const int32_t clock = std::max<int32_t>(performAdapter.clock(), 0);

	// The status line (spec §13.5). Left: Section and its phrase count, HELD; STOPPED when stopped.
	char held[8] = "";
	if (piece == nullptr) {
		snprintf(screen.left, sizeof(screen.left), "%s", l10n::get(l10n::String::STRING_FOR_STOPPED));
	}
	else if (!output.sectionKnown) {
		snprintf(screen.left, sizeof(screen.left), "%s", l10n::get(l10n::String::STRING_FOR_STARTING));
	}
	else {
		// The Section's length, read while the decided side is still in it (it runs 12 ticks ahead).
		const Arc& arc = piece->arc();
		if (arc.section() == output.section && arc.occurrence() == output.occurrence) {
			lengthSection_ = static_cast<int32_t>(output.section);
			lengthOccurrence_ = output.occurrence;
			sectionPhrases_ = (arc.sectionSteps() * kTicksPerStep + phraseTicks - 1) / phraseTicks;
		}
		const bool lengthKnown =
		    lengthSection_ == static_cast<int32_t>(output.section) && lengthOccurrence_ == output.occurrence;
		const int32_t phrase = std::max<int32_t>(clock - output.sectionTick, 0) / phraseTicks + 1;
		if (lengthKnown && sectionPhrases_ > 0) {
			snprintf(screen.left, sizeof(screen.left), "%s %ld/%ld", sectionName(output.section),
			         static_cast<long>(std::min(phrase, sectionPhrases_)), static_cast<long>(sectionPhrases_));
		}
		else {
			snprintf(screen.left, sizeof(screen.left), "%s %ld", sectionName(output.section),
			         static_cast<long>(phrase));
		}
		if (arc.held()) {
			snprintf(held, sizeof(held), " %s", l10n::get(l10n::String::STRING_FOR_HELD));
		}
	}

	// Right: the next queued action as an inverted tag, or bar.beat in the phrase; EXT under an external clock.
	screen.ext = playbackHandler.isExternalClockActive();
	const std::span<const perform::QueuedAction> queued(queued_, piece != nullptr ? queuedCount_ : 0);
	if (piece != nullptr) {
		perform::statusTag(queued, performAdapter.clock(), screen.right);
		screen.tag = screen.right[0] != '\0';
		const int32_t inPhrase = clock % phraseTicks;
		if (!screen.tag) {
			snprintf(screen.right, sizeof(screen.right), "%ld.%ld", static_cast<long>(inPhrase / kTicksPerBar + 1),
			         static_cast<long>(inPhrase % kTicksPerBar / kTicksPerBeat + 1));
		}
		screen.phraseBar = static_cast<int16_t>(OLED_MAIN_WIDTH_PIXELS * inPhrase / phraseTicks);
	}
	// The status line is in capitals, as the Section names are.
	for (char& c : screen.left) {
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
	}
	// HELD in full if it fits beside the right side, or else H, or nothing rather than overlap it.
	const int32_t rightChars = static_cast<int32_t>(strlen(screen.right)) + (screen.ext ? 4 : 0) + 1;
	if (held[0] != '\0') {
		const int32_t room = kMaxChars - rightChars - static_cast<int32_t>(strlen(screen.left));
		const char* shown = static_cast<int32_t>(strlen(held)) <= room ? held : room >= 2 ? " H" : "";
		strncat(screen.left, shown, sizeof(screen.left) - strlen(screen.left) - 1);
	}

	// Below it: a peek, or the page following the grid page: SONG → ARC (spec §13.5). Stopped, the Recipe's macros.
	const auto& recipe = currentRecipe.recipe;
	if (peek_ == Peek::Queue) {
		if (piece != nullptr && genre != nullptr) {
			perform::QueueLines queue;
			perform::queueLines(queued, performAdapter.clock(), *genre, queue);
			static_assert(perform::QueueLines::kLines == Screen::kLines);
			for (int32_t i = 0; i < Screen::kLines; i++) {
				strcpy(screen.lines[i], queue.label[i]);
				strcpy(screen.lineRight[i], queue.when[i]);
			}
		}
		else {
			strcpy(screen.lines[0], perform::kNothingQueued);
		}
	}
	else if (peek_ == Peek::Recipe) {
		snprintf(screen.lines[0], sizeof(screen.lines[0]), "%s",
		         genre != nullptr ? genre->oledName : l10n::get(l10n::String::STRING_FOR_PERFORM));
		snprintf(screen.lines[1], sizeof(screen.lines[1]), "%ld BPM SW %ld", static_cast<long>(recipe.tempo / 100),
		         static_cast<long>(recipe.swing));
		snprintf(screen.lines[2], sizeof(screen.lines[2]), "%s %s", perform::keyName(recipe.keyRoot),
		         perform::scaleName(recipe.scale));
	}
	else if (page_ == Page::Clip) {
		// PART: the CLIP page's part's sound, take and six controls (spec §13.5).
		screen.part = true;
		std::copy_n(faderLevels_, deluge::generative::kNumSoundControls,
		            screen.controls); // as refreshFaders() read them
		composePartTitle(screen);
	}
	else if (page_ == Page::Keyboard) {
		// MACROS: each macro's centre, and where the Arc has it (spec §13.5).
		screen.macros = true;
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			screen.macroCentre[m] = static_cast<int16_t>(recipe.macros[macro]);
			screen.macroArc[m] = static_cast<int16_t>(piece != nullptr ? piece->effectiveMacro(macro) : -1);
		}
	}
	else if (piece != nullptr) {
		screen.arc = true;
		perform::composeArcPage(arcMapInput(), screen.arcPage);
		// Queued boxes blink: the screen changes with the phase, so it redraws.
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		for (perform::ArcBox& box : screen.arcPage.box) {
			box.blink = box.blink && !blinkOff;
		}
	}
	else {
		snprintf(screen.lines[0], sizeof(screen.lines[0]), "%s",
		         genre != nullptr ? genre->oledName : l10n::get(l10n::String::STRING_FOR_PERFORM));
		const auto& macros = recipe.macros;
		snprintf(screen.lines[1], sizeof(screen.lines[1]), "E%d D%d C%d M%d",
		         static_cast<int>(macroToPoints(macros[Macro::Energy])),
		         static_cast<int>(macroToPoints(macros[Macro::Density])),
		         static_cast<int>(macroToPoints(macros[Macro::Complexity])),
		         static_cast<int>(macroToPoints(macros[Macro::Motion])));
	}
}

void PerformView::composePartTitle(Screen& screen) const {
	using namespace deluge::generative;
	const int32_t part = lastFocusedPart_;
	if (part < 0 || part >= performAdapter.rosterParts()) {
		return;
	}
	const Genre* genre = performAdapter.rosterGenre();
	// The part's name, then a synth part's preset as its file name: "ACID 165 Acid Arp". A Kit part has no one
	// pick (each lane has its own sample, spec §13.5), so it shows its name only.
	const char* preset = currentRecipe.sounds.part[part].c_str();
	const char* slash = strrchr(preset, '/');
	const char* name = slash != nullptr ? slash + 1 : preset;
	const char* dot = strrchr(name, '.');
	const int32_t nameLength = dot != nullptr ? static_cast<int32_t>(dot - name) : static_cast<int32_t>(strlen(name));
	perform::takeLabel(performAdapter.takeRow(part), partFrozen(part), screen.take);
	// Cut to leave the take its room on the line.
	const int32_t room = std::min<int32_t>(static_cast<int32_t>(sizeof(screen.sound)) - 1,
	                                       kMaxChars - 1 - static_cast<int32_t>(strlen(screen.take)));
	if (genre->parts[part].lanes.empty()) {
		snprintf(screen.sound, static_cast<size_t>(room) + 1, "%s %.*s", genre->parts[part].name, nameLength, name);
	}
	else {
		snprintf(screen.sound, static_cast<size_t>(room) + 1, "%s", genre->parts[part].name);
	}
}

void PerformView::notifyPlaybackBegun() {
	scroll_ = 0;
	refreshScreen();
}

void PerformView::playbackEnded() {
	performAdapter.playbackEnded();
	// Focus is performance state, lost on stop (spec §13.1).
	scroll_ = 0;
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

void PerformView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	// The status line (spec §13.5): left the Section, right a tag or bar.beat, then EXT; the phrase bar under it.
	canvas.drawString(screen_.left, 0, kStatusY, kTextSpacingX, kTextSpacingY);
	int32_t right = OLED_MAIN_WIDTH_PIXELS;
	if (screen_.ext) {
		canvas.drawStringAlignRight("EXT", kStatusY, kTextSpacingX, kTextSpacingY, right);
		right -= 4 * kTextSpacingX;
	}
	if (screen_.right[0] != '\0') {
		canvas.drawStringAlignRight(screen_.right, kStatusY, kTextSpacingX, kTextSpacingY, right);
		if (screen_.tag) {
			const int32_t width = static_cast<int32_t>(strlen(screen_.right)) * kTextSpacingX;
			canvas.invertArea(right - width - 1, width + 1, kStatusY - 1, kStatusY + 7);
		}
	}
	if (screen_.phraseBar > 0) {
		canvas.drawHorizontalLine(kStatusY + 9, 0, screen_.phraseBar - 1);
	}

	if (screen_.arc) {
		// ARC: now and the next 7 phrases as boxes, now inverted, queued ones blinking, a bar for a Drop; then the
		// Arc's next change (spec §13.5).
		constexpr int32_t kBoxWidth = OLED_MAIN_WIDTH_PIXELS / deluge::generative::perform::ArcPage::kBoxes;
		constexpr int32_t kBoxHeight = 11;
		for (int32_t i = 0; i < deluge::generative::perform::ArcPage::kBoxes; i++) {
			const auto& box = screen_.arcPage.box[i];
			const int32_t x = i * kBoxWidth;
			if (box.ended) {
				continue;
			}
			// The tag's two glyphs take x+1 to x+11; the Drop bar sits clear of them, inside the box's right edge.
			canvas.drawRectangle(x, kBodyY, x + kBoxWidth - 2, kBodyY + kBoxHeight - 1);
			canvas.drawString(box.tag, x + 1, kBodyY + 2, kTextSpacingX, kTextSpacingY);
			if (box.drop) {
				canvas.drawVerticalLine(x + kBoxWidth - 3, kBodyY + 1, kBodyY + kBoxHeight - 2);
			}
			if (box.now != box.blink) {
				canvas.invertArea(x, kBoxWidth - 1, kBodyY, kBodyY + kBoxHeight - 1);
			}
		}
		canvas.drawString(screen_.arcPage.next, 0, kBodyY + kBoxHeight + 4, kTextSpacingX, kTextSpacingY);
		return;
	}
	if (screen_.part) {
		// PART: the part's sound and take n/m (* when frozen), then two columns of three controls, in the CLIP page's
		// order; each a label, a bar filled to home and a tick at the live value.
		canvas.drawString(screen_.sound, 0, kBodyY, kTextSpacingX, kTextSpacingY);
		canvas.drawStringAlignRight(screen_.take, kBodyY, kTextSpacingX, kTextSpacingY);
		for (int32_t i = 0; i < deluge::generative::kNumSoundControls; i++) {
			const SoundControl control =
			    deluge::generative::perform::faderControl(i + deluge::generative::perform::kFirstFaderRow);
			const ControlLevel& level = screen_.controls[static_cast<int32_t>(control)];
			const int32_t x = (i % 2) * (OLED_MAIN_WIDTH_PIXELS / 2);
			const int32_t y = kBodyY + (1 + i / 2) * kTextSpacingY;
			char label[4];
			deluge::generative::firmware::upperControlName(control, label);
			canvas.drawString(label, x, y, kTextSpacingX, kTextSpacingY);
			if (!level.has) {
				continue;
			}
			canvas.drawRectangle(x + 20, y + 1, x + 21 + kBarPixels, y + 5);
			using deluge::generative::perform::kKnobMin;
			using deluge::generative::perform::kKnobSpan;
			const int32_t home = (level.home - kKnobMin) * kBarPixels / kKnobSpan;
			const int32_t live = (level.live - kKnobMin) * kBarPixels / kKnobSpan;
			if (home > 0) {
				canvas.invertArea(x + 21, home, y + 2, y + 4);
			}
			canvas.drawVerticalLine(x + 21 + live, y, y + 6);
		}
		return;
	}
	if (screen_.macros) {
		// MACROS: a row per macro, its initial and centre in points, then a bar filled to the centre with a tick where
		// the Arc has it (spec §13.5).
		for (int32_t m = 0; m < kNumMacros; m++) {
			const int32_t y = kBodyY - 1 + m * kMacroRowPixels;
			char label[8];
			snprintf(label, sizeof(label), "%c%d", macroInitial(m),
			         static_cast<int>(macroToPoints(screen_.macroCentre[m])));
			canvas.drawString(label, 0, y, kTextSpacingX, kTextSpacingY);
			canvas.drawRectangle(kMacroBarX, y + 1, kMacroBarX + kMacroBarPixels + 1, y + 5);
			const int32_t centre = screen_.macroCentre[m] * kMacroBarPixels / kMacroFull;
			if (centre > 0) {
				canvas.invertArea(kMacroBarX + 1, centre, y + 2, y + 4);
			}
			if (screen_.macroArc[m] >= 0) {
				// 2 px wide, inside the bar's borders at 0 and 50.
				const int32_t tick =
				    std::clamp<int32_t>(kMacroBarX + screen_.macroArc[m] * kMacroBarPixels / kMacroFull, kMacroBarX + 1,
				                        kMacroBarX + kMacroBarPixels - 1);
				canvas.invertArea(tick, 2, y, y + 6);
			}
		}
		return;
	}
	for (int32_t i = 0; i < Screen::kLines; i++) {
		const int32_t y = kBodyY + i * kTextSpacingY;
		canvas.drawString(screen_.lines[i], 0, y, kTextSpacingX, kTextSpacingY);
		if (screen_.lineRight[i][0] != '\0') {
			canvas.drawStringAlignRight(screen_.lineRight[i], y, kTextSpacingX, kTextSpacingY);
		}
	}
}
