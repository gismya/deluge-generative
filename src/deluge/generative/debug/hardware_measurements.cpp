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

#include "generative/debug/hardware_measurements.h"

#if GENERATIVE_MEASUREMENTS

#include "OSLikeStuff/scheduler_api.h"
extern "C" {
#include "RZA1/oled/oled_low_level.h"
}
#include "OSLikeStuff/timers_interrupts/clock_type.h"
#include "definitions.h"
#include "drivers/pic/pic.h"
#include "generative/perform/perform_driver.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/generative_perform_view.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/pad_leds.h"
#include "io/debug/log.h"
#include "io/midi/midi_engine.h"
#include "memory/general_memory_allocator.h"
#include "model/instrument/instrument.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace deluge::generative::measure {

namespace {

// ---- Time ----

constexpr uint32_t kClocksPerSecond = DELUGE_CLOCKS_PER;
/// An audio render later than this after the previous one has run the 128-sample output buffer dry.
constexpr uint32_t kLateGapTicks =
    static_cast<uint32_t>(static_cast<uint64_t>(SSI_TX_BUFFER_NUM_SAMPLES) * kClocksPerSecond / kSampleRate);

uint32_t now() {
	return getSystemTicks();
}

uint32_t toUs(uint32_t ticks) {
	return static_cast<uint32_t>(static_cast<uint64_t>(ticks) * 1000000u / kClocksPerSecond);
}

// ---- Log output ----

bool midiOutputDrained() {
	return !midiEngine.anythingInOutputBuffer();
}

/// One log line. Lets the MIDI output drain after each one: a burst of lines overflows the USB and DIN sysex
/// buffers, which drop what doesn't fit. Only called after the timed part, from UI context, so yielding is fine.
void logLine(char const* format, ...) __attribute__((format(printf, 1, 2)));
void logLine(char const* format, ...) {
	char line[200];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	D_PRINTLN("%s", line);
	midiEngine.flushMIDI();
	yieldWithTimeout(midiOutputDrained, 0.1);
}

// ---- Audio gaps ----

volatile uint32_t lastAudioTicks = 0;
volatile uint32_t maxGapTicks = 0;
volatile uint32_t lateGaps = 0;
/// Off until the first measurement, so boot's long gaps (card scan, song setup) aren't counted.
volatile bool watchingAudio = false;

struct AudioGaps {
	uint32_t maxGapUs;
	uint32_t late;
};

/// Returns the gaps since the last call and starts counting afresh.
AudioGaps takeAudioGaps() {
	watchingAudio = true;
	AudioGaps gaps{toUs(maxGapTicks), lateGaps};
	maxGapTicks = 0;
	lateGaps = 0;
	return gaps;
}

// ---- Memory ----

constexpr char const* kRegionNames[NUM_MEMORY_REGIONS] = {"stealable", "internal", "external", "external_small",
                                                          "internal_small"};

struct RegionStats {
	uint32_t size;
	uint32_t free;
	uint32_t largest;
};

struct Memory {
	RegionStats region[NUM_MEMORY_REGIONS];
	uint32_t cacheBytes; ///< Stealables waiting in the reclamation queues: sample data the allocator can evict.
	uint32_t cacheItems;
};

Memory takeMemory() {
	GeneralMemoryAllocator& allocator = GeneralMemoryAllocator::get();
	Memory memory{};
	for (int32_t i = 0; i < NUM_MEMORY_REGIONS; i++) {
		MemoryRegion& region = allocator.regions[i];
		RegionStats& stats = memory.region[i];
		stats.size = region.end - region.start;
		for (int32_t j = 0; j < region.emptySpaces.getNumElements(); j++) {
			auto* space = static_cast<EmptySpaceRecord*>(region.emptySpaces.getElementAddress(j));
			stats.free += space->length;
			stats.largest = std::max(stats.largest, space->length);
		}
	}
	for (int32_t q = 0; q < kNumStealableQueue; q++) {
		BidirectionalLinkedList& list = allocator.cacheManager.queue(static_cast<StealableQueue>(q));
		for (BidirectionalLinkedListNode* node = list.getFirst(); node != nullptr; node = list.getNext(node)) {
			memory.cacheBytes += allocator.getAllocatedSize(node);
			memory.cacheItems++;
		}
	}
	return memory;
}

/// Free bytes in the fast (on-chip) heap: the internal region and its small-object region.
uint32_t freeFast(const Memory& m) {
	return m.region[MEMORY_REGION_INTERNAL].free + m.region[MEMORY_REGION_INTERNAL_SMALL].free;
}
/// Free bytes in the 2 MiB SDRAM general heap (operator new, containers) and its small-object region.
uint32_t freeExternal(const Memory& m) {
	return m.region[MEMORY_REGION_EXTERNAL].free + m.region[MEMORY_REGION_EXTERNAL_SMALL].free;
}
uint32_t freeStealable(const Memory& m) {
	return m.region[MEMORY_REGION_STEALABLE].free;
}

/// RAM taken between two snapshots, per group. Negative means freed.
struct Used {
	int32_t fast;
	int32_t external;
	int32_t stealable;
};

Used used(const Memory& before, const Memory& after) {
	return Used{static_cast<int32_t>(freeFast(before) - freeFast(after)),
	            static_cast<int32_t>(freeExternal(before) - freeExternal(after)),
	            static_cast<int32_t>(freeStealable(before) - freeStealable(after))};
}

void logMemory(char const* label, const Memory& m) {
	for (int32_t i = 0; i < NUM_MEMORY_REGIONS; i++) {
		const RegionStats& r = m.region[i];
		logLine("MEAS %s mem %s size %u free %u largest %u", label, kRegionNames[i], static_cast<unsigned>(r.size),
		        static_cast<unsigned>(r.free), static_cast<unsigned>(r.largest));
	}
	logLine("MEAS %s mem cache %u bytes in %u stealables", label, static_cast<unsigned>(m.cacheBytes),
	        static_cast<unsigned>(m.cacheItems));
	logLine("MEAS %s mem free fast %u external %u stealable %u (+cache %u)", label, static_cast<unsigned>(freeFast(m)),
	        static_cast<unsigned>(freeExternal(m)), static_cast<unsigned>(freeStealable(m)),
	        static_cast<unsigned>(m.cacheBytes));
}

// ---- The summary popup ----

char summary[128] = "No measurement yet";

void showSummary() {
	if (display->haveOLED()) {
		display->popupText(summary);
		uiTimerManager.setTimer(TimerName::DISPLAY, 30000); // Long enough to copy down, then it clears itself.
	}
	else {
		display->displayPopup("MEAS");
	}
}

/// Appends a line to `summary`.
void addSummaryLine(char const* format, ...) __attribute__((format(printf, 1, 2)));
void addSummaryLine(char const* format, ...) {
	size_t length = strlen(summary);
	if (length > 0 && length < sizeof(summary) - 1) {
		summary[length++] = '\n';
		summary[length] = 0;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(summary + length, sizeof(summary) - length, format, args);
	va_end(args);
}

/// "12.3" for 12345 µs.
void formatMs(char* buffer, size_t size, uint32_t us) {
	snprintf(buffer, size, "%u.%u", static_cast<unsigned>(us / 1000), static_cast<unsigned>((us % 1000) / 100));
}

// ---- Timed state ----

constexpr int32_t kMaxPhases = 8;
constexpr int32_t kMaxLanes = 16;

struct Phase {
	char const* name;
	uint32_t ticks;
};

struct LaneResult {
	uint32_t ticks;
	Used used;
};

struct TimedState {
	char const* name;
	bool playing;
	uint32_t startTicks;
	uint32_t phaseStartTicks;
	uint32_t laneStartTicks;
	Memory startMemory;
	Memory laneMemory;
	Phase phases[kMaxPhases];
	int32_t numPhases;
	LaneResult lanes[kMaxLanes];
	int32_t numLanes;
	char const* memoryPhaseName;
	Memory memoryAtPhase;
	bool finished;
};

TimedState state;
bool timedOpen = false;

bool clustersLoaded() {
	return audioFileManager.loadingQueue.empty();
}

bool picTxIdle() {
	return uartGetTxBufferFullnessByItem(UART_ITEM_PIC_PADS) == 0;
}

/// The pad UART is idle and the OLED isn't waiting on it, so a pad redraw is queued now rather than deferred.
bool picTxClear() {
	return picTxIdle() && !oledIsWaitingForPicTxDrain();
}

// ---- The triggered measurements ----

// A factory synth preset of typical size (4 KB of XML, no samples). It's also Perform's Bass, so loading it in
// Perform first makes no difference: presets aren't cached, only samples are.
constexpr char const* kSynthFolder = "SYNTHS";
constexpr char const* kSynthName = "047 Basic Dirty Bass";
constexpr char const* kSynthPath = "SYNTHS/047 Basic Dirty Bass.XML";
// A factory kit: 57 KB of XML, 16 drums of 808 samples.
constexpr char const* kKitFolder = "KITS";
constexpr char const* kKitName = "000 TR-808";
constexpr char const* kKitPath = "KITS/000 TR-808.XML";

// The 12-lane kit: one factory Linndrum sample per lane. No other factory kit but "006 LD" uses these, and Perform
// uses 909 samples, so the first build after power-on reads every sample from the card.
constexpr int32_t kKit12Lanes = 12;
constexpr char const* kKit12Paths[kKit12Lanes] = {
    "SAMPLES/DRUMS/Kick/Linndrum Kick.wav",
    "SAMPLES/DRUMS/Snare/Linndrum Snare.wav",
    "SAMPLES/DRUMS/Clap/Linndrum Clap.wav",
    "SAMPLES/DRUMS/HatC/Linndrum Closed hihat.wav",
    "SAMPLES/DRUMS/HatO/Linndrum Open hihat.wav",
    "SAMPLES/DRUMS/Rim/Linndrum Rim.wav",
    "SAMPLES/DRUMS/Tambourine/Linndrum Tambourine.wav",
    "SAMPLES/DRUMS/Shaker/Linndrum Cabasa.wav",
    "SAMPLES/DRUMS/Crash/Linndrum Crash.wav",
    "SAMPLES/DRUMS/Ride/Linndrum Ride.wav",
    "SAMPLES/DRUMS/TomL/Linndrum Tom low.wav",
    "SAMPLES/DRUMS/TomH/Linndrum Tom high.wav",
};
constexpr char const* kKit12Names[kKit12Lanes] = {"Kick", "Snare",  "Clap",  "Closed hat", "Open hat", "Rim",
                                                  "Tamb", "Cabasa", "Crash", "Ride",       "Tom low",  "Tom high"};

/// Loads a preset into the current song without linking it, as Perform loads its Bass, then deletes it.
void measurePresetLoad(OutputType type, char const* label, char const* path, char const* folderName,
                       char const* presetName) {
	Instrument* instrument = nullptr;
	{
		Timed timed(label);
		FilePointer filePointer;
		Error error = Error::NONE;
		if (!StorageManager::fileExists(path, &filePointer)) {
			error = Error::FILE_NOT_FOUND;
		}
		else {
			String name;
			String folder;
			name.set(presetName);
			folder.set(folderName);
			error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, type, false, &instrument, &filePointer,
			                                               &name, &folder);
			timed.phase("parse");
			if (error == Error::NONE && instrument != nullptr) {
				instrument->loadAllAudioFiles(true);
				timed.phase("files");
				yieldWithTimeout(clustersLoaded, 10.0);
				timed.phase("clusters");
			}
		}
		timed.setError(error);
	} // Reports here, while the preset is still in RAM.
	if (instrument != nullptr) {
		perform::deleteUnlinkedInstrument(currentSong, instrument);
	}
}

/// Builds a 12-lane kit on Perform's code path, waits for its samples, reports, then deletes it.
void measureKitBuild() {
	Kit* kit = nullptr;
	{
		Timed timed("kit12");
		Drum* drums[kKit12Lanes] = {};
		Error error = Error::NONE;
		kit = perform::buildSampleKit(currentSong, "MEAS KIT", kKit12Paths, kKit12Names, kKit12Lanes, drums, &error);
		timed.phase("build");
		if (kit != nullptr) {
			yieldWithTimeout(clustersLoaded, 10.0);
			timed.phase("clusters");
		}
		timed.setError(error);
	}
	if (kit != nullptr) {
		perform::deleteUnlinkedInstrument(currentSong, kit);
	}
}

void reportMemory() {
	Memory memory = takeMemory();
	bool wasWatching = watchingAudio;
	AudioGaps gaps = takeAudioGaps();
	logLine("MEAS mem begin playing=%d", playbackHandler.isEitherClockActive() ? 1 : 0);
	logMemory("mem", memory);
	if (wasWatching) {
		logLine("MEAS mem audio maxgap %u us late %u (since the last measurement)",
		        static_cast<unsigned>(gaps.maxGapUs), static_cast<unsigned>(gaps.late));
	}
	else {
		logLine("MEAS mem audio gaps not watched before the first measurement");
	}

	char gap[16];
	formatMs(gap, sizeof(gap), gaps.maxGapUs);
	summary[0] = 0;
	addSummaryLine("Free fast %uK", static_cast<unsigned>(freeFast(memory) / 1024));
	addSummaryLine("ext %uK", static_cast<unsigned>(freeExternal(memory) / 1024));
	addSummaryLine("steal %uK c%uK", static_cast<unsigned>(freeStealable(memory) / 1024),
	               static_cast<unsigned>(memory.cacheBytes / 1024));
	addSummaryLine("gap %sms late %u", gap, static_cast<unsigned>(gaps.late));
	showSummary();
}

struct MinAvgMax {
	uint32_t min = UINT32_MAX;
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
};

/// Full redraws of the main grid and sidebar: the current UI renders into the pad image, the image is queued to the
/// PIC UART, and the UART drains. Pad UART runs at 200 kbaud, so the drain is the wire time.
void measurePadRedraw() {
	constexpr int32_t kRuns = 16;
	if (!yieldWithTimeout(picTxIdle, 1.0)) {
		strcpy(summary, "Pads: PIC UART\nnever idle");
		showSummary();
		return;
	}
	UI* ui = getCurrentUI();
	MinAvgMax render;
	MinAvgMax queue;
	MinAvgMax drain;
	MinAvgMax total;
	uint32_t bytes = 0;
	int32_t deferred = 0;
	takeAudioGaps();
	// The OLED shares the PIC UART. While it waits for the UART to drain, sendOutMainPadColours() defers and queues
	// nothing, so each run waits for a clear UART, and a run that still queued nothing is counted, not timed.
	for (int32_t run = 0; run < kRuns && deferred < kRuns;) {
		yieldWithTimeout(picTxClear, 0.5);
		uint32_t t0 = now();
		ui->renderMainPads(0xFFFFFFFF, PadLEDs::image, PadLEDs::occupancyMask, true);
		ui->renderSidebar(0xFFFFFFFF, PadLEDs::image, PadLEDs::occupancyMask);
		uint32_t t1 = now();
		PadLEDs::sendOutMainPadColours();
		PadLEDs::sendOutSidebarColours();
		uint32_t t2 = now();
		uint32_t queued = uartGetTxBufferFullnessByItem(UART_ITEM_PIC_PADS);
		yieldWithTimeout(picTxIdle, 0.5);
		uint32_t t3 = now();
		if (queued == 0) {
			deferred++;
			continue;
		}
		bytes = queued;
		render.add(toUs(t1 - t0));
		queue.add(toUs(t2 - t1));
		drain.add(toUs(t3 - t2));
		total.add(toUs(t3 - t0));
		run++;
	}
	AudioGaps gaps = takeAudioGaps();

	char const* uiName = (ui == &sessionView) ? "session" : (ui == &generativePerformView) ? "perform" : "other";
	logLine("MEAS pads ui %s runs %u deferred %d bytes %u", uiName, static_cast<unsigned>(render.count),
	        static_cast<int>(deferred), static_cast<unsigned>(bytes));
	logLine("MEAS pads render min %u avg %u max %u us", static_cast<unsigned>(render.min),
	        static_cast<unsigned>(render.avg()), static_cast<unsigned>(render.max));
	logLine("MEAS pads queue min %u avg %u max %u us", static_cast<unsigned>(queue.min),
	        static_cast<unsigned>(queue.avg()), static_cast<unsigned>(queue.max));
	logLine("MEAS pads uart min %u avg %u max %u us", static_cast<unsigned>(drain.min),
	        static_cast<unsigned>(drain.avg()), static_cast<unsigned>(drain.max));
	logLine("MEAS pads total min %u avg %u max %u us", static_cast<unsigned>(total.min),
	        static_cast<unsigned>(total.avg()), static_cast<unsigned>(total.max));
	logLine("MEAS pads audio maxgap %u us late %u", static_cast<unsigned>(gaps.maxGapUs),
	        static_cast<unsigned>(gaps.late));

	char renderMs[16];
	char drainMs[16];
	char totalMs[16];
	formatMs(renderMs, sizeof(renderMs), render.avg());
	formatMs(drainMs, sizeof(drainMs), drain.min);
	formatMs(totalMs, sizeof(totalMs), total.max);
	summary[0] = 0;
	addSummaryLine("Pads %uB", static_cast<unsigned>(bytes));
	addSummaryLine("render %sms", renderMs);
	addSummaryLine("uart %sms", drainMs);
	addSummaryLine("max all %sms", totalMs);
	showSummary();
}

using Action = void (*)();

void synthAction() {
	measurePresetLoad(OutputType::SYNTH, "synth", kSynthPath, kSynthFolder, kSynthName);
}
void kitAction() {
	measurePresetLoad(OutputType::KIT, "kit", kKitPath, kKitFolder, kKitName);
}

bool busy = false;

} // namespace

