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

#include "io/midi/test_port.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "generative/debug/hw_probe.h"
#include "gui/ui/ui.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/encoders.h"
#include "hid/matrix/pad.h"
#include "io/debug/log.h"
#include "io/debug/print.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "util/pack.h"
#include <algorithm>
#include <charconv>
#include <cstdarg>
#include <cstring>
#include <strings.h>

namespace TestPort {
namespace {

namespace encoders = deluge::hid::encoders;

// ---- Input queue ----
// Written by the MIDI routine (tail), read by the buttons-and-pads task (head). Both run as scheduler tasks, never
// from an ISR, and each index has one writer, so no locking is needed.

enum class Kind : uint8_t { Hid, Encoder, Wait, Probe };

struct Entry {
	Kind kind;
	uint8_t index;   ///< Hid: PIC byte. Encoder: 0 scroll Y, 1 scroll X, 2 tempo, 3 select, 4 upper gold, 5 lower gold.
	int32_t amount;  ///< Hid: velocity, 0 = release. Encoder: signed detents. Wait: milliseconds. Probe: command id.
	int32_t args[3]; ///< Probe: arguments
	uint32_t receivedTicks; ///< getSystemTicks() when the MIDI routine queued it
};

constexpr uint32_t kQueueSize = 64; // a power of two
// One slot stays free, so putBackHidEvent() never lands on a slot the MIDI routine has refilled since the take.
constexpr uint32_t kQueueCapacity = kQueueSize - 1;
Entry queue[kQueueSize];
uint32_t queueHead = 0;
uint32_t queueTail = 0;

uint32_t queueDepth() {
	return queueTail - queueHead;
}

double resumeAt = 0; ///< getSystemTime() at which a "wait" entry ends
int32_t busyEncoder = -1;
int32_t goldEdgesLeft = 0; ///< Signed edges still to feed to busyEncoder (4 or 5)
double nextGoldEdgeAt = 0;

constexpr int32_t kEdgesPerGoldDetent = 4; // one quadrature cycle, as the encoder hardware and delugemu produce
/// Gold-knob edges this far apart keep EncoderAcceleration at 1x (its steady-state multiplier is 0.15 s / interval),
/// so each edge moves the parameter by exactly one step. Faster feeding would multiply the turn by up to 3.
constexpr double kGoldEdgeInterval = 0.16;

/// Pads and buttons held down by injected events, by PIC byte.
uint32_t injectedHeld[256 / 32];

encoders::ContinuousEncoder& goldEncoder(int32_t index) {
	return encoders::modEncoderAt(index == 4 ? 1 : 0);
}

bool encoderBusy() {
	if (busyEncoder < 0) {
		return false;
	}
	bool busy = busyEncoder < (int32_t)encoders::kNumFunctionEncoders
	                ? encoders::functionEncoderAt(busyEncoder).pending()
	                : goldEdgesLeft != 0 || goldEncoder(busyEncoder).pending();
	if (!busy) {
		busyEncoder = -1;
	}
	return busy;
}

/// True while an encoder turn or a wait taken from the queue is still running.
bool entryRunning() {
	return encoderBusy() || getSystemTime() < resumeAt;
}

// ---- Parsing ----

constexpr int32_t kMaxTokens = 5;

struct Tokens {
	char* token[kMaxTokens];
	int32_t count;
};

bool isSeparator(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Tokens tokenize(char* line) {
	Tokens t{{}, 0};
	char* p = line;
	while (*p != 0) {
		while (isSeparator(*p)) {
			*p++ = 0;
		}
		if (*p == 0) {
			break;
		}
		if (t.count == kMaxTokens) {
			t.count = kMaxTokens + 1; // too many
			break;
		}
		t.token[t.count++] = p;
		while (*p != 0 && !isSeparator(*p)) {
			p++;
		}
	}
	return t;
}

bool parseInt(const char* s, int32_t min, int32_t max, int32_t* out) {
	if (*s == '+') {
		s++;
	}
	const char* end = s + strlen(s);
	int32_t value;
	auto [last, error] = std::from_chars(s, end, value);
	if (error != std::errc{} || last != end || value < min || value > max) {
		return false;
	}
	*out = value;
	return true;
}

bool parseDown(const char* s, bool* down) {
	if (strcmp(s, "down") == 0) {
		*down = true;
		return true;
	}
	if (strcmp(s, "up") == 0) {
		*down = false;
		return true;
	}
	return false;
}

bool buttonFromName(const char* name, uint8_t* value) {
	for (int32_t y = 0; y < NUM_BUTTON_ROWS; y++) {
		for (int32_t x = 0; x < NUM_BUTTON_COLS; x++) {
			uint8_t b = deluge::hid::button::fromXY(x, y);
			if (strcasecmp(Buttons::getButtonName(b), name) == 0) {
				*value = b;
				return true;
			}
		}
	}
	return false;
}

/// Line syntax, as delugemu's dz-event plus names and waits:
///   button <x> <y> down|up      button at matrix column x (0-8), row y (0-3)
///   button <NAME> down|up       a name from Buttons::getButtonName(), e.g. SHIFT, SESSION_VIEW
///   pad <x> <y> down|up [vel]   grid pad, x 0-17 (16 and 17 are the sidebar), y 0-7, vel 1-127
///   encoder <i> <detents>       0 scroll Y, 1 scroll X, 2 tempo, 3 select, 4 upper gold, 5 lower gold
///   wait <ms>                   holds back the following entries
///   probe <name> [a] [b] [c]    runs a stand-in probe command, see generative/debug/hw_probe.cpp
bool parseLine(char* line, Entry* entry) {
	Tokens t = tokenize(line);
	if (t.count < 2 || t.count > kMaxTokens) {
		return false;
	}
	const char* kind = t.token[0];
	int32_t x, y;
	bool down;

	if (strcmp(kind, "button") == 0) {
		uint8_t value;
		if (t.count == 3 && buttonFromName(t.token[1], &value) && parseDown(t.token[2], &down)) {}
		else if (t.count == 4 && parseInt(t.token[1], 0, NUM_BUTTON_COLS - 1, &x)
		         && parseInt(t.token[2], 0, NUM_BUTTON_ROWS - 1, &y) && parseDown(t.token[3], &down)) {
			value = deluge::hid::button::fromXY(x, y);
		}
		else {
			return false;
		}
		*entry = {Kind::Hid, value, down ? USE_DEFAULT_VELOCITY : 0, {}, 0};
		return true;
	}

	if (strcmp(kind, "pad") == 0) {
		int32_t velocity = USE_DEFAULT_VELOCITY;
		if (t.count < 4 || !parseInt(t.token[1], 0, kDisplayWidth + kSideBarWidth - 1, &x)
		    || !parseInt(t.token[2], 0, kDisplayHeight - 1, &y) || !parseDown(t.token[3], &down)
		    || (t.count == 5 && (!down || !parseInt(t.token[4], 1, 127, &velocity)))) {
			return false;
		}
		*entry = {Kind::Hid, Pad(x, y).toChar(), down ? velocity : 0, {}, 0};
		return true;
	}

	if (strcmp(kind, "encoder") == 0) {
		int32_t detents;
		if (t.count != 3 || !parseInt(t.token[1], 0, 5, &x) || !parseInt(t.token[2], -1000, 1000, &detents)) {
			return false;
		}
		*entry = {Kind::Encoder, static_cast<uint8_t>(x), detents, {}, 0};
		return true;
	}

	if (strcmp(kind, "probe") == 0) {
		int32_t id = deluge::generative::hwprobe::commandId(t.token[1]);
		if (id < 0) {
			return false;
		}
		*entry = {Kind::Probe, 0, id, {0, 0, 0}, 0};
		for (int32_t i = 2; i < t.count; i++) {
			if (!parseInt(t.token[i], -100000000, 100000000, &entry->args[i - 2])) {
				return false;
			}
		}
		return true;
	}

	if (strcmp(kind, "wait") == 0) {
		int32_t ms;
		if (t.count != 2 || !parseInt(t.token[1], 0, 60000, &ms)) {
			return false;
		}
		*entry = {Kind::Wait, 0, ms, {}, 0};
		return true;
	}

	return false;
}

// ---- Replies ----

const uint8_t kReplyHeader[] = {SysEx::SYSEX_START,           SysEx::DELUGE_SYSEX_ID_BYTE0,
                                SysEx::DELUGE_SYSEX_ID_BYTE1, SysEx::DELUGE_SYSEX_ID_BYTE2,
                                SysEx::DELUGE_SYSEX_ID_BYTE3, SysEx::SysexCommands::HID};

void sendStatus(MIDICable& cable, uint8_t seq, Status status) {
	uint8_t reply[sizeof(kReplyHeader) + 4];
	memcpy(reply, kReplyHeader, sizeof(kReplyHeader));
	reply[sizeof(kReplyHeader)] = kStatusReply;
	reply[sizeof(kReplyHeader) + 1] = seq;
	reply[sizeof(kReplyHeader) + 2] = static_cast<uint8_t>(status);
	reply[sizeof(kReplyHeader) + 3] = SysEx::SYSEX_END;
	cable.sendSysex(reply, sizeof(reply));
}

/// Checks the optional key after the sequence byte: 00 <key:5>. Returns the offset of the rest of the payload, or -1
/// for a wrong key. `data` is the payload from the HID command byte on, ending in F7.
int32_t checkKey(uint8_t* data, int32_t len) {
	constexpr int32_t kStart = 3;
	if (len < kStart + 7 || data[kStart] != 0x00) {
		return kStart;
	}
	uint32_t received;
	unpack_7bit_to_8bit(reinterpret_cast<uint8_t*>(&received), sizeof(received), data + kStart + 1, 5);
	uint32_t key = runtimeFeatureSettings.get(RuntimeFeatureSettingType::DevSysexAllowed);
	return (key != 0 && received == key) ? kStart + 6 : -1;
}

// ---- Events ----
// Written by event() (tail), sent by sendPendingEvent() (head), both from task context.

constexpr uint32_t kEventQueueSize = 16; // a power of two
constexpr size_t kEventLength = 120;
char events[kEventQueueSize][kEventLength];
uint32_t eventHead = 0;
uint32_t eventTail = 0;
uint32_t eventsDropped = 0;

} // namespace

void injectReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 4) {
		return;
	}
	uint8_t seq = data[2] & 0x7F;
	int32_t start = checkKey(data, len);
	if (start < 0) {
		sendStatus(cable, seq, Status::BadKey);
		return;
	}

