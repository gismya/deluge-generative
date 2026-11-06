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

#include "dsp/stereo_sample.h"
#include <cstddef>
#include <span>

class Sound;
class Output;
namespace deluge::generative::perform {
class PerformDriver;
}

namespace deluge::generative::hwprobe {

// ---- H1, H3, H4: the Perform hook ----

/// From PerformDriver::doTickForward() after Generator::tick(), with how many steps that call decided. Runs the H1
/// busy loop on decision ticks, fires H3 bursts and writes H4 glide tempo. Returns the Generator tick the hook should
/// next run at (0 if it has no wish).
int64_t afterTick(int64_t generatorNow, int32_t stepsDecided, perform::PerformDriver& driver);
/// From Sound::noteOnPostArpeggiator(): a voice is starting.
void voiceStarted(const Sound* sound);
/// From AudioEngine's voice culling: `count` voices were culled.
void voicesCulled(int32_t count);

/// Parses a probe name for the test port. Returns -1 for an unknown name.
int32_t commandId(const char* name);
/// Commands the test port runs as soon as they arrive (from the MIDI routine), not from the input queue: a cancel
/// must reach a build that holds the buttons task.
bool runsOnArrival(int32_t id);
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

// ---- H8: input during card loads ----

/// Whether the H8 input test runs. The Perform view then treats pad (15, 7) as a "Wait"-class action (it returns
/// REMIND_ME_OUTSIDE_CARD_ROUTINE during a card routine), other pads as "act at once", and BACK as "act at once"
/// instead of leaving Perform.
bool h8On();
/// From the Perform view's handlers in H8 mode: what ran, and whether it was the Wait-class pad.
void h8Handled(int32_t kind, int32_t value);
/// From readButtonsAndPads() when an injected edge's handler asked to be called again outside the card routine.
void injectedDeferred(uint32_t receivedTicks, uint8_t value, int32_t velocity);
/// From the test port when it takes an encoder turn off the queue, and from the Perform view's modEncoderAction().
void encoderEntryTaken(uint32_t receivedTicks, int32_t encoder);
void goldKnobActioned(int32_t whichModEncoder);

/// From readButtonsAndPads() once an injected edge's handler returned. `receivedTicks` is when the MIDI routine
/// queued it.
void injectedHandled(uint32_t receivedTicks, uint8_t value, int32_t velocity);

// ---- #83: silent incoming Kits, retiring Outputs, tail checks ----

/// From AudioEngine::routine_() around renderAudio(): times every render.
uint32_t renderBegin();
void renderEnd(uint32_t startTicks, size_t numSamples);
/// From Song::renderAudio() around each Output's renderOutput(). Returns the probe's slot for a tracked Output (-1 for
/// any other), which renderEnd() takes back: it times the Output and, for the tail check, measures what it added.
int32_t outputRenderBegin(const Output* output, std::span<StereoSample> buffer);
void outputRenderEnd(int32_t slot, std::span<StereoSample> buffer);

} // namespace deluge::generative::hwprobe

#define HW_PROBE(...) __VA_ARGS__
#endif // __cplusplus

#else

#define HW_PROBE(...)

#endif // ENABLE_SYSEX_TEST
