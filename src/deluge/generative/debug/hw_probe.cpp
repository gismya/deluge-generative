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

#include "generative/debug/hw_probe.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "OSLikeStuff/timers_interrupts/clock_type.h"
#include "OSLikeStuff/timers_interrupts/timers_interrupts.h"
#include "definitions.h"
#include "hid/display/display.h"
#include "hid/display/screensaver.h"
#include "io/midi/test_port.h"
extern "C" {
#include "RZA1/oled/oled_low_level.h"
#include "drivers/oled/oled.h"
extern int oledPendingMessageToSend;
}
#include "fatfs/ff.h"
#include "generative/perform/perform_driver.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace deluge::generative::hwprobe {

namespace {

uint32_t now() {
	return getSystemTicks();
}

uint32_t toUs(uint32_t ticks) {
	return static_cast<uint32_t>(static_cast<uint64_t>(ticks) * 1000000u / DELUGE_CLOCKS_PER);
}

struct MinAvgMax {
	uint32_t min = std::numeric_limits<uint32_t>::max();
	uint64_t sum = 0;
	uint32_t max = 0;
	uint32_t count = 0;

	void add(uint32_t value) {
		min = std::min(min, value);
		max = std::max(max, value);
		sum += value;
		count++;
	}
	[[nodiscard]] uint32_t avg() const { return count ? static_cast<uint32_t>(sum / count) : 0; }
	[[nodiscard]] uint32_t lo() const { return count ? min : 0; }
};

// ---- Commands ----

enum Command : int32_t {
	kBpm,
	kRedraw,
	kRedrawReport,
	kOledState,
	kClockConfig,
	kH14Start,
	kH14Report,
	kH14Dump,
	kH8,
	kKitBuild,
	kH8Dump,
	kKitPrep,
	kNumCommands,
};

constexpr const char* kCommandNames[kNumCommands] = {
    "bpm",      "redraw", "redraw_report", "oled_state", "clockcfg", "h14", "h14_report",
    "h14_dump", "h8",     "kitbuild",      "h8_dump",    "kitprep",
};

// ---- H7 state ----

constexpr int32_t kGridRing = 64;
volatile uint32_t gridTicks[kGridRing];
volatile int32_t lastGridStep = -1;
volatile uint32_t stepTicks = 0; ///< the last 16th's length

bool redrawOn = false;
int32_t redrawRowCount = 0;
bool redrawSidebar = false;
bool oledEveryStep = false;
int32_t shownStep = -1;

int32_t rowsWaitingStep = -1;           ///< step whose rows are rendered but not yet queued
volatile int32_t rowsDrainingStep = -1; ///< step whose rows are queued and draining
volatile int32_t oledWaitingStep = -1;  ///< step whose OLED frame is queued
volatile uint32_t rowsRequestTicks = 0;
volatile uint32_t rowsDrainingRequestTicks = 0;
volatile uint32_t oledRequestTicks = 0;

struct RedrawStats {
	uint32_t startTicks;
	uint32_t steps;
	uint32_t missed;  ///< a newer step's rows replaced rows never queued
	uint32_t skipped; ///< grid lines with no redraw at all
	uint32_t late;    ///< rows drained after their 16th ended
	uint32_t deferred;
	MinAvgMax queueUs;   ///< grid line -> rows queued
	MinAvgMax rowUs;     ///< grid line -> rows out of the UART
	MinAvgMax oledUs;    ///< grid line -> OLED frame out
	MinAvgMax rowReqUs;  ///< rows requested (step shown) -> rows out of the UART
	MinAvgMax oledReqUs; ///< OLED frame requested (step shown) -> frame out
	MinAvgMax pressUs;   ///< injected edge received -> its handler returned
	MinAvgMax stepUs;
	uint32_t oledFrames;
};

RedrawStats stats;

// PIC UART, from ISRs
volatile uint32_t picBusyTicks = 0;
volatile uint32_t picBusySince = 0;
volatile bool picBusy = false;
volatile uint32_t picBytes = 0;
volatile uint32_t oledFramesAll = 0;

void resetStats() {
	ENTER_CRITICAL_SECTION();
	stats = RedrawStats{};
	stats.startTicks = now();
	picBusyTicks = 0;
	picBusySince = stats.startTicks;
	picBytes = 0;
	oledFramesAll = 0;
	rowsWaitingStep = -1;
	rowsDrainingStep = -1;
	oledWaitingStep = -1;
	shownStep = -1;
	EXIT_CRITICAL_SECTION();
}

void reportRedraw() {
	ENTER_CRITICAL_SECTION();
	uint32_t t = now();
	uint32_t busy = picBusyTicks + (picBusy ? t - picBusySince : 0);
	uint32_t bytes = picBytes;
	uint32_t frames = oledFramesAll;
	RedrawStats s = stats;
	EXIT_CRITICAL_SECTION();
	uint32_t elapsedUs = toUs(t - s.startTicks);
	uint32_t busyUs = toUs(busy);
	TEST_EVENT("h7 rows %d side %d oledstep %d steps %u stepus %u elapsed %u busy %u bytes %u",
	           static_cast<int>(redrawRowCount), redrawSidebar ? 1 : 0, oledEveryStep ? 1 : 0,
	           static_cast<unsigned>(s.steps), static_cast<unsigned>(s.stepUs.avg()), static_cast<unsigned>(elapsedUs),
	           static_cast<unsigned>(busyUs), static_cast<unsigned>(bytes));
	TEST_EVENT("h7row n %u lo %u avg %u hi %u late %u missed %u deferred %u q_avg %u q_hi %u",
	           static_cast<unsigned>(s.rowUs.count), static_cast<unsigned>(s.rowUs.lo()),
	           static_cast<unsigned>(s.rowUs.avg()), static_cast<unsigned>(s.rowUs.max), static_cast<unsigned>(s.late),
	           static_cast<unsigned>(s.missed), static_cast<unsigned>(s.deferred),
	           static_cast<unsigned>(s.queueUs.avg()), static_cast<unsigned>(s.queueUs.max));
	TEST_EVENT("h7oled n %u lo %u avg %u hi %u frames %u ravg %u rhi %u", static_cast<unsigned>(s.oledUs.count),
	           static_cast<unsigned>(s.oledUs.lo()), static_cast<unsigned>(s.oledUs.avg()),
	           static_cast<unsigned>(s.oledUs.max), static_cast<unsigned>(frames),
	           static_cast<unsigned>(s.oledReqUs.avg()), static_cast<unsigned>(s.oledReqUs.max));
	TEST_EVENT("h7rowreq avg %u hi %u skipped %u", static_cast<unsigned>(s.rowReqUs.avg()),
	           static_cast<unsigned>(s.rowReqUs.max), static_cast<unsigned>(s.skipped));
	TEST_EVENT("h7press n %u lo %u avg %u hi %u", static_cast<unsigned>(s.pressUs.count),
	           static_cast<unsigned>(s.pressUs.lo()), static_cast<unsigned>(s.pressUs.avg()),
	           static_cast<unsigned>(s.pressUs.max));
}

// ---- H14 state ----

struct HookRecord {
	uint32_t us;   ///< since h14 was armed
	int32_t swung; ///< swung tick position at the hook call
	int32_t delta; ///< since the previous hook call
	int8_t mag;    ///< input tick magnitude at the call
	uint8_t flags; ///< 1 = first call after a clock event, 2 = counted as a jump, 4 = magnitude matching active
	uint8_t event; ///< the last clock event's kind
	uint8_t spare;
};

constexpr int32_t kMaxRecords = 160;
HookRecord records[kMaxRecords];
int32_t numRecords = 0;
bool h14On = false;
uint32_t h14StartTicks = 0;
bool haveLastSwung = false;
int64_t lastSwung = 0;
int32_t lastMag = 0;
int32_t eventKind = 0; ///< the last clock event
int32_t callsSinceEvent = 0;
uint32_t h14Calls = 0;
uint32_t jumpsAfterEvent = 0; ///< jumps on the first hook call after a Start, Continue or SPP: expected
uint32_t falseJumps = 0;      ///< jumps anywhere else
uint32_t magChanges = 0;
int32_t maxDelta = 0;
int32_t clockEvents[5] = {};

void h14Reset() {
	numRecords = 0;
	h14StartTicks = now();
	haveLastSwung = false;
	callsSinceEvent = 1000;
	h14Calls = 0;
	jumpsAfterEvent = 0;
	falseJumps = 0;
	magChanges = 0;
	maxDelta = 0;
	eventKind = 0;
	std::fill(std::begin(clockEvents), std::end(clockEvents), 0);
	lastMag = currentSong ? currentSong->getInputTickMagnitude() : 0;
}

// ---- H8 state ----

enum H8Kind : uint8_t {
	kH8Hid = 1,      ///< an injected pad or button edge's handler returned
	kH8Deferred = 2, ///< an injected edge's handler asked to wait for the card routine to end (first time only)
	kH8Encoder = 3,  ///< an injected encoder turn was taken off the queue
	kH8Knob = 4,     ///< the Perform view's modEncoderAction ran
	kH8Action = 5,   ///< the Perform view ran an H8 action (value: 1 act-at-once pad, 2 Wait-class pad, 3 BACK)
	kH8BuildStart = 6,
	kH8BuildEnd = 7,
};

struct H8Record {
	uint8_t kind;
	uint8_t value;
	int8_t velocity;
	uint8_t flags;      ///< 1 = during a kit build, 2 = sdRoutineLock set
	uint32_t arrivalUs; ///< when the test port queued it (0 if not an injected entry)
	uint32_t us;        ///< when this happened
};

constexpr int32_t kMaxH8Records = 96;
H8Record h8Records[kMaxH8Records];
int32_t numH8Records = 0;
bool h8Mode = false;
bool kitBuilding = false;
uint32_t h8StartTicks = 0;
uint32_t lastDeferredReceived = 0; ///< so a put-back edge is recorded as deferred once
uint32_t encoderArrival[2] = {};   ///< the last gold-knob turn's arrival, per mod encoder

uint32_t h8Us(uint32_t ticks) {
	return ticks == 0 ? 0 : toUs(ticks - h8StartTicks);
}

void h8Record(uint8_t kind, uint8_t value, int32_t velocity, uint32_t arrivalTicks) {
	if (!h8Mode || numH8Records >= kMaxH8Records) {
		return;
	}
	h8Records[numH8Records++] = H8Record{kind,
	                                     value,
	                                     static_cast<int8_t>(std::clamp<int32_t>(velocity, -128, 127)),
	                                     static_cast<uint8_t>((kitBuilding ? 1 : 0) | (sdRoutineLock ? 2 : 0)),
	                                     h8Us(arrivalTicks),
	                                     h8Us(now())};
}

// The factory card's drum folders. kitbuild takes samples from them in this order, so each offset gets new ones.
constexpr const char* kDrumFolders[] = {"Block",  "China", "Clap",       "Claves",  "ConH", "ConL",  "ConM", "Cowbell",
                                        "Crash",  "Guiro", "HatC",       "HatO",    "Kick", "Other", "Ride", "Rim",
                                        "Shaker", "Snare", "Tambourine", "Timpany", "TomH", "TomL",  "TomM"};
constexpr int32_t kMaxKitLanes = 16;
char kitPaths[kMaxKitLanes][160];
char kitNames[kMaxKitLanes][8];
int32_t kitIndexes[kMaxKitLanes]; ///< each lane's sample, as an index into the drum folders
int32_t nextSampleIndex = 0;      ///< the first sample after the last one collected
constexpr int32_t kMaxSamples = 512;
uint8_t badSample[kMaxSamples / 8]; ///< samples a build failed on

bool isBad(int32_t index) {
	return index < kMaxSamples && (badSample[index / 8] & (1 << (index % 8)));
}

/// Whether the file starts like a WAV or AIFF. A plain FatFS read: it doesn't put the sample in the sample cache.
bool looksLikeAudio(const char* path) {
	FIL file;
	if (f_open(&file, path, FA_READ) != FR_OK) {
		return false;
	}
	uint32_t header[3] = {};
	UINT got = 0;
	bool ok = f_read(&file, header, sizeof(header), &got) == FR_OK && got == sizeof(header)
	          && ((header[0] == 0x46464952 && header[2] == 0x45564157)      // RIFF .... WAVE
	              || (header[0] == 0x4D524F46 && header[2] == 0x46464941)); // FORM .... AIFF
	f_close(&file);
	return ok;
}

/// Fills kitPaths with n samples of the drum folders, starting at sample `offset` and skipping files that aren't
/// audio or failed before. Returns how many it found.
int32_t collectSamples(int32_t offset, int32_t n) {
	int32_t index = 0;
	int32_t found = 0;
	for (const char* folder : kDrumFolders) {
		char path[64];
		snprintf(path, sizeof(path), "SAMPLES/DRUMS/%s", folder);
		DIR dir;
		if (f_opendir(&dir, path) != FR_OK) {
			continue;
		}
		FILINFO info;
		while (found < n && f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
			if ((info.fattrib & AM_DIR) || info.fname[0] == '.') {
				continue;
			}
			int32_t thisIndex = index++;
			if (thisIndex >= offset && !isBad(thisIndex)) {
				snprintf(kitPaths[found], sizeof(kitPaths[found]), "%s/%s", path, info.fname);
				if (!looksLikeAudio(kitPaths[found])) {
					continue;
				}
				snprintf(kitNames[found], sizeof(kitNames[found]), "L%d", static_cast<int>(found));
				kitIndexes[found] = thisIndex;
				nextSampleIndex = thisIndex + 1;
				found++;
			}
		}
		f_closedir(&dir);
		if (found >= n) {
			break;
		}
	}
	return found;
}

int32_t preparedOffset = -1;
int32_t preparedCount = 0;
uint32_t preparedScanUs = 0;

/// Picks the samples for the next kitbuild, so the build itself does no folder scan.
void kitPrep(int32_t offset, int32_t n) {
	uint32_t t0 = now();
	preparedCount = collectSamples(offset, std::clamp<int32_t>(n, 1, kMaxKitLanes));
	preparedScanUs = toUs(now() - t0);
	preparedOffset = offset;
	TEST_EVENT("kitprep offset %d lanes %d scan %u next %d", static_cast<int>(offset), static_cast<int>(preparedCount),
	           static_cast<unsigned>(preparedScanUs), static_cast<int>(nextSampleIndex));
}

void kitBuild(int32_t offset, int32_t n) {
	n = std::clamp<int32_t>(n, 1, kMaxKitLanes);
	uint32_t t0 = now();
	int32_t found = preparedOffset == offset ? preparedCount : collectSamples(offset, n);
	preparedOffset = -1;
	uint32_t t1 = now();
	const char* paths[kMaxKitLanes];
	const char* names[kMaxKitLanes];
	for (int32_t i = 0; i < found; i++) {
		paths[i] = kitPaths[i];
		names[i] = kitNames[i];
	}
	Drum* drums[kMaxKitLanes] = {};
	Error error = Error::NONE;
	kitBuilding = true;
	h8Record(kH8BuildStart, static_cast<uint8_t>(found), 0, 0);
	Kit* kit = found > 0 ? perform::buildSampleKit(currentSong, "H8 KIT", paths, names, found, drums, &error) : nullptr;
	uint32_t t2 = now();
	h8Record(kH8BuildEnd, static_cast<uint8_t>(error), 0, 0);
	kitBuilding = false;
	int32_t badLane = -1;
	if (kit != nullptr) {
		perform::deleteUnlinkedInstrument(currentSong, kit);
	}
	else if (error != Error::NONE) {
		// The lane it failed on is the first one it didn't hand back. Never use that sample again.
		badLane = 0;
		while (badLane < found && drums[badLane] != nullptr) {
			badLane++;
		}
		if (badLane < found && kitIndexes[badLane] < kMaxSamples) {
			badSample[kitIndexes[badLane] / 8] |= 1 << (kitIndexes[badLane] % 8);
		}
	}
	uint32_t t3 = now();
	TEST_EVENT("kitbuild offset %d lanes %d scan %u build %u delete %u err %d bad %d next %d", static_cast<int>(offset),
	           static_cast<int>(found), static_cast<unsigned>(toUs(t1 - t0)), static_cast<unsigned>(toUs(t2 - t1)),
	           static_cast<unsigned>(toUs(t3 - t2)), static_cast<int>(error),
	           static_cast<int>(badLane < 0 ? -1 : kitIndexes[badLane]), static_cast<int>(nextSampleIndex));
	if (badLane >= 0 && badLane < found) {
		TEST_EVENT("kitbuild bad %s", kitPaths[badLane]);
	}
}

} // namespace