	char line[64];
	int32_t lineLength = len - 1 - start; // drop the F7
	if (lineLength <= 0 || lineLength >= (int32_t)sizeof(line)) {
		sendStatus(cable, seq, Status::ParseError);
		return;
	}
	memcpy(line, data + start, lineLength);
	line[lineLength] = 0;

	Entry entry;
	if (!parseLine(line, &entry)) {
		sendStatus(cable, seq, Status::ParseError);
		return;
	}
	if (entry.kind == Kind::Probe && deluge::generative::hwprobe::runsOnArrival(entry.amount)) {
		deluge::generative::hwprobe::command(entry.amount, entry.args[0], entry.args[1], entry.args[2], false);
		sendStatus(cable, seq, Status::Queued);
		return;
	}
	if (queueDepth() >= kQueueCapacity) {
		sendStatus(cable, seq, Status::QueueFull);
		return;
	}
	entry.receivedTicks = getSystemTicks();
	queue[queueTail % kQueueSize] = entry;
	queueTail++;
	sendStatus(cable, seq, Status::Queued);
}

void stateReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 4) {
		return;
	}
	uint8_t seq = data[2] & 0x7F;
	if (checkKey(data, len) < 0) {
		sendStatus(cable, seq, Status::BadKey);
		return;
	}

	UI* ui = getCurrentUI();
	RootUI* root = getRootUI();
	uint8_t reply[sizeof(kReplyHeader) + 2 + 400];
	size_t pos = sizeof(kReplyHeader);
	memcpy(reply, kReplyHeader, pos);
	reply[pos++] = kStateReply;
	reply[pos++] = seq;
	size_t room = sizeof(reply) - pos; // the F7 goes where snprintf puts its 0
	int32_t jsonLength = snprintf(
	    reinterpret_cast<char*>(reply + pos), room,
	    "{\"ui\":\"%s\",\"root_ui\":\"%s\",\"ui_mode\":%u,\"playing\":%s,\"bpm\":%.2f,\"swing\":%d,"
	    "\"voices\":%d,\"cpu_direness\":%d,\"sd_routine_lock\":%s,\"accessing_card\":%u,"
	    "\"inject_queue\":%u,\"events_dropped\":%u,\"sample_timer\":%u}",
	    ui ? ui->getUIName() : "none", root ? root->getUIName() : "none", static_cast<unsigned>(currentUIMode),
	    playbackHandler.isEitherClockActive() ? "true" : "false",
	    currentSong ? playbackHandler.calculateBPMForDisplay() : 0.0f, currentSong ? currentSong->swingAmount : 0,
	    static_cast<int>(AudioEngine::getNumVoices()), static_cast<int>(AudioEngine::cpuDireness),
	    sdRoutineLock ? "true" : "false", static_cast<unsigned>(currentlyAccessingCard),
	    static_cast<unsigned>(queueDepth() + (entryRunning() ? 1 : 0)), static_cast<unsigned>(eventsDropped),
	    static_cast<unsigned>(AudioEngine::audioSampleTimer));
	pos += std::min<int32_t>(jsonLength, room - 1);
	reply[pos++] = SysEx::SYSEX_END;
	cable.sendSysex(reply, pos);
}

