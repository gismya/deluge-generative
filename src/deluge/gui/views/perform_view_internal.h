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

// Private to PerformView's files (perform_view*.cpp): the view's includes and the constants and helpers more than
// one of its areas uses, in perform_view_shared. A constant one area uses lives in that area's file, and each file
// names the using-declarations it needs.

#pragma once

#include "OSLikeStuff/scheduler_api.h"
#include "generative/core/genre.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/perform_midi.h"
#include "generative/firmware/pump.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "generative/perform/knob_page.h"
#include "generative/perform/queue_text.h"
#include "generative/perform/sidebar.h"
#include "gui/colour/palette.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/colour.h"
#include "gui/menu_item/generative/perform_menu.h"
#include "gui/ui/browser/perform_preset_browser.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
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
#include <cstdio>
#include <cstring>
#include <iterator>

namespace perform_view_shared {

/// F8, the mod button Perform takes from song-level decimation and bitcrush (spec §13.5).
constexpr uint8_t kMacroModButton = 7;

/// A pad's row counted from the top from 0: MUTE/LAUNCH row 1 + p is part p.
constexpr int32_t rowFromY(int32_t y) {
	return kDisplayHeight - 1 - y;
}
/// The pad `y` of a row counted from the top: rowFromY()'s inverse.
constexpr int32_t yFromRow(int32_t row) {
	return kDisplayHeight - 1 - row;
}

/// Queued actions blink with this half-period.
constexpr uint32_t kBlinkSamples = kSampleRate * 2 * kFlashTime / 1000;

/// Each role's colour on the Arc map's part rows (the control-layout prototype's), indexed by Role: Drums, Bass,
/// Chords, Pad, Lead, FX.
constexpr RGB kRoleColours[] = {RGB(255, 40, 40),  RGB(255, 140, 0), RGB(0, 200, 255),
                                RGB(150, 80, 255), RGB(40, 255, 90), RGB(255, 60, 220)};

/// Each Section kind's colour on the AUDITION/SECTION column (the control-layout prototype's), indexed by
/// SectionKind: Intro, Groove, Break, Build, Outro.
constexpr RGB kSectionColours[] = {RGB(140, 140, 140), RGB(0, 220, 70), RGB(40, 90, 255), RGB(255, 150, 0),
                                   RGB(170, 60, 210)};

constexpr RGB sectionColour(deluge::generative::SectionKind kind, bool bright) {
	const RGB colour = kSectionColours[static_cast<int32_t>(kind)];
	return bright ? colour : colour.dim(2);
}

/// A param value as a gold knob's position (kKnobMin..kKnobMax) for the param `stack` addresses.
inline int32_t knobPos(ModelStackWithAutoParam& stack, int32_t value) {
	return stack.paramCollection->paramValueToKnobPos(value, &stack);
}

/// The param `stack` addresses, as a gold knob's position now.
inline int32_t liveKnobPos(ModelStackWithAutoParam& stack) {
	return knobPos(stack, stack.autoParam->getCurrentValue());
}

/// A Kit part's clip edits its whole Kit, not one drum: the gold knobs and the sound editor show its kit-level pages
/// and FX (spec §13.5, §13.7). Leaves a synth part's alone.
inline void affectWholeKit(InstrumentClip& clip) {
	if (clip.output != nullptr && clip.output->type == OutputType::KIT) {
		clip.affectEntire = true;
	}
}

#if ENABLE_SYSEX_TEST
/// Test builds: the Arc map as last reported (gen_arcmap, gen_arcrow), so only changes are. Forgotten on each entry
/// (opened()), so a script's first map reports in full whatever the last script left.
inline char shownSections[deluge::generative::perform::kArcMapTextChars + 1] = "";
inline char shownParts[deluge::generative::kMaxParts][deluge::generative::perform::ArcMap::kColumns + 1] = {};
#endif

} // namespace perform_view_shared
