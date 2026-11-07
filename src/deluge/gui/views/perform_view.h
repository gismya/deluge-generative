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
// for the Recipe tempo and swing (F4), the End pad, an OLED page with the Section and the macros, and the leave menu
// on BACK hold. The grid pages, the rest of the sidebar and the status line are later slices.

#pragma once

#include "generative/core/recipe.h"
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
	ActionResult timerCallback() override;
	/// F4: the tempo encoder while Perform is the root UI, under any overlay too (spec §13.5, §13.7). Turn = Recipe
	/// tempo, SHIFT = swing, TAP held = swing interval, X held = stock sync nudge; LEARN+turn is swallowed. Under an
	/// external clock a tempo turn shows the tempo marked EXT and changes nothing.
	void tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed);
	/// TAP, from the panel or MIDI: the Recipe tempo, gliding (spec §13.7).
	void setRecipeTempo(int32_t tempo);
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
	void redrawEndPad();

	/// The End pad is held, and whether this hold has armed End yet (spec §13.5).
	bool endHeld_ = false;
	bool endArmedThisHold_ = false;
	bool shownEndQueued_ = false;

	/// F8's two sets: false = Energy / Density, true = Complexity / Motion.
	bool secondMacroSet_ = false;
	/// What the OLED last showed, so graphicsRoutine() redraws only on a change.
	int32_t shownBeat_ = -1;
	int32_t shownSection_ = -1;
	uint16_t shownOccurrence_ = 0;
};

extern PerformView performView;