std::optional<HidEvent> takeHidEvent() {
	while (queueHead != queueTail) {
		if (entryRunning()) {
			return std::nullopt;
		}
		Entry entry = queue[queueHead % kQueueSize];
		queueHead++;
		switch (entry.kind) {
		case Kind::Hid: {
			uint32_t bit = 1u << (entry.index % 32);
			if (entry.amount != 0) {
				injectedHeld[entry.index / 32] |= bit;
			}
			else {
				injectedHeld[entry.index / 32] &= ~bit;
			}
			return HidEvent{entry.index, entry.amount, entry.receivedTicks};
		}

		case Kind::Probe:
			if (!deluge::generative::hwprobe::command(entry.amount, entry.args[0], entry.args[1], entry.args[2],
			                                          sdRoutineLock)) {
				queueHead--; // Runs again once the card routine has ended.
				return std::nullopt;
			}
			break;

		case Kind::Wait:
			resumeAt = getSystemTime() + entry.amount / 1000.0;
			break;

		case Kind::Encoder:
			deluge::generative::hwprobe::encoderEntryTaken(entry.receivedTicks, entry.index);
			busyEncoder = entry.index;
			if (entry.index < encoders::kNumFunctionEncoders) {
				encoders::functionEncoderAt(entry.index).restore(entry.amount);
			}
			else {
				goldEdgesLeft = entry.amount * kEdgesPerGoldDetent;
			}
			unblockTask(encoders::EncoderTaskID);
			break;
		}
	}
	return std::nullopt;
}

