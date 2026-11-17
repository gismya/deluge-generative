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

// PerformView's OLED and tempo (spec §13.4, §13.5): the status line and pages, the peeks, the tempo encoder, TAP
// and swing.

#include "gui/views/perform_view_internal.h"

namespace {

/// The SELECT-turn peek lasts this long (spec §13.5).
constexpr uint32_t kPeekSamples = kSampleRate * 4;

/// A knob position's span on a PART page bar, in pixels.
constexpr int32_t kBarPixels = 38;

/// The MACROS page: a row per macro, in the 7 px font with a pixel less between rows than the other pages, so four
/// fit; each bar's left edge and width in pixels.
constexpr int32_t kMacroRowPixels = 8;

constexpr int32_t kMacroBarX = 24;

constexpr int32_t kMacroBarPixels = OLED_MAIN_WIDTH_PIXELS - kMacroBarX - 2;

constexpr int32_t kStatusY = OLED_MAIN_TOPMOST_PIXEL;

constexpr int32_t kBodyY = OLED_MAIN_TOPMOST_PIXEL + 12;

/// A coarse tempo step: 16 an octave, as stock's metronome values (2^(1/16)).
constexpr float kCoarseTempoStep = 1.0442738f;

} // namespace

void PerformView::tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed) {
	using namespace deluge::hid::button;
	auto& recipe = currentRecipe.recipe;

	if (Buttons::isButtonPressed(TAP_TEMPO)) {
		// The swing interval, as stock: the first turn shows it.
		if (display->hasPopupOfType(PopupType::SWING)) {
			int32_t interval = std::clamp<int32_t>(recipe.swingInterval + (offset > 0 ? 1 : -1), 0,
			                                       deluge::generative::kMaxSwingInterval);
			if (refusedWithPopup(performAdapter.steering().setSwingInterval(interval))) {
				return;
			}
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
			if (refusedWithPopup(performAdapter.steering().setSwing(std::clamp<int32_t>(
			        recipe.swing + offset, -deluge::generative::kMaxSwing, deluge::generative::kMaxSwing)))) {
				return;
			}
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

void PerformView::tapTempo(uint32_t samplesPerQuarter) {
	if (samplesPerQuarter > 0) {
		// Taps are quarter notes: hundredths of a BPM from samples per quarter.
		setRecipeTempo(static_cast<int32_t>(static_cast<uint32_t>(kSampleRate) * 60 * 100 / samplesPerQuarter));
	}
}

void PerformView::setRecipeTempo(int32_t tempo) {
	if (refusedWithPopup(performAdapter.steering().setTempo(
	        std::clamp(tempo, deluge::generative::kMinTempo, deluge::generative::kMaxTempo)))) {
		TEST_EVENT("gen_tempo refused");
		return;
	}
	showTempo();
}

void PerformView::showTempo() {
	// The stock tempo popup, with the Recipe tempo the song glides to; under an external clock the tracked tempo,
	// marked EXT (spec §13.7).
	DEF_STACK_STRING_BUF(text, 30);
	text.append(deluge::l10n::get(deluge::l10n::String::STRING_FOR_TEMPO_POPUP));
	if (playbackHandler.isExternalClockActive()) {
		playbackHandler.getTempoStringForOLED(playbackHandler.calculateBPMForDisplay(), text);
		text.append(" ");
		text.append(deluge::l10n::get(deluge::l10n::String::STRING_FOR_EXTERNAL_CLOCK_TAG));
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
			static char shown[deluge::generative::perform::kArcPageTextChars + 1] = "";
			char text[sizeof(shown)];
			deluge::generative::perform::describeArcPage(screen.arcPage, text);
			if (strcmp(text, shown) != 0) {
				strcpy(shown, text);
				TEST_EVENT("gen_arcpage %s", text);
			}
		}
		if (screen.macros
		    && !(screen_.macros
		         && std::equal(std::begin(screen.macroCentre), std::end(screen.macroCentre),
		                       std::begin(screen_.macroCentre))
		         && std::equal(std::begin(screen.macroArc), std::end(screen.macroArc), std::begin(screen_.macroArc)))) {
			char text[deluge::generative::perform::kMacrosPageTextChars + 1];
			deluge::generative::perform::describeMacrosPage(screen, text);
			TEST_EVENT("gen_macropage %s", text);
		}
		if (screen.part && (strcmp(screen.sound, screen_.sound) != 0 || strcmp(screen.take, screen_.take) != 0)) {
			TEST_EVENT("gen_partpage %s|%s", screen.sound, screen.take);
		}
#endif
		screen_ = screen;
		// Only while Perform's own screen shows: under an overlay that covers it (the preset browser, the sound
		// editor, a menu) a redraw every beat restarted its side scroller, so a long name never scrolled to its end.
		// focusRegained() redraws when the overlay closes. Around one that shows it (LEAVE PERFORM?) it redraws.
		if (screenShows()) {
			renderUIsForOled();
		}
	}
}

bool PerformView::screenShows() {
	for (int32_t up = 0;; up++) {
		UI* ui = getUIUpOneLevel(up);
		if (ui == nullptr || ui == &performView) {
			return ui == &performView;
		}
		if (!ui->oledShowsUIUnderneath) {
			return false;
		}
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
	static_assert(perform::kOledWidthPixels == OLED_MAIN_WIDTH_PIXELS);
	static_assert(perform::kQueueLineChars == OLED_MAIN_WIDTH_PIXELS / kTextSpacingX);
	perform::OledInput in;
	const Generator* piece = performAdapter.playingGenerator();
	const auto& output = performAdapter.output();
	in.piece = piece;
	in.recipe = &currentRecipe.recipe;
	in.heard = {
	    .known = output.sectionKnown,
	    .kind = output.section,
	    .occurrence = output.occurrence,
	    .tick = output.sectionTick,
	};
	in.clock = performAdapter.clock();
	in.phraseSteps = performAdapter.steering().phraseSteps();
	in.ext = playbackHandler.isExternalClockActive();
	in.peek = peek_;
	in.page = page_;
	if (page_ == Page::Clip) {
		// PART reads the CLIP page's part, and its controls as refreshFaders() read them.
		const int32_t part = shownPart(); // the Recipe has sound picks for the roster's parts only
		in.title.part = lastFocusedPart_;
		in.title.parts = performAdapter.steering().rosterParts();
		if (part >= 0) {
			in.title.genre = performAdapter.steering().rosterGenre();
			in.title.preset = currentRecipe.sounds.part[part].c_str();
			in.title.takes = performAdapter.steering().takeRow(part);
			in.title.frozen = partFrozen(part);
		}
		std::copy_n(faderLevels_, kNumSoundControls, in.levels);
	}
	in.history = &performAdapter.history();
	in.arcState = &arc_;
	perform::composeOled(in, sectionCount_, screen);
	if (screen.arc) {
		// Queued boxes blink: the screen changes with the phase, so it redraws.
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		for (perform::ArcBox& box : screen.arcPage.box) {
			box.blink = box.blink && !blinkOff;
		}
	}
}

void PerformView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	// The status line (spec §13.5): left the Section, right a tag or bar.beat, then EXT; the phrase bar under it.
	canvas.drawString(screen_.left, 0, kStatusY, kTextSpacingX, kTextSpacingY);
	int32_t right = OLED_MAIN_WIDTH_PIXELS;
	if (screen_.ext) {
		const char* tag = deluge::l10n::get(deluge::l10n::String::STRING_FOR_EXTERNAL_CLOCK_TAG);
		canvas.drawStringAlignRight(tag, kStatusY, kTextSpacingX, kTextSpacingY, right);
		right -= static_cast<int32_t>(strlen(tag) + 1) * kTextSpacingX; // the tag and a space
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
			char label[deluge::generative::perform::kMacroLabelChars + 1];
			deluge::generative::perform::macroLabel(m, screen_.macroCentre[m], label);
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