bool h8On() {
	return h8Mode;
}

void h8Handled(int32_t kind, int32_t value) {
	h8Record(kH8Action, static_cast<uint8_t>(kind), value, 0);
}

void injectedDeferred(uint32_t receivedTicks, uint8_t value, int32_t velocity) {
	if (receivedTicks == lastDeferredReceived) {
		return;
	}
	lastDeferredReceived = receivedTicks;
	h8Record(kH8Deferred, value, velocity, receivedTicks);
}

void encoderEntryTaken(uint32_t receivedTicks, int32_t encoder) {
	if (encoder == 4 || encoder == 5) {
		encoderArrival[encoder == 4 ? 1 : 0] = receivedTicks;
	}
	h8Record(kH8Encoder, static_cast<uint8_t>(encoder), 0, receivedTicks);
}

void goldKnobActioned(int32_t whichModEncoder) {
	uint32_t arrival = encoderArrival[whichModEncoder & 1];
	encoderArrival[whichModEncoder & 1] = 0;
	if (arrival != 0) { // the first action of a turn: its lag
		h8Record(kH8Knob, static_cast<uint8_t>(whichModEncoder), 0, arrival);
	}
}

void driverHook(int64_t swungPos) {
	if (!h14On || currentSong == nullptr) {
		return;
	}
	h14Calls++;
	int32_t mag = currentSong->getInputTickMagnitude();
	int64_t barLength = currentSong->getBarLength();
	int64_t delta = haveLastSwung ? swungPos - lastSwung : 0;
	bool firstAfterEvent = callsSinceEvent == 0;
	bool jump = haveLastSwung && (delta < 0 || delta > barLength);
	if (jump) {
		if (firstAfterEvent) {
			jumpsAfterEvent++;
		}
		else {
			falseJumps++;
			TEST_EVENT("h14 false jump delta %d at %d mag %d bar %d", static_cast<int>(delta),
			           static_cast<int>(swungPos), static_cast<int>(mag), static_cast<int>(barLength));
		}
	}
	if (haveLastSwung && mag != lastMag) {
		magChanges++;
	}
	if (!jump && delta > maxDelta && !firstAfterEvent) {
		maxDelta = static_cast<int32_t>(delta);
	}
	// Keep every call for the first 48 after a clock event, every jump and every magnitude change.
	if (numRecords < kMaxRecords && (callsSinceEvent < 48 || jump || mag != lastMag)) {
		records[numRecords++] =
		    HookRecord{toUs(now() - h14StartTicks),
		               static_cast<int32_t>(swungPos),
		               static_cast<int32_t>(delta),
		               static_cast<int8_t>(mag),
		               static_cast<uint8_t>((firstAfterEvent ? 1 : 0) | (jump ? 2 : 0)
		                                    | (playbackHandler.tempoMagnitudeMatchingActiveNow ? 4 : 0)),
		               static_cast<uint8_t>(eventKind),
		               0};
	}
	lastMag = mag;
	lastSwung = swungPos;
	haveLastSwung = true;
	callsSinceEvent++;
}