// ---- Audio hook ----

void audioRoutineEntered() {
	uint32_t t = getSystemTicks();
	uint32_t last = lastAudioTicks;
	lastAudioTicks = t;
	if (last == 0 || !watchingAudio) {
		return;
	}
	uint32_t gap = t - last;
	if (gap > maxGapTicks) {
		maxGapTicks = gap;
	}
	if (gap > kLateGapTicks) {
		lateGaps = lateGaps + 1;
	}
}

// ---- Timed ----

Timed::Timed(char const* name, Error const* error) : active_(!timedOpen), errorSource_(error) {
	if (!active_) {
		return;
	}
	timedOpen = true;
	state = TimedState{};
	state.name = name;
	state.playing = playbackHandler.isEitherClockActive();
	state.startMemory = takeMemory();
	state.laneMemory = state.startMemory;
	takeAudioGaps();
	state.startTicks = now();
	state.phaseStartTicks = state.startTicks;
	state.laneStartTicks = state.startTicks;
}

void Timed::phase(char const* name) {
	if (!active_ || state.finished) {
		return;
	}
	uint32_t t = now();
	if (state.numPhases < kMaxPhases) {
		state.phases[state.numPhases++] = Phase{name, t - state.phaseStartTicks};
	}
	state.phaseStartTicks = t;
	state.laneStartTicks = t;
}

