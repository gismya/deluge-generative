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
#include "model/mod_controllable/mod_controllable.h"
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
using deluge::generative::kStepsPerBeat;
using deluge::generative::kTicksPerStep;
using deluge::generative::Macro;
using deluge::generative::macroToPoints;
using deluge::generative::SectionKind;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using deluge::gui::context_menu::leavePerform;

PLACE_SDRAM_BSS PerformView performView{};

namespace {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;
// The sidebar (spec §13.5). The spec counts rows from the top; firmware pad y 0 is the bottom row.
constexpr int32_t kMuteColumn = kDisplayWidth;
constexpr int32_t kSectionColumn = kDisplayWidth + 1;
/// MUTE/LAUNCH row 1 + p is part p.
constexpr int32_t muteRowY(int32_t part) {
	return kDisplayHeight - 1 - part;
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

/// Each Section kind's colour on the AUDITION/SECTION column (the control-layout prototype's), indexed by
/// SectionKind: Intro, Groove, Break, Build, Outro.
constexpr RGB kSectionColours[] = {RGB(140, 140, 140), RGB(0, 220, 70), RGB(40, 90, 255), RGB(255, 150, 0),
                                   RGB(170, 60, 210)};
constexpr RGB sectionColour(SectionKind kind, bool bright) {
	const RGB colour = kSectionColours[static_cast<int32_t>(kind)];
	return bright ? colour : colour.dim(2);
}

constexpr int32_t kTicksPerBar = kStepsPerBar * kTicksPerStep;
constexpr int32_t kStatusY = OLED_MAIN_TOPMOST_PIXEL;
constexpr int32_t kBodyY = OLED_MAIN_TOPMOST_PIXEL + 12;
constexpr int32_t kMaxChars = OLED_MAIN_WIDTH_PIXELS / kTextSpacingX;
/// A coarse tempo step: 16 an octave, as stock's metronome values (2^(1/16)).
constexpr float kCoarseTempoStep = 1.0442738f;
/// The upper gold knob is encoder 1, the lower is 0.
constexpr int32_t kUpperKnob = 1;
constexpr int32_t kTicksPerBeat = kStepsPerBeat * kTicksPerStep;

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
	releaseEndPad(); // a hold cut short by a menu or overlay never arms End

	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kSideBarWidth; x++) {
			sidebar_[y][x] = sidebarColour(kDisplayWidth + x, y);
		}
	}
	compose(screen_);
	uiNeedsRendering(this);
	renderUIsForOled();
}

ActionResult PerformView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;

	if (b == BACK) {
		// A tap cancels the newest queued action (spec §13.5). A hold opens the leave menu instead (exitUI()).
		if (on) {
			backHeld_ = false;
		}
		else if (!backHeld_) {
			refused(performAdapter.cancelNewest());
			TEST_EVENT("gen_back_tap");
		}
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
	// The grid pages are a later slice (#110).
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
		const auto* genre = currentRecipe.recipe.genre;
		const int32_t part = kDisplayHeight - 1 - y;
		if (genre == nullptr || part >= deluge::generative::partCount(*genre)) {
			return;
		}
		// X held + MUTE = solo, as stock (spec §13.5).
		const bool solo = Buttons::isButtonPressed(X_ENC);
		refused(solo ? performAdapter.pressSolo(part, now) : performAdapter.pressMute(part, now));
		TEST_EVENT("gen_press %s part=%ld now=%d", solo ? "solo" : "mute", static_cast<long>(part), now ? 1 : 0);
		return;
	}
	if (y == kHoldPadY) {
		refused(performAdapter.pressHold());
		TEST_EVENT("gen_press hold");
		return;
	}
	const int32_t row = kDisplayHeight - 1 - y;
	if (row < static_cast<int32_t>(std::size(kForceRows))) {
		refused(performAdapter.forceSection(kForceRows[row]));
		TEST_EVENT("gen_press force %s", deluge::generative::sectionName(kForceRows[row]));
	}
}

void PerformView::refused(bool accepted) {
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
	refreshSidebar();
}

RGB PerformView::sidebarColour(int32_t x, int32_t y) const {
	using namespace deluge::gui;
	const Generator* piece = performAdapter.piece();
	const auto& output = performAdapter.output();
	const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
	const auto* genre = currentRecipe.recipe.genre;

	if (x == kMuteColumn) {
		const int32_t part = kDisplayHeight - 1 - y;
		if (genre == nullptr || part >= deluge::generative::partCount(*genre)) {
			return colours::black;
		}
		if (piece == nullptr) {
			return menu_item::activeColourMenu.getRGB(); // every start is fresh: nothing muted
		}
		// Stock colours: green active, red muted, blue solo, dim = silenced by the Arc or soloed out (spec §13.5).
		RGB colour = menu_item::activeColourMenu.getRGB();
		if (piece->partSoloed(part)) {
			colour = menu_item::soloColourMenu.getRGB();
		}
		else if (piece->partMuted(part)) {
			colour = menu_item::stoppedColourMenu.getRGB();
		}
		else if (piece->anySoloed() || piece->partSilenced(part)) {
			colour = colour.dull();
		}
		const bool queued = piece->mutePending(part) || piece->soloPending(part);
		return queued && blinkOff ? colours::black : colour;
	}

	const SectionKind playing = output.sectionKnown ? output.section : SectionKind::Intro;
	const bool sounding = piece != nullptr && output.sectionKnown;
	if (y == kEndPadY) {
		const bool bright = endHeld_ || (sounding && playing == SectionKind::Outro);
		const bool queued = piece != nullptr && piece->endQueued();
		return queued && blinkOff ? colours::black : sectionColour(SectionKind::Outro, bright || queued);
	}
	if (y == kHoldPadY) {
		return piece != nullptr && piece->arc().held() ? colours::white : RGB::monochrome(40);
	}
	const int32_t row = kDisplayHeight - 1 - y;
	if (row < static_cast<int32_t>(std::size(kForceRows))) {
		const SectionKind kind = kForceRows[row];
		const bool queued = piece != nullptr && piece->sectionForced(kind);
		return queued && blinkOff ? colours::black : sectionColour(kind, sounding && playing == kind);
	}
	return colours::black;
}