void clockEvent(int32_t kind, int32_t value) {
	if (!h14On) {
		return;
	}
	eventKind = kind;
	callsSinceEvent = 0;
	if (kind >= 0 && kind < 5) {
		clockEvents[kind]++;
	}
	TEST_EVENT("h14 clock %d value %d mag %d", static_cast<int>(kind), static_cast<int>(value),
	           currentSong ? static_cast<int>(currentSong->getInputTickMagnitude()) : 0);
}

int32_t commandId(const char* name) {
	for (int32_t i = 0; i < kNumCommands; i++) {
		if (strcmp(name, kCommandNames[i]) == 0) {
			return i;
		}
	}
	return -1;
}

bool command(int32_t id, int32_t a, int32_t b, int32_t c, bool inCardRoutine) {
	switch (id) {
	case kBpm:
		if (currentSong != nullptr && a > 0) {
			currentSong->setBPM(static_cast<float>(a) / 10.f, false);
			TEST_EVENT("probe bpm %d", static_cast<int>(a));
		}
		return true;

	case kRedraw:
		redrawOn = a >= 0;
		redrawRowCount = std::clamp<int32_t>(a, 0, kDisplayHeight);
		redrawSidebar = b != 0;
		oledEveryStep = c != 0;
		resetStats();
		TEST_EVENT("probe redraw rows %d side %d oledstep %d", static_cast<int>(a), static_cast<int>(b),
		           static_cast<int>(c));
		return true;

	case kRedrawReport:
		reportRedraw();
		resetStats();
		return true;

	case kClockConfig:
		// Runtime only: nothing is saved to flash.
		TEST_EVENT("clockcfg was magmatch %d clockin %d", playbackHandler.tempoMagnitudeMatchingEnabled ? 1 : 0,
		           playbackHandler.midiInClockEnabled ? 1 : 0);
		if (a >= 0) {
			playbackHandler.tempoMagnitudeMatchingEnabled = a != 0;
		}
		if (b >= 0) {
			playbackHandler.midiInClockEnabled = b != 0;
		}
		return true;

	case kH14Start:
		h14On = a != 0;
		h14Reset();
		TEST_EVENT("probe h14 %d mag %d", static_cast<int>(a), static_cast<int>(lastMag));
		return true;

	case kH14Report:
		TEST_EVENT("h14rep calls %u records %d false %u expected %u magchanges %u maxdelta %d mag %d", h14Calls,
		           static_cast<int>(numRecords), falseJumps, jumpsAfterEvent, magChanges, static_cast<int>(maxDelta),
		           currentSong ? static_cast<int>(currentSong->getInputTickMagnitude()) : 0);
		TEST_EVENT("h14ev start %d continue %d spp %d stop %d", clockEvents[1], clockEvents[2], clockEvents[3],
		           clockEvents[4]);
		return true;

	case kH14Dump:
		// Records a .. a+b-1, one event each. The host asks for at most 12 at a time.
		for (int32_t i = a; i < std::min<int32_t>(a + std::min<int32_t>(b, 12), numRecords); i++) {
			const HookRecord& r = records[i];
			TEST_EVENT("h14r %d us %u pos %d d %d mag %d fl %d ev %d", static_cast<int>(i), static_cast<unsigned>(r.us),
			           static_cast<int>(r.swung), static_cast<int>(r.delta), static_cast<int>(r.mag),
			           static_cast<int>(r.flags), static_cast<int>(r.event));
		}
		return true;

	case kH8:
		h8Mode = a != 0;
		numH8Records = 0;
		h8StartTicks = now();
		TEST_EVENT("probe h8 %d", static_cast<int>(a));
		return true;

	case kKitBuild:
		if (inCardRoutine) {
			return false;
		}
		kitBuild(a, b);
		return true;

	case kKitPrep:
		if (inCardRoutine) {
			return false;
		}
		kitPrep(a, b);
		return true;

	case kH8Dump:
		if (a == 0) {
			TEST_EVENT("h8n %d", static_cast<int>(numH8Records));
		}
		for (int32_t i = a; i < std::min<int32_t>(a + std::min<int32_t>(b, 12), numH8Records); i++) {
			const H8Record& r = h8Records[i];
			TEST_EVENT("h8r %d k %d v %d vel %d fl %d arr %u us %u", static_cast<int>(i), r.kind, r.value, r.velocity,
			           r.flags, static_cast<unsigned>(r.arrivalUs), static_cast<unsigned>(r.us));
		}
		return true;

	case kOledState:
		TEST_EVENT("oled_state bus %d wait %d pend %d qr %d qw %d frames %u saver %d popup %d",
		           spiBusCurrentlySending ? 1 : 0, oledWaitingForMessage, oledPendingMessageToSend,
		           static_cast<int>(oledFrameQueueReadPos), static_cast<int>(oledFrameQueueWritePos),
		           static_cast<unsigned>(oledFramesAll), deluge::hid::display::Screensaver::isActive() ? 1 : 0,
		           display->hasPopup() ? 1 : 0);
		return true;

	default:
		return true;
	}
}