void Timed::phaseWithMemory(char const* name) {
	if (!active_ || state.finished) {
		return;
	}
	phase(name);
	state.memoryPhaseName = name;
	state.memoryAtPhase = takeMemory();
	// The snapshot's own time goes to no phase.
	state.phaseStartTicks = now();
	state.laneStartTicks = state.phaseStartTicks;
}

void laneDone(int32_t lane) {
	if (!timedOpen || state.finished) {
		return;
	}
	uint32_t t = now();
	Memory memory = takeMemory();
	if (state.numLanes < kMaxLanes) {
		state.lanes[state.numLanes++] = LaneResult{t - state.laneStartTicks, used(state.laneMemory, memory)};
	}
	state.laneMemory = memory;
	state.laneStartTicks = now();
}

Timed::~Timed() {
	if (!active_) {
		return;
	}
	uint32_t totalTicks = now() - state.startTicks;
	if (errorSource_ != nullptr) {
		error_ = *errorSource_;
	}
	Memory endMemory = takeMemory();
	AudioGaps gaps = takeAudioGaps();
	state.finished = true;
	char const* name = state.name;

	logLine("MEAS %s begin playing=%d error=%d", name, state.playing ? 1 : 0, static_cast<int>(error_));
	uint64_t laneTicks = 0;
	int64_t laneFast = 0;
	int64_t laneExternal = 0;
	int64_t laneStealable = 0;
	for (int32_t i = 0; i < state.numLanes; i++) {
		const LaneResult& lane = state.lanes[i];
		logLine("MEAS %s lane %d %u us fast %+d external %+d stealable %+d", name, static_cast<int>(i),
		        static_cast<unsigned>(toUs(lane.ticks)), static_cast<int>(lane.used.fast),
		        static_cast<int>(lane.used.external), static_cast<int>(lane.used.stealable));
		laneTicks += lane.ticks;
		laneFast += lane.used.fast;
		laneExternal += lane.used.external;
		laneStealable += lane.used.stealable;
	}
	uint32_t laneAvgUs = 0;
	int32_t laneAvgBytes = 0;
	if (state.numLanes > 0) {
		laneAvgUs = toUs(static_cast<uint32_t>(laneTicks / state.numLanes));
		laneAvgBytes = static_cast<int32_t>((laneFast + laneExternal) / state.numLanes);
		logLine("MEAS %s lane avg %u us fast %+d external %+d stealable %+d", name, static_cast<unsigned>(laneAvgUs),
		        static_cast<int>(laneFast / state.numLanes), static_cast<int>(laneExternal / state.numLanes),
		        static_cast<int>(laneStealable / state.numLanes));
	}
	for (int32_t i = 0; i < state.numPhases; i++) {
		logLine("MEAS %s phase %s %u us", name, state.phases[i].name,
		        static_cast<unsigned>(toUs(state.phases[i].ticks)));
	}
	uint32_t totalUs = toUs(totalTicks);
	logLine("MEAS %s total %u us", name, static_cast<unsigned>(totalUs));
	Used total = used(state.startMemory, endMemory);
	logLine("MEAS %s used fast %+d external %+d stealable %+d", name, static_cast<int>(total.fast),
	        static_cast<int>(total.external), static_cast<int>(total.stealable));
	if (state.memoryPhaseName != nullptr) {
		char label[48];
		snprintf(label, sizeof(label), "%s@%s", name, state.memoryPhaseName);
		logMemory(label, state.memoryAtPhase);
	}
	logMemory(name, endMemory);
	logLine("MEAS %s audio maxgap %u us late %u", name, static_cast<unsigned>(gaps.maxGapUs),
	        static_cast<unsigned>(gaps.late));

	// The popup: at most 4 lines of 19 characters.
	char ms[16];
	summary[0] = 0;
	formatMs(ms, sizeof(ms), totalUs);
	if (error_ != Error::NONE) {
		addSummaryLine("%s ERR %d", name, static_cast<int>(error_));
	}
	else {
		addSummaryLine("%s %sms", name, ms);
	}
	if (state.numLanes > 0) {
		formatMs(ms, sizeof(ms), laneAvgUs);
		addSummaryLine("lane %sms %dB", ms, static_cast<int>(laneAvgBytes));
	}
	else if (state.numPhases > 0) {
		formatMs(ms, sizeof(ms), toUs(state.phases[0].ticks));
		addSummaryLine("%s %sms", state.phases[0].name, ms);
	}
	if (state.memoryPhaseName != nullptr) {
		addSummaryLine("@%s fast %uK", state.memoryPhaseName,
		               static_cast<unsigned>(freeFast(state.memoryAtPhase) / 1024));
	}
	else {
		addSummaryLine("used %dK", static_cast<int>((total.fast + total.external) / 1024));
	}
	formatMs(ms, sizeof(ms), gaps.maxGapUs);
	addSummaryLine("gap %sms late %u", ms, static_cast<unsigned>(gaps.late));
	showSummary();

	timedOpen = false;
}

