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

// Stand-in probes for the spec review's hardware tests (H1-H14 in docs/research/spec-review-technical.md on
// research/spec-review). Driven by "probe <name> [a] [b] [c]" lines on the sysex test port, reported as test events.
// Compiled in only with ENABLE_SYSEX_TEST; every hook is a no-op otherwise.

#pragma once

#include <stdint.h>

#if ENABLE_SYSEX_TEST

#ifdef __cplusplus
extern "C" {
#endif

// Hooks from C drivers. All may run in an ISR, so they only add to counters.
void hwProbePicTxStart(void);         ///< The PIC UART TX DMA started (uartFlushIfNotSending).
void hwProbePicTxIdle(void);          ///< The PIC UART TX DMA ran out of bytes (tx_interrupt).
void hwProbePicBytes(int32_t number); ///< Bytes handed to the PIC UART TX DMA.
void hwProbeOledFrameDone(void);      ///< An OLED frame's SPI DMA completed (oledTransferComplete).

#ifdef __cplusplus
}

namespace deluge::generative::hwprobe {

/// Parses a probe name for the test port. Returns -1 for an unknown name.
int32_t commandId(const char* name);
/// Runs a probe command from the buttons-and-pads task. Returns false if it must wait for the card routine to end.
bool command(int32_t id, int32_t a, int32_t b, int32_t c, bool inCardRoutine);

// ---- H7: pad rows and OLED each 16th ----

/// From the POC probe: the Generator started with step 0 at `originTick` (swung firmware ticks), and stopped.
void generatorStarted(int64_t originTick);
void generatorStopped();
/// From the POC probe at the top of every audio render: finds 16th grid lines from the playback position. The
/// Generator's hook can't, since it only runs on the ticks the Generator asks for.
void audioRoutineEntered();
/// Whether the H7 redraw test runs, and the last 16th grid line (Generator steps, -1 before the first).
bool redrawTestOn();
int32_t gridStep();
/// From the Perform view when it shows a new step. Returns the main rows to redraw for the test (0 = none).
uint32_t stepShown(int32_t step, bool* oledThisStep);
/// Rows the redraw test changes, and the step they show, for the Perform view's renderMainPads.
uint32_t redrawRows();
int32_t redrawStep();
/// Sidebar rows the redraw test changes along with the main rows.
uint32_t redrawSidebarRows();
/// Whether the OLED shows the step, so its frame changes each 16th.
bool redrawOledEveryStep();
/// From PadLEDs::sendOutMainPadColours() once the main grid is queued, and when it had to defer.
void padsQueued();
void padsDeferred();
// ---- H14: the driver's clock under an external clock ----

/// From PerformDriver::doTickForward() on every hook call, before it does anything else.
void driverHook(int64_t swungPos);
/// From PlaybackHandler when a MIDI Start (1), Continue (2), Song Position Pointer (3, `value` = 16ths) or Stop (4)
/// arrives and is acted on.
void clockEvent(int32_t kind, int32_t value);

/// From readButtonsAndPads() once an injected edge's handler returned. `receivedTicks` is when the MIDI routine
/// queued it.
void injectedHandled(uint32_t receivedTicks, uint8_t value, int32_t velocity);

} // namespace deluge::generative::hwprobe

#define HW_PROBE(...) __VA_ARGS__
#endif // __cplusplus

#else

#define HW_PROBE(...)

#endif // ENABLE_SYSEX_TEST