// ---- H7 ----

namespace {
volatile bool originKnown = false;
int64_t originTick = 0;

void gridLine(int32_t step) {
	uint32_t t = now();
	if (lastGridStep >= 0 && step == lastGridStep + 1) {
		stepTicks = t - gridTicks[lastGridStep & (kGridRing - 1)];
	}
	gridTicks[step & (kGridRing - 1)] = t;
	lastGridStep = step;
}
} // namespace

void generatorStarted(int64_t origin) {
	originTick = origin;
	lastGridStep = -1;
	stepTicks = 0;
	originKnown = true;
}

void generatorStopped() {
	originKnown = false;
}

void audioRoutineEntered() {
	if (!redrawOn || !originKnown || currentSong == nullptr || !playbackHandler.isEitherClockActive()) {
		return;
	}
	int64_t ticksPerStep = currentSong->getQuarterNoteLength() / 4;
	int64_t sinceOrigin = playbackHandler.getActualSwungTickCount() - originTick;
	if (ticksPerStep <= 0 || sinceOrigin < 0) {
		return;
	}
	int32_t step = static_cast<int32_t>(sinceOrigin / ticksPerStep);
	if (step != lastGridStep) {
		gridLine(step); // Up to one render (128 samples, 2.9 ms) after the grid line itself.
	}
}

