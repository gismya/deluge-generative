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
/// Test hooks for the Compose build (spec §14.5, §15.9), each from a "hold <name>" line until "release <name>":
/// "compose" makes the build (a Compose build, or Capture's on leave) wait after its first bar, so a script can press
/// BACK on a private song with notes in it;
/// "compose_alone" makes the size estimate fit only alone, so the build frees the player's song first;
/// "compose_reserve" makes the build (Compose's or Capture's) find the SDRAM reserve reached after its first bar;
/// "compose_roll" makes it wait after a genre roll, so a script can press BACK there.
bool composeHeld();
bool composeAloneHeld();
bool composeRollHeld();
bool composeReserveHeld();
/// Test hook for the firmware check (spec §21.3): true from a "hold golden" line until "release golden". A genre roll
/// then makes the genre's golden case (golden_case.h: its default Recipe, at the goldens' seed, takes 0), and a Compose
/// build starts in the Groove, as the golden plays from SHIFT+PLAY, so the saved song holds the golden's notes.
bool goldenHeld();
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
/// Test hook for the preset preview (spec §13.3): true from a "hold preset_preview" line until "release
/// preset_preview". Each preview build waits at its start, yielding, until it is cancelled or the hold is released, so
/// a script's SELECT lands while one builds.
bool presetPreviewHeld();
/// Test hook for sound re-rolls, transitions and the Compose screen's card edits: true from a "hold preload" line until
/// "release preload" or releasePreload(). The job waits at its start, yielding, so a script can press BACK while it
/// runs instead of racing it.
bool preloadHeld();
void releasePreload();
/// Test hook for presses that land inside a card wait: true from a "hold card_routine" line until "release
/// card_routine". Injected presses then reach their handlers with sdRoutineLock set, as a press read inside
/// routineForSD() does, so a script can put a release inside a card wait instead of racing a load. A handler that
/// returns REMIND_ME_OUTSIDE_CARD_ROUTINE meanwhile waits until the hold is released.
bool cardRoutineHeld();
/// A preload job's wait at its start while the preload hold is on: it yields at `chunks`' boundaries until the hold
/// is released or the job is cancelled, then turns the hold off, so the next preload runs straight through.
template <typename Chunks>
void waitWhilePreloadHeld(Chunks& chunks) {
	while (preloadHeld() && !chunks.cancelled()) {
		chunks.boundary();
	}
	releasePreload();
}
/// Test hook for a sound re-roll whose preload ends Done after a transition's confirm (#316): true from a "hold
/// sound_committed" line until "release sound_committed" or releaseSoundCommitted(). The sound's job waits just past
/// its chunks.commit(), yielding, so a script's confirm finds a preload it can't cancel.
bool soundCommittedHeld();
void releaseSoundCommitted();
/// The sound job's wait past its commit while the sound_committed hold is on: it yields at `chunks`' boundaries until
/// the hold is released (a cancel no longer stops a committed run), then turns the hold off.
template <typename Chunks>
void waitWhileSoundCommittedHeld(Chunks& chunks) {
	while (soundCommittedHeld()) {
		chunks.boundary();
	}
	releaseSoundCommitted();
}
/// Test hook for retiring Outputs (spec §12.9): true from a "hold reactions" line until "release reactions". Perform's
/// view then skips its display reactions to a sound swap and a new roster (the knob rebind, the focus drop, closing a
/// sound editor on a replaced part), so a script shows that the pump's release alone keeps the UI off a freed Output.
/// The view doesn't count what it skipped as seen: the release plays those reactions, late.
bool reactionsHeld();
/// Test hook for freePartOutput()'s identity check: true from a "hold ui_release" line until "release ui_release". The
/// pump then hands Outputs to the free job without asking the UI to let go, so a script can make the check fail.
bool uiReleaseHeld();
/// Test hook for leaving while Outputs are being freed (spec §13.8): true from a "hold free" line until "release
/// free". Perform's free job waits at its start, yielding, so a script can press LEAVE while it runs.
bool freeHeld();
/// Test hook for a full steering queue: true from a "hold queue_full" line until "release queue_full". A sound pad
/// pressed on a part whose sound waits for its line then finds no room for the cancel, and a transition confirm none
/// for its presses (refused with QUEUE FULL, whatever the phase). A script can't fill the queue itself: it holds 64
/// presses, and every step decision takes them.
bool queueFullHeld();
/// Test hook for a steering log too full to keep the playing piece's header (spec §13.2, §17.2): true from a "hold
/// log_full" line until "release log_full". AS IT BEGAN, playing, then finds no header to restart from. A script can't
/// fill the log quickly: it takes about 45 minutes of busy steering (capture_h13.txt).
bool logFullHeld();
/// Test hook for delay setups (spec §10.6, §11.4): true from a "hold delay_setups" line until "release delay_setups".
/// Perform entry then plays a test copy of the chosen genre that gives its Bass, its Drums kit (kit level) and the
/// Snare row a delay setup, a dotted half on the Bass so that at 60 BPM the render fills the largest delay buffers
/// (spec §20). The copy isn't the genre in the genre list, so a same-genre Regenerate under the hold looks like a genre
/// change.
bool delaySetupsHeld();
/// Test hook for pinning Perform's rolls: from a "hold entropy" line until "release entropy", gives freshEntropy() the
/// next draw of a fixed sequence in *out and returns true; the hold starts the sequence again. Otherwise false.
bool pinnedEntropy(uint32_t* out);
/// How long, in ms on the audio clock, the pad or button with PIC byte `value` has been held by an injected press, or
/// -1 when no injected press holds it (or another was pressed after it): for a TEST_EVENT that reports a hold's
/// length.
int32_t injectedHoldMs(uint8_t value);
/// `samples` of the audio clock in ms, for a TEST_EVENT that reports a length (injectedHoldMs(), gen_end's hold).
int32_t samplesToMs(uint32_t samples);

} // namespace TestPort

#define TEST_EVENT(...) TestPort::event(__VA_ARGS__)

#else

#define TEST_EVENT(...)

#endif
