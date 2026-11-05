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
	kNumCommands,
};

constexpr const char* kCommandNames[kNumCommands] = {
    "bpm",
    "redraw",
    "redraw_report",
    "oled_state",
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

} // namespace

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

void injectedHandled(uint32_t receivedTicks, uint8_t /*value*/, int32_t /*velocity*/) {
	stats.pressUs.add(toUs(now() - receivedTicks));
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