uint32_t stepShown(int32_t step, bool* oledThisStep) {
	*oledThisStep = false;
	if (!redrawOn || step < 0 || step == shownStep) {
		return 0;
	}
	if (shownStep >= 0 && step > shownStep + 1) {
		stats.skipped += step - shownStep - 1; // grid lines the UI polling never saw
	}
	shownStep = step;
	stats.steps++;
	if (stepTicks != 0) {
		stats.stepUs.add(toUs(stepTicks));
	}
	if (rowsWaitingStep >= 0) {
		stats.missed++;
	}
	uint32_t t = now();
	if (redrawRowCount > 0) {
		rowsWaitingStep = step;
		rowsRequestTicks = t;
	}
	if (oledEveryStep || step % 4 == 0) {
		*oledThisStep = true;
		oledWaitingStep = step;
		oledRequestTicks = t;
	}
	return redrawRowCount > 0 ? (1u << redrawRowCount) - 1 : 0;
}

uint32_t redrawRows() {
	return redrawOn ? (1u << redrawRowCount) - 1 : 0;
}

int32_t redrawStep() {
	return shownStep;
}

bool redrawTestOn() {
	return redrawOn;
}

int32_t gridStep() {
	return lastGridStep;
}

uint32_t redrawSidebarRows() {
	return redrawOn && redrawSidebar ? 0xFF : 0;
}

