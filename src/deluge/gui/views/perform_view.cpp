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
#include "generative/perform/queue_text.h"
#include "gui/colour/palette.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/colour.h"
#include "gui/ui/generative_modal.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/mod_controllable/mod_controllable.h"
#include "model/output.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "util/d_stringbuf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

using deluge::generative::Generator;
using deluge::generative::kMacroUnitsPerKnobStep;
using deluge::generative::kStepsPerBar;
using deluge::generative::kTicksPerBar;
using deluge::generative::kTicksPerBeat;
using deluge::generative::kTicksPerStep;
using deluge::generative::Macro;
using deluge::generative::macroToPoints;
using deluge::generative::SectionKind;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using deluge::generative::perform::ArcMap;
using deluge::generative::perform::ArcMapCell;
using deluge::gui::context_menu::leavePerform;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;
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
/// The upper gold knob is encoder 1, the lower is 0.
constexpr int32_t kUpperKnob = 1;

/// Indexed by Macro.
constexpr deluge::l10n::String kMacroNames[] = {
    deluge::l10n::String::STRING_FOR_ENERGY,
    deluge::l10n::String::STRING_FOR_DENSITY,
    deluge::l10n::String::STRING_FOR_COMPLEXITY,
    deluge::l10n::String::STRING_FOR_MOTION,
};

uint8_t* songModKnobMode() {
	ModControllable* modControllable = view.activeModControllableModelStack.modControllable;
	return modControllable ? modControllable->getModKnobMode() : nullptr;
}

} // namespace

