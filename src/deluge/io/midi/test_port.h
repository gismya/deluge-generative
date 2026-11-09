/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#pragma once

/// Sysex test port: lets a host press buttons and pads, turn encoders, read UI state and receive test events over
/// MIDI sysex. Compiled in only with the ENABLE_SYSEX_TEST CMake option, which is off by default and must stay off in
/// shipped builds: anything on the MIDI port can drive the device.
///
/// Requests (HID namespace, F0 00 21 7B 01 02 ...):
///   10 <seq> [00 <key:5>] <ascii line> F7   queue one input event, see parseLine() in test_port.cpp
///   11 <seq> [00 <key:5>] F7                state query
/// Replies:
///   02 50 <seq> <status> F7                 status, see TestPort::Status
///   02 51 <seq> {json} F7                   state
/// Events go out in the debug log frame, F0 00 21 7B 01 03 40 01 <ascii> F7, to the cable that subscribed to the
/// debug log (03 00 01). The optional key is the "Allow insecure develop sysex messages" key, packed as for loadfw.
/// A wrong key gets the BadKey status.

#if ENABLE_SYSEX_TEST

#include <cstdint>
#include <optional>

class MIDICable;

namespace TestPort {

enum class Status : uint8_t { Queued, QueueFull, ParseError, BadKey };

constexpr uint8_t kInjectCommand = 0x10;
constexpr uint8_t kStateCommand = 0x11;
constexpr uint8_t kStatusReply = 0x50;
constexpr uint8_t kStateReply = 0x51;
constexpr uint8_t kEventCategory = 0x01;

/// Called from the MIDI routine. Only parses and queues, never acts on the UI.
void injectReceived(MIDICable& cable, uint8_t* data, int32_t len);
void stateReceived(MIDICable& cable, uint8_t* data, int32_t len);

/// An injected pad or button edge, as the PIC would send it.
struct HidEvent {
	uint8_t value;    ///< PIC byte: Pad::toChar() or a button::fromXY() value
	int32_t velocity; ///< 0 for a release, else the press velocity (USE_DEFAULT_VELOCITY for none)
};

/// Takes the next injected pad or button edge once it is due. Runs encoder and wait entries itself, and holds back
/// everything after an encoder turn until the encoder task has taken it, so events keep their order.
std::optional<HidEvent> takeHidEvent();
/// Puts back the event takeHidEvent() just returned, for the card-routine retry.
void putBackHidEvent();

/// True while an injected pad or button is down. The PIC's "no presses happening" safety net would release it, so
/// readButtonsAndPads() skips that message meanwhile.
bool holdingInjectedPresses();

/// Called from the encoder task before it reads the encoders: feeds the next gold-knob edge when it is due.
void feedEncoderEdges();

/// Sends at most one queued test event, once the MIDI output has drained.
void sendPendingEvent();

void event(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// Test hook for Perform entry: true from a "hold entry" line until a "release entry" line or releaseEntry(). The entry
/// job waits at its first boundary while it's set, so a script can cancel it at a known point.
bool entryHeld();
void releaseEntry();
/// Test hook for H2 (spec §20): true from a "hold ramps" line until "release ramps". Perform then drops its Control
/// events, so the same piece can be timed with and without live ramps.
bool rampsHeld();
/// Test hook for H2's worst case: true from a "hold stress" line until "release stress". Perform then drops its Control
/// events and instead ramps every sound-control param it bound each step, so every target is rewritten each step.
bool rampsStressed();
/// Test hook for card errors (spec §10.7): true from a "hold card_error" line until "release card_error". Perform's
/// sound loads (CardFolders and its loaders) then fail with Error::SD_CARD, as with a card pulled mid-load. delugemu
/// can't fault its card, and on the Deluge pulling it would end the session.
bool cardErrorHeld();
/// Test hook for sound re-rolls and transitions: true from a "hold preload" line until "release preload" or
/// releasePreload(). The preload's job waits at its start, yielding, so a script can press BACK while it runs instead
/// of racing it.
bool preloadHeld();
void releasePreload();

} // namespace TestPort

#define TEST_EVENT(...) TestPort::event(__VA_ARGS__)

#else

#define TEST_EVENT(...)

#endif