#if ENABLE_SYSEX_TEST
namespace {
/// The sidebar's state as text, for the test port: a letter per part (a active, m muted, s solo, d dim), followed by *
/// while a press waits; the forced Section (G, B, U or -); the hold; End (q while queued).
void sidebarState(const Generator* piece, int32_t parts, char* out, size_t size) {
	char mute[2 * deluge::generative::kMaxParts + 1] = {};
	int32_t n = 0;
	for (int32_t p = 0; p < parts; p++) {
		char state = 'a';
		if (piece != nullptr) {
			state = piece->partSoloed(p)                           ? 's'
			        : piece->partMuted(p)                          ? 'm'
			        : piece->anySoloed() || piece->partSilenced(p) ? 'd'
			                                                       : 'a';
		}
		mute[n++] = state;
		if (piece != nullptr && (piece->mutePending(p) || piece->soloPending(p))) {
			mute[n++] = '*';
		}
	}
	char force = '-';
	if (piece != nullptr) {
		force = piece->sectionForced(SectionKind::Groove)  ? 'G'
		        : piece->sectionForced(SectionKind::Break) ? 'B'
		        : piece->sectionForced(SectionKind::Build) ? 'U'
		                                                   : '-';
	}
	snprintf(out, size, "mute=%s force=%c hold=%d end=%c", mute, force, piece != nullptr && piece->arc().held() ? 1 : 0,
	         piece != nullptr && piece->endQueued() ? 'q' : '-');
}
char shownSidebarState[48] = "";
} // namespace
#endif

void PerformView::refreshSidebar() {
#if ENABLE_SYSEX_TEST
	{
		const auto* genre = currentRecipe.recipe.genre;
		char state[sizeof(shownSidebarState)];
		sidebarState(performAdapter.piece(), genre != nullptr ? deluge::generative::partCount(*genre) : 0, state,
		             sizeof(state));
		if (strcmp(state, shownSidebarState) != 0) {
			strcpy(shownSidebarState, state);
			TEST_EVENT("gen_sidebar %s", state);
		}
	}
#endif
	uint32_t changed = 0;
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kSideBarWidth; x++) {
			const RGB colour = sidebarColour(kDisplayWidth + x, y);
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
	refreshSidebar();
	refreshScreen();
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
	const Generator* piece = performAdapter.piece();
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
	if (piece != nullptr) {
		perform::statusTag(*piece, performAdapter.clock(), screen.right);
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
	// HELD in full if it fits beside the right side, or else H.
	const int32_t rightChars = static_cast<int32_t>(strlen(screen.right)) + (screen.ext ? 4 : 0) + 1;
	if (held[0] != '\0') {
		const int32_t room = kMaxChars - rightChars - static_cast<int32_t>(strlen(screen.left));
		strncat(screen.left, static_cast<int32_t>(strlen(held)) <= room ? held : " H",
		        sizeof(screen.left) - strlen(screen.left) - 1);
	}

	// Below it: a peek, or the macros (the grid-page bodies come with #110).
	const auto& recipe = currentRecipe.recipe;
	if (peek_ == Peek::Queue) {
		perform::QueuedAction actions[PendingList::kCapacity];
		const int32_t count = piece != nullptr ? perform::queuedByTime(*piece, actions) : 0;
		if (count == 0) {
			snprintf(screen.lines[0], sizeof(screen.lines[0]), "%s",
			         l10n::get(l10n::String::STRING_FOR_NOTHING_QUEUED));
		}
		for (int32_t i = 0; i < std::min<int32_t>(count, Screen::kLines); i++) {
			perform::queueLabel(actions[i].command, *genre, screen.lines[i]);
			perform::countdown(stepTick(actions[i].landsAt) - performAdapter.clock(), true, screen.lineRight[i]);
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
	refreshScreen();
}

void PerformView::playbackEnded() {
	performAdapter.playbackEnded();
	refreshSidebar();
	refreshScreen();
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

	for (int32_t i = 0; i < Screen::kLines; i++) {
		const int32_t y = kBodyY + i * kTextSpacingY;
		canvas.drawString(screen_.lines[i], 0, y, kTextSpacingX, kTextSpacingY);
		if (screen_.lineRight[i][0] != '\0') {
			canvas.drawStringAlignRight(screen_.lineRight[i], y, kTextSpacingX, kTextSpacingY);
		}
	}
}
