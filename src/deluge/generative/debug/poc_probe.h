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

// Hardware tests of the Perform POC (#45): what the Perform driver did, sent as sysex test port events.
// Compiled in only with ENABLE_SYSEX_TEST. Every hook is a no-op macro otherwise.

#pragma once

#if ENABLE_SYSEX_TEST

#include <cstdint>

namespace deluge::generative::probe {

/// From AudioEngine::routine(): watches for renders that come too late to keep the output buffer full.
void audioRoutineEntered();
/// From AudioEngine::allowedToStartVoice(), when it refuses (the note stays pending).
void voiceStartRefused();
/// From Sound::process_postarp_notes(), when it starts a note that was left pending.
void pendingVoiceStarted();

/// From the Perform driver.
void generatorStarted(int64_t swungPos, int64_t originTick, int64_t barLength);
void generatorStopped();
/// `part` 0 = Drums, 1 = Bass. Call noteOnBefore() just before forwarding a note-on and noteOnAfter() just after.
void noteOnBefore();
void noteOnAfter(int32_t tick, uint8_t part, uint8_t lane, bool openHatRinging);
void noteOff();
/// Every hook call while the Generator runs. `generatorNow` is in Generator ticks (24 per 16th).
void hook(int64_t generatorNow, int32_t currentStep);

/// From the Perform view's graphicsRoutine (UI context): memory now and then.
void uiTick();
/// Perform entry: how long PerformDriver::enter() took.
void entered(double seconds, int32_t error);
/// The player moved a macro.
void recipeSet(int32_t energy, int32_t density);

} // namespace deluge::generative::probe

#define GEN_PROBE(...) __VA_ARGS__

#else

#define GEN_PROBE(...)

#endif
