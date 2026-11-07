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

// Perform (spec §13): the root UI while the Generator plays live. It has PLAY and SHIFT+PLAY, the Recipe-level gold
// knobs (F8 = Energy / Density, F8 again = Complexity / Motion; F1–F7 stay stock song FX), the tempo encoder and TAP
// for the Recipe tempo and swing (F4), the sidebar (part mute and solo, force, hold and End, spec §13.5), the OLED
// status line with a SELECT-turn peek at QUEUE and RECIPE, BACK tap to cancel the newest queued action, and the
// leave menu on BACK hold. The grid pages are later slices (#110).

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include "gui/colour/rgb.h"
#include "gui/ui/root_ui.h"
#include "hid/button.h"
#include <cstdint>

class PerformView final : public RootUI {
public:
	bool opened() override;
	void focusRegained() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	/// BACK held: the leave menu, while playback continues (spec §13.8).
	ActionResult exitUI() override;
	void modButtonAction(uint8_t whichButton, bool on) override;
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	/// F4: the tempo encoder while Perform is the root UI, under any overlay too (spec §13.5, §13.7). Turn = Recipe
	/// tempo, SHIFT = swing, TAP held = swing interval, X held = stock sync nudge; LEARN+turn is swallowed. Under an
	/// external clock a tempo turn shows the tempo marked EXT and changes nothing.
	void tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed);
	/// TAP, from the panel or MIDI: the Recipe tempo, gliding (spec §13.7).
	void setRecipeTempo(int32_t tempo);
	/// SELECT turn: a 4 s peek at QUEUE or RECIPE below the status line (spec §13.5).
	void selectEncoderAction(int8_t offset) override;
	void graphicsRoutine() override;
	void notifyPlaybackBegun() override;
	void playbackEnded() override;
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	UIType getUIType() override { return UIType::GENERATIVE_PERFORM; }

	/// LEAVE with nothing kept: tears Perform down as a runner job, which lands on a new blank song in session view.
	void leave();
	UIModControllableContext getUIModControllableContext() override { return UIModControllableContext::SONG; }
	bool getAffectEntire() override { return true; }

private:
	/// Whether the gold knobs are on F8, Perform's macro page.
	[[nodiscard]] bool onMacroPage() const;
	/// The macro a gold knob turns on F8's current set: the upper knob Energy or Complexity, the lower Density or
	/// Motion (spec §13.5).
	[[nodiscard]] deluge::generative::Macro knobMacro(int32_t whichModEncoder) const;
	void updateKnobIndicators();
	void showTempo();
	void showSwing();
	/// A pad in the sidebar (spec §13.5): MUTE/LAUNCH rows 1–6 mute or (X held) solo the parts, SHIFT at once;
	/// AUDITION/SECTION rows 1–3 force Groove, Break or Build, row 5 holds, row 8 is End.
	void sidebarPadAction(int32_t x, int32_t y, bool on);
	/// The popup for a press the full steering queue refused (spec §2.6).
	static void refused(bool accepted);
	/// A sidebar pad's colour now: stock mute colours, the Section kinds' colours, queued ones blinking.
	[[nodiscard]] RGB sidebarColour(int32_t x, int32_t y) const;
	/// Works out every sidebar colour and asks to redraw only the rows that changed (spec §13.5).
	void refreshSidebar();
	/// Forgets a hold on the End pad without arming or cancelling anything.
	void releaseEndPad();
	/// From graphicsRoutine(), every 15 ms: arms End once its pad has been held 0.6 s. A hold that an overlay
	/// interrupted is forgotten. Not the shared UI_SPECIFIC timer, which goes to whichever UI is current.
	void checkEndHold();

	/// The End pad is held, and whether this hold has armed End yet (spec §13.5).
	bool endHeld_ = false;
	bool endArmedThisHold_ = false;
	uint32_t endPressedAt_ = 0; ///< AudioEngine::audioSampleTimer at the press.

	/// BACK's hold opened the leave menu since it went down, so its release isn't a tap.
	bool backHeld_ = false;

	/// The sidebar as last asked to render, per row and column, so only changed rows redraw.
	RGB sidebar_[kDisplayHeight][kSideBarWidth] = {};
	/// The Section kind last seen playing, and its length in phrases (0 for a Groove), for the status line's count.
	int32_t lengthSection_ = -1;
	uint16_t lengthOccurrence_ = 0;
	int32_t sectionPhrases_ = 0;

	/// F8's two sets: false = Energy / Density, true = Complexity / Motion.
	bool secondMacroSet_ = false;

	/// The SELECT-turn peek below the status line (spec §13.5), and until when (audioSampleTimer).
	enum class Peek : uint8_t { None, Queue, Recipe };
	Peek peek_ = Peek::None;
	uint32_t peekUntil_ = 0;

	/// What the OLED shows, composed as text so graphicsRoutine() redraws only on a change.
	struct Screen {
		char left[24] = {};  ///< Section and count, HELD; or STOPPED
		char right[12] = {}; ///< the next queued action's tag, or bar.beat
		bool tag = false;    ///< `right` is a tag: drawn inverted
		bool ext = false;
		int16_t phraseBar = 0; ///< pixels
		static constexpr int32_t kLines = 3;
		char lines[kLines][24] = {};
		char lineRight[kLines][12] = {};
		bool operator==(const Screen&) const = default;
	};
	void compose(Screen& screen);
	/// Composes the screen and, if anything changed, redraws the OLED.
	void refreshScreen();
	Screen screen_;
};

extern PerformView performView;
