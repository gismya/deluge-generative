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

// MIDI in Perform (spec §13.7), what the stock MIDI routes call while Perform is the root UI: where MIDI-follow CCs
// go (F8), the macro CCs, the global MIDI commands (F10) and the live-value feedback. External notes are dropped in
// the stock routes themselves (F18).

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/recipe.h"
#include <cstdint>

class Clip;
class ModelStackWithAutoParam;
class ModelStackWithTimelineCounter;

namespace deluge::generative::firmware {

/// F8: the Clip whose params MIDI-follow CCs edit, as the gold knobs do: the focused part's, or nullptr for the
/// song's params with nothing focused.
Clip* followedClip();
/// F8: a song param for MIDI follow with nothing focused, on `stack`'s memory: `globalParamId`'s param on the song,
/// or nullptr when the CC maps to none.
ModelStackWithAutoParam* followedSongParam(ModelStackWithTimelineCounter* stack, int32_t globalParamId);

/// F8: a CC on a MIDI-follow channel. CCs 114–117 steer the macros' centres. @return whether it was one.
bool followCC(int32_t cc, int32_t value);
/// F8: a follow CC reached `clip`'s param (or the song's, with no clip) on `stack`: the value the param took is noted
/// as sent, so feedback doesn't send it back to the controller that set it.
void followCCLanded(ModelStackWithTimelineCounter* stack, Clip* clip, int32_t cc);

/// F10: a global MIDI command (a note-on, or with `on` false its note-off) under Perform (spec §13.7). TRANSPOSE
/// queues a key change; PLAYBACK_RESTART is blocked; LOOP, UNDO, REDO and NEXT_SONG are ignored. @return whether
/// Perform took it: PLAY, RECORD, TAP, FILL and SHIFT go the stock way, where Perform already handles them.
bool globalMidiCommand(GlobalMIDICommand command, bool on, int32_t note);

/// MIDI-follow feedback under Perform (spec §13.7): `stack`'s param set to `knobPos`, as stock sends a player edit,
/// if the param is on what the gold knobs point at;
/// with no stack, a pass over the followed params and the macros, sending each value that moved since it was last
/// sent (`isAutomation`, at stock's automation-feedback rate) or every value (a new context).
void sendFeedback(ModelStackWithAutoParam* stack, int32_t knobPos, bool isAutomation);
/// A macro's centre moved on the panel (a gold knob, a fader tap): its CC's feedback, if it moved.
void sendMacroFeedback(Macro macro);
/// Each frame while playing: keeps stock's automation-feedback timer running, unless its rate is DISABLED, so the
/// live values go out at that rate.
void keepFeedbackRunning();

} // namespace deluge::generative::firmware