bool redrawOledEveryStep() {
	return redrawOn && oledEveryStep;
}

void padsQueued() {
	if (rowsWaitingStep < 0) {
		return;
	}
	uint32_t t = now();
	int32_t step = rowsWaitingStep;
	rowsWaitingStep = -1;
	stats.queueUs.add(toUs(t - gridTicks[step & (kGridRing - 1)]));
	// If the previous step's rows are still in the UART, they leave with these and are timed together.
	rowsDrainingRequestTicks = rowsRequestTicks;
	rowsDrainingStep = step;
}

void padsDeferred() {
	if (rowsWaitingStep >= 0) {
		stats.deferred++;
	}
}

void injectedHandled(uint32_t receivedTicks, uint8_t value, int32_t velocity) {
	stats.pressUs.add(toUs(now() - receivedTicks));
	h8Record(kH8Hid, value, velocity, receivedTicks);
}

} // namespace deluge::generative::hwprobe

using namespace deluge::generative::hwprobe;

extern "C" void hwProbePicTxStart(void) {
	if (!picBusy) {
		picBusy = true;
		picBusySince = now();
	}
}

extern "C" void hwProbePicTxIdle(void) {
	uint32_t t = now();
	if (picBusy) {
		picBusy = false;
		picBusyTicks = picBusyTicks + (t - picBusySince);
	}
	int32_t step = rowsDrainingStep;
	if (step >= 0) {
		rowsDrainingStep = -1;
		uint32_t latency = t - gridTicks[step & (kGridRing - 1)];
		stats.rowUs.add(toUs(latency));
		stats.rowReqUs.add(toUs(t - rowsDrainingRequestTicks));
		if (stepTicks != 0 && latency > stepTicks) {
			stats.late++;
		}
	}
}

extern "C" void hwProbePicBytes(int32_t number) {
	picBytes = picBytes + number;
}

extern "C" void hwProbeOledFrameDone(void) {
	oledFramesAll = oledFramesAll + 1;
	int32_t step = oledWaitingStep;
	if (step >= 0) {
		oledWaitingStep = -1;
		uint32_t t = now();
		stats.oledUs.add(toUs(t - gridTicks[step & (kGridRing - 1)]));
		stats.oledReqUs.add(toUs(t - oledRequestTicks));
		stats.oledFrames++;
	}
}

#endif // ENABLE_SYSEX_TEST