bool PerformView::opened() {
	// A new entry starts with nothing focused (spec §13.1), whatever the view held when Perform last closed.
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
	// pad never arms, and BACK's release isn't a tap.
	heldPart_ = -1;
	releaseEndPad();
	backDown_ = false;
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
				popupIfRefused(performAdapter.cancelNewest());
				TEST_EVENT("gen_back_tap");
			}
			backDown_ = false;
		}
		return ActionResult::DEALT_WITH;
	}
	if (b == X_ENC) {
		// Tapping X returns the Arc map to now. A turn or a solo (X held + MUTE) while it's down isn't a tap.
		if (on) {
			xUsed_ = false;
		}
		else if (!xUsed_ && scroll_ != 0) {
			scroll_ = 0;
			TEST_EVENT("gen_arc_scroll 0");
			refreshArcMap();
			refreshScreen();
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
	if (b == SESSION_VIEW) {
		return ActionResult::DEALT_WITH; // SONG: the page that's on (#111 and #113 add CLIP and KEYBOARD)
	}
	if (b == TAP_TEMPO) {
		// Stock's TAP handling (SHIFT+TAP = metronome, MIDI learn); the tap itself sets the Recipe tempo (F4).
		return view.buttonAction(b, on, inCardRoutine);
	}
	// PLAY, RECORD, SHIFT and the encoder pushes go to the global handling, which guards RECORD and X+PLAY under
	// Perform (F9). Nothing else reaches stock views from here.
	return ActionResult::NOT_DEALT_WITH;
}

ActionResult PerformView::exitUI() {
	// Without Capture there's only LEAVE PERFORM? (spec §13.8). Playback goes on under it.
	backHeld_ = true;
	if (currentUIMode == UI_MODE_NONE && leavePerform.setupAndCheckAvailability()) {
		releaseEndPad();
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
	uint8_t* mode = songModKnobMode();
	return focusedPart() < 0 && mode != nullptr && *mode == kMacroModButton;
}

void PerformView::modButtonAction(uint8_t whichButton, bool on) {
	if (whichButton == kMacroModButton && focusedPart() < 0) {
		if (on) {
			uint8_t* mode = songModKnobMode();
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
	if (on && whichButton == 0 && songModKnobMode() != nullptr && *songModKnobMode() == 0) {
		return; // Perform owns the mod buttons: F1 again never turns the VU meter on (spec §13.5)
	}
	view.modButtonAction(whichButton, on);
	assertPageLeds();
	updateKnobIndicators();
}

int32_t PerformView::rosterParts() {
	const Generator* piece = performAdapter.playingGenerator();
	const auto* genre = piece != nullptr ? piece->recipe().genre : currentRecipe.recipe.genre;
	return genre != nullptr ? deluge::generative::partCount(*genre) : 0;
}

void PerformView::songPadAction(int32_t y, bool on) {
	const int32_t part = rowFromY(y);
	if (part >= rosterParts()) {
		return;
	}
	if (!on) {
		if (heldPart_ == part) {
			heldPart_ = -1;
			bindKnobs();
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

void PerformView::dropFocus() {
	heldPart_ = -1;
	lockedPart_ = -1;
	bindKnobs();
}

void PerformView::assertPageLeds() {
	indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, true);
	indicator_leds::setLedState(IndicatorLED::CLIP_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::KEYBOARD, false);
	indicator_leds::setLedState(IndicatorLED::AFFECT_ENTIRE, focusedPart() < 0);
}

Macro PerformView::knobMacro(int32_t whichModEncoder) const {
	if (whichModEncoder == kUpperKnob) {
		return secondMacroSet_ ? Macro::Complexity : Macro::Energy;
	}
	return secondMacroSet_ ? Macro::Motion : Macro::Density;
}

void PerformView::updateKnobIndicators() {
	if (!onMacroPage()) {
		indicator_leds::stopLedBlinking(indicator_leds::modLed[kMacroModButton]);
		view.setKnobIndicatorLevels();
		return;
	}
	// The knob rings show the centre (spec §13.5). The F8 LED blinks while Complexity / Motion is on.
	const auto& macros = currentRecipe.recipe.macros;
	for (int32_t knob = 0; knob < 2; knob++) {
		indicator_leds::setKnobIndicatorLevel(knob, macros[knobMacro(knob)] / kMacroUnitsPerKnobStep);
	}
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
		// A focused part's own pages, or the song FX: stock (spec §13.5).
		view.modEncoderAction(whichModEncoder, offset);
		return;
	}
	Macro macro = knobMacro(whichModEncoder);
	int32_t value = currentRecipe.recipe.macros[macro] + offset * kMacroUnitsPerKnobStep;
	performAdapter.setMacro(macro, value);
	updateKnobIndicators();

	char popup[24];
	snprintf(popup, sizeof(popup), "%s %d", deluge::l10n::get(kMacroNames[static_cast<int32_t>(macro)]),
	         static_cast<int>(macroToPoints(currentRecipe.recipe.macros[macro])));
	display->displayPopup(popup);
	renderUIsForOled();
}

ActionResult PerformView::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth) {
		sidebarPadAction(x, y, velocity != 0);
		refreshSidebar();
	}
	else {
		songPadAction(y, velocity != 0);
		refreshArcMap();
	}
	return ActionResult::DEALT_WITH;
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
		if (part >= rosterParts()) {
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

void PerformView::popupIfRefused(bool accepted) {
	if (!accepted) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_QUEUE_FULL));
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
	state.parts = rosterParts();
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
			int32_t interval = std::clamp<int32_t>(recipe.swingInterval + (offset > 0 ? 1 : -1), 0,
			                                       deluge::generative::kMaxSwingInterval);
			performAdapter.setSwingInterval(interval);
		}
		showSwing();
		return;
	}
	offset = std::clamp<int8_t>(offset, -1, 1);
	if (Buttons::isButtonPressed(X_ENC)) {
		playbackHandler.commandNudgeClock(offset); // stock (spec §13.7)
		return;
	}
	if (shiftButtonPressed) {
		if (display->hasPopupOfType(PopupType::SWING)) {
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
	refreshPlan();
	refreshSidebar();
	refreshArcMap();
	refreshScreen();
}

void PerformView::refreshPlan() {
	using namespace deluge::generative;
	const Generator* piece = performAdapter.playingGenerator();
	PlanKey key;
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
	// The plan can get shorter (End queued) or the history move on: keep the scroll inside what's known.
	const auto range = perform::arcMapScrollRange(arcMapInput());
	const int32_t scroll = std::clamp(scroll_, range.back, range.ahead);
	if (scroll != scroll_) {
		scroll_ = scroll;
		TEST_EVENT("gen_arc_scroll %ld", static_cast<long>(scroll_));
	}
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
char shownSections[2 * ArcMap::kColumns + 8] = "";
char shownParts[deluge::generative::kMaxParts][ArcMap::kColumns + 1] = {};
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
	ArcMap map;
	const auto in = arcMapInput();
	deluge::generative::perform::composeArcMap(in, map);
	const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
	uint32_t changed = 0;
	for (int32_t row = 0; row < ArcMap::kRows; row++) {
		const int32_t y = kDisplayHeight - 1 - row; // firmware y 0 is the bottom row
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			const RGB colour = arcMapColour(map.cell[row][x], blinkOff);
			if (colour != mainPads_[y][x]) {
				mainPads_[y][x] = colour;
				changed |= 1U << y;
			}
		}
	}
	// Any main-pad change sends the whole grid (21–22 ms of UART, spec §13.5), so redraw only on a change: at most
	// each step (the now column's flash), each blink and each phrase line.
	if (changed != 0) {
		uiNeedsRendering(this, changed, 0);
		mainRedraws_++;
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
	const int32_t phraseTicks = (genre != nullptr ? genre->phraseSteps() : 8 * kStepsPerBar) * kTicksPerStep;
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
	for (int32_t i = 0; i < Screen::kLines; i++) {
		const int32_t y = kBodyY + i * kTextSpacingY;
		canvas.drawString(screen_.lines[i], 0, y, kTextSpacingX, kTextSpacingY);
		if (screen_.lineRight[i][0] != '\0') {
			canvas.drawStringAlignRight(screen_.lineRight[i], y, kTextSpacingX, kTextSpacingY);
		}
	}
}