void putBackHidEvent() {
	queueHead--;
}

bool holdingInjectedPresses() {
	for (uint32_t word : injectedHeld) {
		if (word != 0) {
			return true;
		}
	}
	return false;
}

void feedEncoderEdges() {
	if (goldEdgesLeft == 0 || busyEncoder < (int32_t)encoders::kNumFunctionEncoders) {
		return;
	}
	auto& encoder = goldEncoder(busyEncoder);
	// One edge per take, never a batch, and slow enough to stay clear of the acceleration.
	double now = getSystemTime();
	if (!encoder.pending() && now >= nextGoldEdgeAt) {
		int8_t edge = goldEdgesLeft > 0 ? 1 : -1;
		encoder.apply_edges(edge);
		goldEdgesLeft -= edge;
		nextGoldEdgeAt = now + kGoldEdgeInterval;
	}
}

void event(const char* format, ...) {
	if (Debug::midiDebugCable == nullptr) {
		return;
	}
	if (eventTail - eventHead >= kEventQueueSize) {
		eventsDropped++;
		return;
	}
	char* line = events[eventTail % kEventQueueSize];
	int32_t prefix = snprintf(line, kEventLength, "%.4f: ", getSystemTime());
	va_list args;
	va_start(args, format);
	vsnprintf(line + prefix, kEventLength - prefix, format, args);
	va_end(args);
	eventTail++;
}

void sendPendingEvent() {
	if (eventHead == eventTail) {
		return;
	}
	if (Debug::midiDebugCable == nullptr) {
		eventHead = eventTail;
		return;
	}
	// Bursts overflow the USB and DIN sysex buffers, which drop what doesn't fit. So one line at a time, each after
	// the previous one has left.
	if (midiEngine.anythingInOutputBuffer()) {
		midiEngine.flushMIDI();
		return;
	}
	Debug::sysexDebugPrint(*Debug::midiDebugCable, events[eventHead % kEventQueueSize], true, kEventCategory);
	midiEngine.flushMIDI();
	eventHead++;
}

} // namespace TestPort

#endif // ENABLE_SYSEX_TEST