// ---- Triggers ----

std::optional<ActionResult> buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (b == LEARN || !Buttons::isButtonPressed(LEARN)) {
		return std::nullopt;
	}
	Action action = nullptr;
	switch (b) {
	case SYNTH:
		action = synthAction;
		break;
	case KIT:
		action = kitAction;
		break;
	case MIDI:
		action = measureKitBuild;
		break;
	case CV:
		action = reportMemory;
		break;
	case KEYBOARD:
		action = measurePadRedraw;
		break;
	case SCALE_MODE:
		action = showSummary;
		break;
	default:
		return std::nullopt;
	}
	if (!on) {
		return ActionResult::DEALT_WITH;
	}
	if (inCardRoutine) {
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	}
	if (busy) {
		return ActionResult::DEALT_WITH;
	}
	// Holding LEARN started MIDI learn in most views; leave it, so the measurement runs in the normal UI mode.
	view.endMIDILearn();
	if (currentUIMode != UI_MODE_NONE) {
		display->displayPopup("BUSY");
		return ActionResult::DEALT_WITH;
	}

	busy = true;
	bool showWorking = (action != showSummary && action != reportMemory) && display->haveOLED();
	if (showWorking) {
		deluge::hid::display::OLED::displayWorkingAnimation("Measuring");
	}
	action();
	if (showWorking) {
		display->removeWorkingAnimation();
	}
	busy = false;
	return ActionResult::DEALT_WITH;
}

} // namespace deluge::generative::measure

#endif
