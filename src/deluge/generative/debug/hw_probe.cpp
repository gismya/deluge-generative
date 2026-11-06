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
#include "generative/debug/compose_shape.h"
#include "generative/perform/perform_driver.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/browser/browser.h"
#include "gui/ui/ui.h"
#include "io/midi/midi_follow.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/fx/stutterer.h"
#include "model/instrument/instrument.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/clip_iterators.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/file_item.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <cctype>
#include <cmath>
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
	kBusy,
	kH1Report,
	kBurst,
	kH3Dump,
	kGlide,
	kGlideDump,
	kBrowserName,
	kRenderBurn,
	kLaneRoll,
	kRoster,
	kCompose,
	kComposeCancel,
	kSongSave,
	kSongCount,
	kAudioClean,
	kGentestDelete,
	kTLink,
	kRenderWindow,
	kTHit,
	kTRetire,
	kTTail,
	kRoll,
	kNumCommands,
};

// clang-format off
constexpr const char* kCommandNames[] = {
    "bpm",
    "redraw",
    "redraw_report",
    "oled_state",
    "clockcfg",
    "h14",
    "h14_report",
    "h14_dump",
    "h8",
    "kitbuild",
    "h8_dump",
    "kitprep",
    "busy",
    "h1_report",
    "burst",
    "h3_dump",
    "glide",
    "glide_dump",
    "browser_name",
    "renderburn",
    "laneroll",
    "roster",
    "compose",
    "composecancel",
    "songsave",
    "songcount",
    "audioclean",
    "gentestdelete",
    "tlink",
    "rwin",
    "thit",
    "tretire",
    "ttail",
    "roll",
};
// clang-format on
static_assert(sizeof(kCommandNames) / sizeof(kCommandNames[0]) == kNumCommands, "one name per command");

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

// ---- Audio, from every render ----

constexpr uint32_t kLateGapTicks =
    static_cast<uint32_t>(static_cast<uint64_t>(SSI_TX_BUFFER_NUM_SAMPLES) * DELUGE_CLOCKS_PER / kSampleRate);
volatile uint32_t renderCount = 0;
volatile uint32_t lastRenderTicks = 0;
volatile uint32_t windowMaxGap = 0;
volatile uint32_t windowLate = 0;
volatile int32_t windowMaxDireness = 0;
volatile uint32_t windowMaxVoices = 0;
volatile uint32_t windowCulled = 0;
int32_t renderBurnUs = 0; ///< H3: extra time spent in every render, to push cpuDireness up

void resetAudioWindow() {
	windowMaxGap = 0;
	windowLate = 0;
	windowMaxDireness = 0;
	windowMaxVoices = 0;
	windowCulled = 0;
}

void spinUs(int32_t us) {
	uint32_t until = static_cast<uint32_t>(static_cast<uint64_t>(us) * DELUGE_CLOCKS_PER / 1000000u);
	uint32_t start = now();
	while (now() - start < until) {}
}

// ---- H1: busy loop on decision ticks ----

int32_t busyUs = 0;
uint32_t decisionTicks = 0;
uint32_t busyLoops = 0;

// ---- H3: bursts of note-ons ----

constexpr int32_t kMaxBurstNotes = 16;
constexpr int32_t kMaxBursts = 24;
struct Burst {
	int8_t direness;
	uint8_t notes;
	uint8_t voicesBefore;
	int8_t waited[kMaxBurstNotes]; ///< renders between the burst and each note's voice start, -1 = never seen
	int8_t order[kMaxBurstNotes];  ///< the order the voices started in
};
Burst bursts[kMaxBursts];
int32_t numBursts = 0;
int32_t burstSize = 0;     ///< 0 = off
int64_t nextBurstTick = 0; ///< Generator ticks
int64_t burstOffTick = -1;
Sound* burstSounds[kMaxBurstNotes];
uint32_t burstRender = 0;
int32_t burstStarted = 0;
Burst* currentBurst = nullptr;
constexpr int64_t kTicksPerBar = 384;

// ---- H4: tempo glides ----

int32_t glideFromX10 = 0;
int32_t glideToX10 = 0;
int64_t glideStart = -1; ///< Generator ticks; -1 = no glide armed
int64_t glideLength = 0;
int64_t glideLogUntil = 0;
constexpr int32_t kMaxGlideSteps = 200;
struct GlideStep {
	int32_t step;
	uint32_t sample;     ///< audioSampleTimer when the hook ran
	uint32_t tickSample; ///< the timer tick's own scheduled time (PlaybackHandler::timeLastTimerTickBig)
	int32_t bpmX100;
};
GlideStep glideSteps[kMaxGlideSteps];
int32_t numGlideSteps = 0;
int32_t glideBpmX100 = 0;

void setTempo(double bpm) {
	// As Song::setBPMInner(), without writing tempo automation: §12.7 calls setTempoFromNumSamples() directly.
	int32_t magnitude = currentSong->insideWorldTickMagnitude;
	if (magnitude > 0) {
		bpm *= static_cast<double>(1u << magnitude);
	}
	double timePerTimerTick = 110250.0 / bpm;
	if (magnitude < 0) {
		timePerTimerTick *= static_cast<double>(1u << -magnitude);
	}
	currentSong->setTempoFromNumSamples(timePerTimerTick, false);
}

// ---- H6: lane rolls (folder scans) ----

uint32_t rngState = 1;
uint32_t nextRandom() {
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

struct LaneFolders {
	const char* name;
	const char* folders[10];
	int32_t count;
};

// §10.2's default table.
constexpr LaneFolders kLaneFolders[] = {
    {"Kick", {"Kick"}, 1},
    {"Snare", {"Snare"}, 1},
    {"Clap", {"Clap"}, 1},
    {"Rim", {"Rim"}, 1},
    {"HatC", {"HatC"}, 1},
    {"HatO", {"HatO"}, 1},
    {"Ride", {"Ride"}, 1},
    {"Crash", {"Crash"}, 1},
    {"Tom", {"TomL", "TomM", "TomH"}, 3},
    {"Perc1", {"Shaker", "ConL", "ConM", "ConH", "Cowbell", "Tambourine", "Claves", "Block", "Guiro", "Other"}, 10},
    {"Perc2", {"Shaker", "ConL", "ConM", "ConH", "Cowbell", "Tambourine", "Claves", "Block", "Guiro", "Other"}, 10},
};
constexpr int32_t kNumRosterLanes = sizeof(kLaneFolders) / sizeof(kLaneFolders[0]);
constexpr int32_t kYieldEveryEntries = 32;

struct RollStats {
	uint32_t maxRunTicks;   ///< the longest stretch between two yields
	uint32_t maxEntryTicks; ///< the slowest single f_readdir()
	uint32_t maxOpenTicks;  ///< the slowest f_opendir()
	uint32_t entries;
	uint32_t candidates;
	uint32_t yields;
};

bool isAudioName(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot != nullptr
	       && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".aif") == 0 || strcasecmp(dot, ".aiff") == 0);
}

bool nameHasTag(const char* name, const char* tag) {
	if (tag == nullptr || *tag == 0) {
		return true;
	}
	size_t n = strlen(tag);
	for (const char* p = name; *p != 0 && *p != '.'; p++) {
		if (strncasecmp(p, tag, n) == 0) {
			return true;
		}
	}
	return false;
}

/// One lane's roll as §10.2–10.3 describe it: every file in the lane's folders, audio files only, those matching the
/// tag, one picked at random. Yields (the audio routine and cluster loading) after every folder and every 32 entries.
bool rollLane(int32_t lane, const char* tag, char* pathOut, size_t pathSize, RollStats& stats) {
	const LaneFolders& def = kLaneFolders[lane];
	uint32_t runStart = now();
	auto yieldHere = [&]() {
		uint32_t t = now();
		stats.maxRunTicks = std::max(stats.maxRunTicks, t - runStart);
		AudioEngine::routineWithClusterLoading();
		stats.yields++;
		runStart = now();
	};
	uint32_t seen = 0;
	bool found = false;
	for (int32_t f = 0; f < def.count; f++) {
		char folder[64];
		snprintf(folder, sizeof(folder), "SAMPLES/DRUMS/%s", def.folders[f]);
		DIR dir;
		uint32_t t0 = now();
		FRESULT opened = f_opendir(&dir, folder);
		stats.maxOpenTicks = std::max(stats.maxOpenTicks, now() - t0);
		if (opened != FR_OK) {
			continue;
		}
		FILINFO info;
		int32_t sinceYield = 0;
		while (true) {
			uint32_t e0 = now();
			FRESULT r = f_readdir(&dir, &info);
			stats.maxEntryTicks = std::max(stats.maxEntryTicks, now() - e0);
			if (r != FR_OK || info.fname[0] == 0) {
				break;
			}
			stats.entries++;
			if (!(info.fattrib & AM_DIR) && info.fname[0] != '.' && isAudioName(info.fname)
			    && nameHasTag(info.fname, tag)) {
				stats.candidates++;
				seen++;
				if (nextRandom() % seen == 0) { // reservoir sampling: each candidate equally likely
					snprintf(pathOut, pathSize, "%s/%s", folder, info.fname);
					found = true;
				}
			}
			if (++sinceYield >= kYieldEveryEntries) {
				sinceYield = 0;
				yieldHere();
			}
		}
		f_closedir(&dir);
		yieldHere();
	}
	stats.maxRunTicks = std::max(stats.maxRunTicks, now() - runStart);
	return found;
}

/// laneroll: which = lane index, 11 = Perc (Perc1), 12 = Tom, 99 = the whole roster (all 11 lanes).
void laneRollCommand(int32_t which, int32_t useTag, int32_t seed) {
	rngState = seed != 0 ? static_cast<uint32_t>(seed) : 1;
	RollStats stats{};
	resetAudioWindow();
	int32_t first = which;
	int32_t last = which;
	if (which == 11) {
		first = last = 9;
	}
	else if (which == 12) {
		first = last = 8;
	}
	else if (which == 99) {
		first = 0;
		last = kNumRosterLanes - 1;
	}
	const char* tag = useTag != 0 ? "808" : nullptr;
	uint32_t t0 = now();
	int32_t picked = 0;
	char path[160];
	for (int32_t lane = first; lane <= last && lane >= 0 && lane < kNumRosterLanes; lane++) {
		if (rollLane(lane, tag, path, sizeof(path), stats)) {
			picked++;
		}
	}
	uint32_t total = now() - t0;
	TEST_EVENT("h6 which %d tag %d us %u maxrun %u maxentry %u maxopen %u entries %u cand %u picked %d late %u",
	           static_cast<int>(which), static_cast<int>(useTag), static_cast<unsigned>(toUs(total)),
	           static_cast<unsigned>(toUs(stats.maxRunTicks)), static_cast<unsigned>(toUs(stats.maxEntryTicks)),
	           static_cast<unsigned>(toUs(stats.maxOpenTicks)), static_cast<unsigned>(stats.entries),
	           static_cast<unsigned>(stats.candidates), static_cast<int>(picked), static_cast<unsigned>(windowLate));
}

// ---- H5: a full roster preload ----

struct Memory {
	uint32_t fastFree;
	uint32_t fastLargest;
	uint32_t extFree;
};

/// Free bytes and the number of free gaps in one region.
struct RegionFree {
	uint32_t free;
	int32_t gaps;
};

RegionFree regionFree(int32_t region) {
	MemoryRegion& r = GeneralMemoryAllocator::get().regions[region];
	RegionFree f{0, r.emptySpaces.getNumElements()};
	for (int32_t j = 0; j < f.gaps; j++) {
		f.free += static_cast<EmptySpaceRecord*>(r.emptySpaces.getElementAddress(j))->length;
	}
	return f;
}

/// After a teardown: each general region's free bytes and gaps, and how many AudioFiles the manager still knows.
void reportRegions(const char* tag) {
	RegionFree in = regionFree(MEMORY_REGION_INTERNAL);
	RegionFree inSmall = regionFree(MEMORY_REGION_INTERNAL_SMALL);
	RegionFree ext = regionFree(MEMORY_REGION_EXTERNAL);
	RegionFree extSmall = regionFree(MEMORY_REGION_EXTERNAL_SMALL);
	TEST_EVENT("%s in %u/%d ins %u/%d ex %u/%d exs %u/%d files %d", tag, static_cast<unsigned>(in.free),
	           static_cast<int>(in.gaps), static_cast<unsigned>(inSmall.free), static_cast<int>(inSmall.gaps),
	           static_cast<unsigned>(ext.free), static_cast<int>(ext.gaps), static_cast<unsigned>(extSmall.free),
	           static_cast<int>(extSmall.gaps), static_cast<int>(audioFileManager.audioFiles.getNumElements()));
}

Memory takeMemory() {
	GeneralMemoryAllocator& allocator = GeneralMemoryAllocator::get();
	Memory m{};
	for (int32_t region :
	     {MEMORY_REGION_INTERNAL, MEMORY_REGION_INTERNAL_SMALL, MEMORY_REGION_EXTERNAL, MEMORY_REGION_EXTERNAL_SMALL}) {
		MemoryRegion& r = allocator.regions[region];
		for (int32_t j = 0; j < r.emptySpaces.getNumElements(); j++) {
			uint32_t length = static_cast<EmptySpaceRecord*>(r.emptySpaces.getElementAddress(j))->length;
			if (region == MEMORY_REGION_INTERNAL || region == MEMORY_REGION_INTERNAL_SMALL) {
				m.fastFree += length;
				if (region == MEMORY_REGION_INTERNAL) {
					m.fastLargest = std::max(m.fastLargest, length);
				}
			}
			else {
				m.extFree += length;
			}
		}
	}
	return m;
}

bool clustersLoaded() {
	return audioFileManager.loadingQueue.empty();
}

/// The largest SYNTHS preset whose name contains `want`, or with want = nullptr the `index`th preset not containing
/// "pad". Fills path and name (name without the extension). Returns false if none.
bool pickPreset(const char* want, int32_t index, char* path, size_t pathSize, char* name, size_t nameSize) {
	DIR dir;
	if (f_opendir(&dir, "SYNTHS") != FR_OK) {
		return false;
	}
	FILINFO info;
	uint32_t bestSize = 0;
	int32_t seenOthers = 0;
	bool found = false;
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
		if ((info.fattrib & AM_DIR) || info.fname[0] == '.') {
			continue;
		}
		const char* dot = strrchr(info.fname, '.');
		if (dot == nullptr || strcasecmp(dot, ".xml") != 0) {
			continue;
		}
		bool match = want != nullptr ? nameHasTag(info.fname, want) && info.fsize > bestSize
		                             : !nameHasTag(info.fname, "pad") && seenOthers++ == index;
		if (match) {
			bestSize = info.fsize;
			snprintf(path, pathSize, "SYNTHS/%s", info.fname);
			snprintf(name, nameSize, "%.*s", static_cast<int>(dot - info.fname), info.fname);
			found = true;
		}
	}
	f_closedir(&dir);
	return found;
}

Instrument* loadSynthPreset(const char* path, const char* presetName, Error* error) {
	FilePointer filePointer;
	if (!StorageManager::fileExists(path, &filePointer)) {
		*error = Error::FILE_NOT_FOUND;
		return nullptr;
	}
	String name;
	String folder;
	name.set(presetName);
	folder.set("SYNTHS");
	Instrument* instrument = nullptr;
	*error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, OutputType::SYNTH, false, &instrument,
	                                                &filePointer, &name, &folder);
	if (*error != Error::NONE || instrument == nullptr) {
		return nullptr;
	}
	instrument->loadAllAudioFiles(true);
	return instrument;
}

SoundDrum* loadSynthDrum(const char* path, const char* presetName, Error* error) {
	FilePointer filePointer;
	if (!StorageManager::fileExists(path, &filePointer)) {
		*error = Error::FILE_NOT_FOUND;
		return nullptr;
	}
	String name;
	String folder;
	name.set(presetName);
	folder.set("SYNTHS");
	SoundDrum* drum = nullptr;
	*error = StorageManager::loadSynthToDrum(currentSong, nullptr, false, &drum, &filePointer, &name, &folder);
	if (*error != Error::NONE || drum == nullptr) {
		return nullptr;
	}
	drum->loadAllSamples(true);
	drum->drumName = presetName;
	return drum;
}

void rosterCommand(int32_t seed) {
	rngState = seed != 0 ? static_cast<uint32_t>(seed) : 1;
	resetAudioWindow();
	Memory before = takeMemory();
	uint32_t t0 = now();
	Error error = Error::NONE;

	// 11 drum lanes, each from a folder scan.
	static char lanePaths[kNumRosterLanes][160];
	const char* paths[kNumRosterLanes];
	const char* names[kNumRosterLanes];
	RollStats stats{};
	int32_t lanes = 0;
	for (int32_t lane = 0; lane < kNumRosterLanes; lane++) {
		if (rollLane(lane, nullptr, lanePaths[lanes], sizeof(lanePaths[lanes]), stats)) {
			paths[lanes] = lanePaths[lanes];
			names[lanes] = kLaneFolders[lane].name;
			lanes++;
		}
	}
	uint32_t tScan = now();
	Drum* drums[kNumRosterLanes] = {};
	Kit* drumKit = perform::buildSampleKit(currentSong, "H5 DRUMS", paths, names, lanes, drums, &error);
	uint32_t tDrums = now();

	// The FX kit: 2 sample lanes and 2 synth rows through loadSynthToDrum.
	static char fxPaths[2][160];
	const char* fxP[2];
	const char* fxNames[2] = {"FX1", "FX2"};
	int32_t fxSamples = 0;
	for (int32_t i = 0; i < 2; i++) {
		if (rollLane(9, nullptr, fxPaths[i], sizeof(fxPaths[i]), stats)) {
			fxP[fxSamples++] = fxPaths[i];
		}
	}
	Drum* fxDrums[2] = {};
	Error fxError = Error::NONE;
	Kit* fxKit = perform::buildSampleKit(currentSong, "H5 FX", fxP, fxNames, fxSamples, fxDrums, &fxError);
	int32_t synthRows = 0;
	for (int32_t i = 0; i < 2 && fxKit != nullptr; i++) {
		char path[160];
		char name[64];
		Error e = Error::NONE;
		if (pickPreset(nullptr, 7 + static_cast<int32_t>(nextRandom() % 40), path, sizeof(path), name, sizeof(name))) {
			SoundDrum* drum = loadSynthDrum(path, name, &e);
			if (drum != nullptr) {
				fxKit->addDrum(drum);
				synthRows++;
			}
		}
		if (e != Error::NONE && fxError == Error::NONE) {
			fxError = e;
		}
	}
	uint32_t tFx = now();

	// 4 synths: the heaviest pad and 3 others.
	Instrument* synths[4] = {};
	int32_t numSynths = 0;
	uint32_t padSize = 0;
	Error synthError = Error::NONE;
	for (int32_t i = 0; i < 4; i++) {
		char path[160];
		char name[64];
		bool ok = i == 0 ? pickPreset("pad", 0, path, sizeof(path), name, sizeof(name))
		                 : pickPreset(nullptr, static_cast<int32_t>(nextRandom() % 60), path, sizeof(path), name,
		                              sizeof(name));
		if (!ok) {
			continue;
		}
		Error e = Error::NONE;
		Instrument* instrument = loadSynthPreset(path, name, &e);
		if (instrument != nullptr) {
			synths[numSynths++] = instrument;
			if (i == 0) {
				FILINFO info;
				padSize = f_stat(path, &info) == FR_OK ? info.fsize : 0;
			}
		}
		else if (synthError == Error::NONE) {
			synthError = e;
		}
	}
	uint32_t tSynths = now();
	yieldWithTimeout(clustersLoaded, 10.0);
	uint32_t tClusters = now();
	Memory peak = takeMemory();
	uint32_t late = windowLate;
	uint32_t maxGap = windowMaxGap;

	// Tear down.
	for (int32_t i = 0; i < numSynths; i++) {
		perform::deleteUnlinkedInstrument(currentSong, synths[i]);
	}
	if (fxKit != nullptr) {
		perform::deleteUnlinkedInstrument(currentSong, fxKit);
	}
	if (drumKit != nullptr) {
		perform::deleteUnlinkedInstrument(currentSong, drumKit);
	}
	uint32_t tEnd = now();
	Memory after = takeMemory();
	reportRegions("h5x");
	TEST_EVENT("h5 seed %d lanes %d fx %d+%d synths %d err %d/%d/%d pad %u", static_cast<int>(seed),
	           static_cast<int>(lanes), static_cast<int>(fxSamples), static_cast<int>(synthRows),
	           static_cast<int>(numSynths), static_cast<int>(error), static_cast<int>(fxError),
	           static_cast<int>(synthError), static_cast<unsigned>(padSize));
	TEST_EVENT("h5t scan %u drums %u fx %u synths %u clusters %u total %u teardown %u",
	           static_cast<unsigned>(toUs(tScan - t0)), static_cast<unsigned>(toUs(tDrums - tScan)),
	           static_cast<unsigned>(toUs(tFx - tDrums)), static_cast<unsigned>(toUs(tSynths - tFx)),
	           static_cast<unsigned>(toUs(tClusters - tSynths)), static_cast<unsigned>(toUs(tClusters - t0)),
	           static_cast<unsigned>(toUs(tEnd - tClusters)));
	TEST_EVENT("h5m before %u peak %u largest %u after %u ext %u/%u late %u gap %u maxrun %u",
	           static_cast<unsigned>(before.fastFree), static_cast<unsigned>(peak.fastFree),
	           static_cast<unsigned>(peak.fastLargest), static_cast<unsigned>(after.fastFree),
	           static_cast<unsigned>(before.extFree), static_cast<unsigned>(peak.extFree), static_cast<unsigned>(late),
	           static_cast<unsigned>(toUs(maxGap)), static_cast<unsigned>(toUs(stats.maxRunTicks)));
}

// ---- H10-H12: Compose- and Capture-shaped songs ----

uint32_t cacheBytes() {
	GeneralMemoryAllocator& allocator = GeneralMemoryAllocator::get();
	uint32_t bytes = 0;
	for (int32_t q = 0; q < kNumStealableQueue; q++) {
		BidirectionalLinkedList& list = allocator.cacheManager.queue(static_cast<StealableQueue>(q));
		for (BidirectionalLinkedListNode* node = list.getFirst(); node != nullptr; node = list.getNext(node)) {
			bytes += allocator.getAllocatedSize(node);
		}
	}
	return bytes;
}

uint32_t stealableFree() {
	MemoryRegion& r = GeneralMemoryAllocator::get().regions[MEMORY_REGION_STEALABLE];
	uint32_t free = 0;
	for (int32_t j = 0; j < r.emptySpaces.getNumElements(); j++) {
		free += static_cast<EmptySpaceRecord*>(r.emptySpaces.getElementAddress(j))->length;
	}
	return free;
}

/// compose: builds a song of `occurrences` x 6 parts over `bars` bars, measures it, and deletes it (private song) or
/// keeps it (intoCurrent, for songsave).
void composeCommand(int32_t occurrences, int32_t bars, int32_t budgetUs, bool intoCurrent) {
	resetAudioWindow();
	Memory before = takeMemory();
	uint32_t cacheBefore = cacheBytes();
	uint32_t stealBefore = stealableFree();
	compose_shape::Shape shape{occurrences, bars, budgetUs, intoCurrent};
	compose_shape::Result result{};
	Song* song = compose_shape::build(shape, &result);
	Memory peak = takeMemory();
	uint32_t cachePeak = cacheBytes();
	uint32_t stealPeak = stealableFree();
	uint32_t late = windowLate;
	uint32_t gap = windowMaxGap;
	uint32_t t0 = now();
	if (!intoCurrent) {
		compose_shape::deleteSong(song);
	}
	uint32_t teardownUs = toUs(now() - t0);
	Memory after = takeMemory();
	TEST_EVENT("h10 occ %d bars %d/%d budget %d err %d clips %d notes %d nodes %d cancel %d lat %u",
	           static_cast<int>(occurrences), static_cast<int>(result.barsBuilt), static_cast<int>(bars),
	           static_cast<int>(budgetUs), static_cast<int>(result.error), static_cast<int>(result.clips),
	           static_cast<int>(result.notes), static_cast<int>(result.nodes), result.cancelled ? 1 : 0,
	           static_cast<unsigned>(result.cancelLatencyUs));
	TEST_EVENT("h10t build %u longest %u yields %u teardown %u late %u gap %u", static_cast<unsigned>(result.buildUs),
	           static_cast<unsigned>(result.longestRunUs), static_cast<unsigned>(result.yields),
	           static_cast<unsigned>(teardownUs), static_cast<unsigned>(late), static_cast<unsigned>(toUs(gap)));
	TEST_EVENT("h10m fast %u/%u/%u largest %u ext %u/%u/%u", static_cast<unsigned>(before.fastFree),
	           static_cast<unsigned>(peak.fastFree), static_cast<unsigned>(after.fastFree),
	           static_cast<unsigned>(peak.fastLargest), static_cast<unsigned>(before.extFree),
	           static_cast<unsigned>(peak.extFree), static_cast<unsigned>(after.extFree));
	TEST_EVENT("h10c cache %u/%u steal %u/%u", static_cast<unsigned>(cacheBefore), static_cast<unsigned>(cachePeak),
	           static_cast<unsigned>(stealBefore), static_cast<unsigned>(stealPeak));
	// Bytes by what took them, fast heap / the rest.
	const compose_shape::Result::Bytes& b = result.bytes;
	TEST_EVENT("h10b outputs %u/%u kit %u/%u synth %u/%u", static_cast<unsigned>(b.outputs[0]),
	           static_cast<unsigned>(b.outputs[1]), static_cast<unsigned>(b.kitClips[0]),
	           static_cast<unsigned>(b.kitClips[1]), static_cast<unsigned>(b.synthClips[0]),
	           static_cast<unsigned>(b.synthClips[1]));
	TEST_EVENT("h10d notes %u/%u nodes %u/%u arranger %u/%u", static_cast<unsigned>(b.notes[0]),
	           static_cast<unsigned>(b.notes[1]), static_cast<unsigned>(b.nodes[0]), static_cast<unsigned>(b.nodes[1]),
	           static_cast<unsigned>(b.arranger[0]), static_cast<unsigned>(b.arranger[1]));
}

void songSaveCommand() {
	const char* path = "SONGS/GENTEST_H12.XML";
	uint32_t t0 = now();
	Error error = StorageManager::createXMLFile(path, smSerializer, true, false);
	uint32_t t1 = now();
	if (error == Error::NONE) {
		currentSong->writeToFile();
		error = GetSerializer().closeFileAfterWriting(path, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<song\n",
		                                              "\n</song>\n");
	}
	uint32_t t2 = now();
	FILINFO info;
	uint32_t size = f_stat(path, &info) == FR_OK ? info.fsize : 0;
	TEST_EVENT("h12save err %d create %u write %u total %u bytes %u", static_cast<int>(error),
	           static_cast<unsigned>(toUs(t1 - t0)), static_cast<unsigned>(toUs(t2 - t1)),
	           static_cast<unsigned>(toUs(t2 - t0)), static_cast<unsigned>(size));
}

void songCountCommand() {
	int32_t session = currentSong->sessionClips.getNumElements();
	int32_t arrangement = currentSong->arrangementOnlyClips.getNumElements();
	int32_t instances = 0;
	int32_t notes = 0;
	for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
		instances += output->clipInstances.getNumElements();
	}
	for (Clip* clip : AllClips::everywhere(currentSong)) {
		if (clip->type != ClipType::INSTRUMENT) {
			continue;
		}
		auto* instrumentClip = static_cast<InstrumentClip*>(clip);
		for (int32_t r = 0; r < instrumentClip->noteRows.getNumElements(); r++) {
			notes += instrumentClip->noteRows.getElement(r)->notes.getNumElements();
		}
	}
	Memory m = takeMemory();
	TEST_EVENT("songcount session %d arrangement %d instances %d notes %d fast %u ext %u", static_cast<int>(session),
	           static_cast<int>(arrangement), static_cast<int>(instances), static_cast<int>(notes),
	           static_cast<unsigned>(m.fastFree), static_cast<unsigned>(m.extFree));
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

namespace {

// ---- #83: silent incoming Kits, retiring Outputs, tail checks ----

// The probe's linked Outputs: slot 0 the Drums kit (11 lanes), 1 the FX kit (2 sample lanes, 2 synth rows), 2 a Synth.
constexpr int32_t kNumSlots = 3;
struct Slot {
	Instrument* instrument;
	InstrumentClip* clip;
	Drum* drums[kNumRosterLanes];
	int32_t numDrums;
};
Slot slots[kNumSlots];
const Output* volatile slotOutputs[kNumSlots];

// The render window: every render, and each tracked Output's renderOutput().
volatile uint32_t rwRenders = 0;
volatile uint32_t rwSamples = 0;
volatile uint64_t rwTicks = 0;
volatile uint32_t rwMaxTicks = 0;
volatile uint64_t rwSlotTicks[kNumSlots];
volatile uint32_t rwSlotMaxTicks[kNumSlots];
volatile uint32_t rwSlotRenders[kNumSlots];
uint32_t slotStartTicks = 0;

void resetRenderWindow() {
	rwRenders = 0;
	rwSamples = 0;
	rwTicks = 0;
	rwMaxTicks = 0;
	for (int32_t i = 0; i < kNumSlots; i++) {
		rwSlotTicks[i] = 0;
		rwSlotMaxTicks[i] = 0;
		rwSlotRenders[i] = 0;
	}
	resetAudioWindow();
}

void reportRenderWindow() {
	uint32_t samples = rwSamples;
	samples = samples != 0 ? samples : 1;
	uint32_t usPer128 = static_cast<uint32_t>(toUs(static_cast<uint32_t>(rwTicks / 16)) * 16ull * 128 / samples);
	TEST_EVENT("rw renders %u samples %u per128 %u max %u late %u gap %u dir %d voices %u",
	           static_cast<unsigned>(rwRenders), static_cast<unsigned>(rwSamples), static_cast<unsigned>(usPer128),
	           static_cast<unsigned>(toUs(rwMaxTicks)), static_cast<unsigned>(windowLate),
	           static_cast<unsigned>(toUs(windowMaxGap)), static_cast<int>(windowMaxDireness),
	           static_cast<unsigned>(windowMaxVoices));
	for (int32_t i = 0; i < kNumSlots; i++) {
		uint32_t perRenderNs = rwSlotRenders[i] != 0
		                           ? static_cast<uint32_t>(static_cast<uint64_t>(rwSlotTicks[i]) * 1000000000ull
		                                                   / DELUGE_CLOCKS_PER / rwSlotRenders[i])
		                           : 0;
		TEST_EVENT("rws slot %d linked %d renders %u perrender_ns %u max %u", static_cast<int>(i),
		           slotOutputs[i] != nullptr ? 1 : 0, static_cast<unsigned>(rwSlotRenders[i]),
		           static_cast<unsigned>(perRenderNs), static_cast<unsigned>(toUs(rwSlotMaxTicks[i])));
	}
}

// ---- The silence checks of §12.9 ----

bool synthSilent(const SoundInstrument* synth) {
	return synth->skippingRendering;
}

bool kitSilent(Kit* kit) {
	return kit->drumsWithRenderingActive.getNumElements() == 0 && kit->delay.repeatsUntilAbandon == 0
	       && !kit->arpeggiator.hasAnyInputNotesActive() && !stutterer.isStuttering(kit);
}

// ---- The tail check ----

constexpr int32_t kTailBucketMs = 20;
constexpr int32_t kTailBuckets = 32000 / kTailBucketMs;
struct TailState {
	volatile bool on;
	int32_t slot;
	uint32_t startTicks;            ///< the note-on
	volatile int32_t firstSilentMs; ///< -1 until the check first reports silence
	volatile int32_t flipsBack;     ///< times it went back to "sounding" after reporting silence
	volatile bool silentNow;
	volatile int32_t lastRepeats; ///< the checked delay's repeatsUntilAbandon at the last render
	volatile int32_t maxRepeats;
	uint32_t peaks[kTailBuckets]; ///< the largest sample the Output added, per bucket
};
TailState tail;
StereoSample tailBefore[SSI_TX_BUFFER_NUM_SAMPLES];
SoundDrum* tailRowDrum = nullptr; ///< kind 2: the drum whose own delay is checked
int32_t tailKind = 0;

bool tailCheckSilent(int32_t* repeats) {
	Slot& s = slots[tail.slot];
	if (s.instrument == nullptr) {
		return true;
	}
	if (s.instrument->type == OutputType::SYNTH) {
		auto* synth = static_cast<SoundInstrument*>(s.instrument);
		*repeats = synth->delay.repeatsUntilAbandon;
		return synthSilent(synth);
	}
	auto* kit = static_cast<Kit*>(s.instrument);
	*repeats = tailKind == 2 && tailRowDrum != nullptr ? tailRowDrum->delay.repeatsUntilAbandon
	                                                   : kit->delay.repeatsUntilAbandon;
	return kitSilent(kit);
}

uint32_t msSince(uint32_t startTicks) {
	return toUs(now() - startTicks) / 1000;
}

// ---- Linking, retiring ----

ModelStackWithThreeMainThings* slotModelStack(char* memory, int32_t slot) {
	Slot& s = slots[slot];
	return setupModelStackWithSong(memory, currentSong)
	    ->addTimelineCounter(s.clip)
	    ->addOtherTwoThingsButNoNoteRow(s.instrument->toModControllable(), &s.clip->paramManager);
}

ModelStackWithThreeMainThings* rowModelStack(char* memory, int32_t slot, Drum* drum) {
	Slot& s = slots[slot];
	int32_t noteRowIndex = -1;
	NoteRow* noteRow = s.clip->getNoteRowForDrum(drum, &noteRowIndex);
	if (noteRow == nullptr) {
		return nullptr;
	}
	return setupModelStackWithSong(memory, currentSong)
	    ->addTimelineCounter(s.clip)
	    ->addNoteRow(noteRowIndex, noteRow)
	    ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
}

void hitSlot(int32_t slot, Drum* drum, bool on) {
	Slot& s = slots[slot];
	char memory[MODEL_STACK_MAX_SIZE];
	int16_t mpeValues[kNumExpressionDimensions] = {};
	if (s.instrument->type == OutputType::SYNTH) {
		ModelStackWithThreeMainThings* ms =
		    setupModelStackWithSong(memory, currentSong)
		        ->addTimelineCounter(s.clip)
		        ->addNoteRow(60, nullptr)
		        ->addOtherTwoThings(s.instrument->toModControllable(), &s.clip->paramManager);
		static_cast<MelodicInstrument*>(s.instrument)
		    ->sendNote(ms, on, 60, on ? mpeValues : nullptr, MIDI_CHANNEL_NONE, 110);
		return;
	}
	ModelStackWithThreeMainThings* ms = rowModelStack(memory, slot, drum);
	if (ms == nullptr) {
		return;
	}
	auto* kit = static_cast<Kit*>(s.instrument);
	if (on) {
		kit->noteOnPreKitArp(ms, drum, 110, mpeValues);
	}
	else {
		kit->noteOffPreKitArp(ms, drum);
	}
}

/// Sets delay feedback in menu units (0-50): the Synth's or a drum row's patched param, or the Kit's unpatched one.
void setFeedback(int32_t slot, Drum* row, int32_t menu) {
	int32_t value = computeFinalValueForStandardMenuItem(std::clamp<int32_t>(menu, 0, 50));
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* ms = nullptr;
	if (row != nullptr) {
		ModelStackWithThreeMainThings* m3 = rowModelStack(memory, slot, row);
		if (m3 != nullptr) {
			ms = m3->getPatchedAutoParamFromId(deluge::modulation::params::GLOBAL_DELAY_FEEDBACK);
		}
	}
	else if (slots[slot].instrument->type == OutputType::SYNTH) {
		ms = slotModelStack(memory, slot)->getPatchedAutoParamFromId(deluge::modulation::params::GLOBAL_DELAY_FEEDBACK);
	}
	else {
		ms = slotModelStack(memory, slot)
		         ->getUnpatchedAutoParamFromId(deluge::modulation::params::UNPATCHED_DELAY_AMOUNT);
	}
	if (ms != nullptr && ms->autoParam != nullptr) {
		ms->autoParam->setCurrentValueInResponseToUserInput(value, ms);
	}
}

void waitMs(int32_t ms) {
	if (ms > 0) {
		yieldWithTimeout([]() { return false; }, ms / 1000.0);
	}
}

/// Builds the two largest kits (and with `withSynth` the card's largest pad) unlinked, then links each with its
/// always-active 1-bar clip and NoteRows, as §12.9's end of Preloading does.
void silentLinkCommand(int32_t seed, int32_t withSynth) {
	if (slots[0].instrument != nullptr || slots[1].instrument != nullptr || slots[2].instrument != nullptr) {
		TEST_EVENT("tl busy");
		return;
	}
	rngState = seed != 0 ? static_cast<uint32_t>(seed) : 1;
	resetAudioWindow();
	Memory before = takeMemory();
	uint32_t t0 = now();
	Error error = Error::NONE;
	RollStats stats{};

	static char lanePaths[kNumRosterLanes][160];
	const char* paths[kNumRosterLanes];
	const char* names[kNumRosterLanes];
	int32_t lanes = 0;
	for (int32_t lane = 0; lane < kNumRosterLanes; lane++) {
		if (rollLane(lane, nullptr, lanePaths[lanes], sizeof(lanePaths[lanes]), stats)) {
			paths[lanes] = lanePaths[lanes];
			names[lanes] = kLaneFolders[lane].name;
			lanes++;
		}
	}
	Kit* drumKit = perform::buildSampleKit(currentSong, "T83 DRUMS", paths, names, lanes, slots[0].drums, &error);
	slots[0].numDrums = drumKit != nullptr ? lanes : 0;

	static char fxPaths[2][160];
	const char* fxP[2];
	const char* fxNames[2] = {"FX1", "FX2"};
	int32_t fxSamples = 0;
	for (int32_t i = 0; i < 2; i++) {
		if (rollLane(9, nullptr, fxPaths[i], sizeof(fxPaths[i]), stats)) {
			fxP[fxSamples++] = fxPaths[i];
		}
	}
	Error fxError = Error::NONE;
	Kit* fxKit = perform::buildSampleKit(currentSong, "T83 FX", fxP, fxNames, fxSamples, slots[1].drums, &fxError);
	slots[1].numDrums = fxKit != nullptr ? fxSamples : 0;
	for (int32_t i = 0; i < 2 && fxKit != nullptr; i++) {
		char path[160];
		char name[64];
		Error e = Error::NONE;
		if (pickPreset(nullptr, 7 + static_cast<int32_t>(nextRandom() % 40), path, sizeof(path), name, sizeof(name))) {
			SoundDrum* drum = loadSynthDrum(path, name, &e);
			if (drum != nullptr) {
				fxKit->addDrum(drum);
				slots[1].drums[slots[1].numDrums++] = drum;
			}
		}
		if (e != Error::NONE && fxError == Error::NONE) {
			fxError = e;
		}
	}

	Instrument* synth = nullptr;
	Error synthError = Error::NONE;
	if (withSynth != 0) {
		char path[160];
		char name[64];
		if (pickPreset("pad", 0, path, sizeof(path), name, sizeof(name))) {
			synth = loadSynthPreset(path, name, &synthError);
		}
	}
	yieldWithTimeout(clustersLoaded, 10.0);
	uint32_t tBuilt = now();
	uint32_t buildLate = windowLate;
	uint32_t buildGap = windowMaxGap;
	Memory built = takeMemory();

	// Link: each one's clip, NoteRows and Output, with no yield inside (perform::linkWithClip, the POC's link).
	resetAudioWindow();
	uint32_t maxLinkTicks = 0;
	Instrument* toLink[kNumSlots] = {drumKit, fxKit, synth};
	Error linkError = Error::NONE;
	for (int32_t i = 0; i < kNumSlots; i++) {
		if (toLink[i] == nullptr) {
			continue;
		}
		uint32_t l0 = now();
		Error e = Error::NONE;
		InstrumentClip* clip = perform::linkWithClip(currentSong, toLink[i], &e);
		maxLinkTicks = std::max(maxLinkTicks, now() - l0);
		if (clip == nullptr) {
			linkError = e;
			continue;
		}
		slots[i].instrument = toLink[i];
		slots[i].clip = clip;
		slotOutputs[i] = toLink[i];
		yieldWithTimeout([]() { return false; }, 0.002);
	}
	uint32_t tLinked = now();
	Memory linked = takeMemory();
	TEST_EVENT("tl seed %d lanes %d fx %d synth %d err %d/%d/%d/%d", static_cast<int>(seed),
	           static_cast<int>(slots[0].numDrums), static_cast<int>(slots[1].numDrums), slots[2].instrument ? 1 : 0,
	           static_cast<int>(error), static_cast<int>(fxError), static_cast<int>(synthError),
	           static_cast<int>(linkError));
	TEST_EVENT("tlt build %u buildlate %u buildgap %u link %u maxlink %u linklate %u linkgap %u",
	           static_cast<unsigned>(toUs(tBuilt - t0)), static_cast<unsigned>(buildLate),
	           static_cast<unsigned>(toUs(buildGap)), static_cast<unsigned>(toUs(tLinked - tBuilt)),
	           static_cast<unsigned>(toUs(maxLinkTicks)), static_cast<unsigned>(windowLate),
	           static_cast<unsigned>(toUs(windowMaxGap)));
	TEST_EVENT("tlm before %u built %u linked %u largest %u ext %u/%u", static_cast<unsigned>(before.fastFree),
	           static_cast<unsigned>(built.fastFree), static_cast<unsigned>(linked.fastFree),
	           static_cast<unsigned>(linked.fastLargest), static_cast<unsigned>(before.extFree),
	           static_cast<unsigned>(linked.extFree));
}

/// Plays each slot in `mask` once (every drum, or the synth's note), for `ms`, then note-offs: so the Outputs to retire
/// have had voices, as outgoing ones would.
void slotHitCommand(int32_t mask, int32_t ms) {
	for (int32_t i = 0; i < kNumSlots; i++) {
		if ((mask & (1 << i)) == 0 || slots[i].instrument == nullptr) {
			continue;
		}
		if (slots[i].instrument->type == OutputType::SYNTH) {
			hitSlot(i, nullptr, true);
		}
		else {
			for (int32_t d = 0; d < slots[i].numDrums; d++) {
				hitSlot(i, slots[i].drums[d], true);
			}
		}
	}
	waitMs(ms);
	for (int32_t i = 0; i < kNumSlots; i++) {
		if ((mask & (1 << i)) == 0 || slots[i].instrument == nullptr) {
			continue;
		}
		if (slots[i].instrument->type == OutputType::SYNTH) {
			hitSlot(i, nullptr, false);
		}
		else {
			for (int32_t d = 0; d < slots[i].numDrums; d++) {
				hitSlot(i, slots[i].drums[d], false);
			}
		}
	}
	TEST_EVENT("thit mask %d", static_cast<int>(mask));
}

// Retiring: counted from the audio routine while a free runs.
const Output* volatile retiring = nullptr;
volatile uint32_t retireEntries = 0;
volatile uint32_t retireListedEntries = 0;

void retireRoutineEntered() {
	retireEntries = retireEntries + 1;
	for (const Output* o = currentSong != nullptr ? currentSong->firstOutput : nullptr; o != nullptr; o = o->next) {
		if (o == retiring) {
			retireListedEntries = retireListedEntries + 1;
			break;
		}
	}
}

/// Frees each slot in `mask` (order: synth, FX kit, Drums kit) the way stock SessionView::removeClip does, with
/// InstrumentRemoval::DELETE: stop its notes, drop MIDI follow's pointer, take the clip out of the session list,
/// then delete the clip and its Output. Times each one and its longest stretch without an audio routine entry.
void retireCommand(int32_t mask) {
	for (int32_t i : {2, 1, 0}) {
		if ((mask & (1 << i)) == 0 || slots[i].instrument == nullptr) {
			continue;
		}
		Slot& s = slots[i];
		int32_t drums = s.instrument->type == OutputType::KIT ? s.numDrums : 0;
		bool silent = s.instrument->type == OutputType::SYNTH ? synthSilent(static_cast<SoundInstrument*>(s.instrument))
		                                                      : kitSilent(static_cast<Kit*>(s.instrument));
		Memory before = takeMemory();
		resetAudioWindow();
		retireEntries = 0;
		retireListedEntries = 0;
		slotOutputs[i] = nullptr;
		uint32_t t0 = now();
		retiring = s.instrument;

		char memory[MODEL_STACK_MAX_SIZE];
		s.clip->stopAllNotesPlaying(setupModelStackWithSong(memory, currentSong)->addTimelineCounter(s.clip));
		midiFollow.removeClip(s.clip);
		if (currentSong->getCurrentClip() == s.clip) {
			currentSong->setCurrentClip(nullptr);
		}
		int32_t index = currentSong->sessionClips.getIndexForClip(s.clip);
		currentSong->removeSessionClipLowLevel(s.clip, index);
		uint32_t tUnlisted = now();
		currentSong->deleteClipObject(s.clip, false, InstrumentRemoval::DELETE);
		uint32_t t1 = now();
		retiring = nullptr;
		uint32_t late = windowLate;
		uint32_t gap = windowMaxGap;
		Memory after = takeMemory();
		TEST_EVENT("tr slot %d drums %d silent %d us %u clipout %u yields %u listed %u late %u gap %u freed %d ext %d",
		           static_cast<int>(i), static_cast<int>(drums), silent ? 1 : 0, static_cast<unsigned>(toUs(t1 - t0)),
		           static_cast<unsigned>(toUs(tUnlisted - t0)), static_cast<unsigned>(retireEntries),
		           static_cast<unsigned>(retireListedEntries), static_cast<unsigned>(late),
		           static_cast<unsigned>(toUs(gap)), static_cast<int>(after.fastFree - before.fastFree),
		           static_cast<int>(after.extFree - before.extFree));
		s = Slot{};
		yieldWithTimeout([]() { return false; }, 0.05);
	}
}

/// The tail check. kind 0: the Synth (slot 2), its own delay. kind 1: the Drums kit (slot 0), the kit's delay, hit on
/// its Snare row. kind 2: the same row, with the row's own delay. Feedback starts at 0 (home), spikes to `fbMenu` just
/// before the hit (the Throw), holds `holdMs` from the hit, then falls to 0 over 1 beat at the song tempo. holdMs < 0
/// never falls. Watches until 1 s after the check first reports silence, or 30 s.
void tailCommand(int32_t kind, int32_t fbMenu, int32_t holdMs) {
	int32_t slot = kind == 0 ? 2 : 0;
	if (slots[slot].instrument == nullptr || (kind != 0 && slots[slot].numDrums < 2)) {
		TEST_EVENT("tt none");
		return;
	}
	Drum* row = kind == 0 ? nullptr : slots[slot].drums[1]; // the Snare lane
	tailKind = kind;
	tailRowDrum = kind == 2 ? static_cast<SoundDrum*>(row) : nullptr;
	// Home: no delay on the kit or the row.
	if (kind == 0) {
		setFeedback(slot, nullptr, 0);
	}
	else {
		setFeedback(slot, nullptr, 0);
		setFeedback(slot, row, 0);
	}
	// Wait for anything from an earlier run to die away.
	tail.slot = slot;
	int32_t repeats = 0;
	for (int32_t i = 0; i < 300 && !tailCheckSilent(&repeats); i++) {
		waitMs(20);
	}
	bool silentBefore = tailCheckSilent(&repeats);

	std::fill(std::begin(tail.peaks), std::end(tail.peaks), 0u);
	tail.firstSilentMs = -1;
	tail.flipsBack = 0;
	tail.silentNow = false;
	tail.maxRepeats = 0;
	tail.lastRepeats = 0;
	setFeedback(slot, kind == 2 ? row : nullptr, fbMenu);
	tail.startTicks = now();
	tail.on = true;
	hitSlot(slot, row, true);
	int32_t beatMs = static_cast<int32_t>(60000.0f / playbackHandler.calculateBPMForDisplay());
	waitMs(beatMs / 4);
	hitSlot(slot, row, false);
	int32_t fallStartMs = -1;
	int32_t fallEndMs = -1;
	if (holdMs >= 0) {
		waitMs(holdMs - static_cast<int32_t>(msSince(tail.startTicks)));
		fallStartMs = static_cast<int32_t>(msSince(tail.startTicks));
		constexpr int32_t kSteps = 16;
		for (int32_t step = 1; step <= kSteps; step++) {
			setFeedback(slot, kind == 2 ? row : nullptr, fbMenu - fbMenu * step / kSteps);
			waitMs(beatMs / kSteps);
		}
		fallEndMs = static_cast<int32_t>(msSince(tail.startTicks));
	}
	while (msSince(tail.startTicks) < 30000
	       && (tail.firstSilentMs < 0 || msSince(tail.startTicks) < static_cast<uint32_t>(tail.firstSilentMs) + 1000)) {
		waitMs(20);
	}
	tail.on = false;
	int32_t endMs = static_cast<int32_t>(msSince(tail.startTicks));
	setFeedback(slot, kind == 2 ? row : nullptr, 0);

	// Summary: the loudest bucket, and the last bucket above each level below it.
	uint32_t peak = 0;
	int32_t peakBucket = 0;
	int32_t buckets = std::min<int32_t>(endMs / kTailBucketMs + 1, kTailBuckets);
	for (int32_t b = 0; b < buckets; b++) {
		if (tail.peaks[b] > peak) {
			peak = tail.peaks[b];
			peakBucket = b;
		}
	}
	constexpr int32_t kLevelsDb[] = {20, 40, 60, 80};
	int32_t lastAbove[4];
	for (int32_t l = 0; l < 4; l++) {
		double factor = std::pow(10.0, -kLevelsDb[l] / 20.0);
		uint32_t threshold = static_cast<uint32_t>(peak * factor);
		lastAbove[l] = -1;
		for (int32_t b = buckets - 1; b >= 0; b--) {
			if (tail.peaks[b] > threshold) {
				lastAbove[l] = (b + 1) * kTailBucketMs;
				break;
			}
		}
	}
	double peakDbfs = peak > 0 ? 20.0 * std::log10(static_cast<double>(peak) / 2147483648.0) : -999.0;
	// Test events are cut at 120 characters, so the summary takes two.
	TEST_EVENT("tt kind %d fb %d hold %d before %d silent %d flips %d end %d fall %d fallend %d",
	           static_cast<int>(kind), static_cast<int>(fbMenu), static_cast<int>(holdMs), silentBefore ? 1 : 0,
	           static_cast<int>(tail.firstSilentMs), static_cast<int>(tail.flipsBack), static_cast<int>(endMs),
	           static_cast<int>(fallStartMs), static_cast<int>(fallEndMs));
	TEST_EVENT("tt2 peak_dbfs %d peakat %d m20 %d m40 %d m60 %d m80 %d maxrep %d lastrep %d",
	           static_cast<int>(std::lround(peakDbfs)), static_cast<int>(peakBucket * kTailBucketMs),
	           static_cast<int>(lastAbove[0]), static_cast<int>(lastAbove[1]), static_cast<int>(lastAbove[2]),
	           static_cast<int>(lastAbove[3]), static_cast<int>(tail.maxRepeats), static_cast<int>(tail.lastRepeats));
	// The envelope, 100 ms per value in dB below the peak (999 = nothing), 20 values a line.
	for (int32_t line = 0; line * 2000 < endMs; line++) {
		char text[200];
		int32_t len = 0;
		for (int32_t v = 0; v < 20; v++) {
			uint32_t m = 0;
			for (int32_t b = 0; b < 100 / kTailBucketMs; b++) {
				int32_t index = (line * 20 + v) * (100 / kTailBucketMs) + b;
				if (index < buckets) {
					m = std::max(m, tail.peaks[index]);
				}
			}
			int32_t db = m > 0 && peak > 0
			                 ? static_cast<int32_t>(std::lround(-20.0 * std::log10(static_cast<double>(m) / peak)))
			                 : 999;
			len += snprintf(text + len, sizeof(text) - len, "%d,", static_cast<int>(db));
		}
		TEST_EVENT("tte %d %s", static_cast<int>(line * 2000), text);
	}
}

// The Build roll stand-in: Clap-lane hits every rollEvery Generator ticks (6 = four a 16th), velocity climbing over
// each bar.
int32_t rollEvery = 0;
int64_t nextRollTick = 0;

} // namespace

int64_t afterTick(int64_t generatorNow, int32_t stepsDecided, perform::PerformDriver& driver) {
	int64_t wake = 0;
	auto wantWake = [&](int64_t tick) {
		if (tick > generatorNow && (wake == 0 || tick < wake)) {
			wake = tick;
		}
	};

	// H1
	if (stepsDecided > 0) {
		decisionTicks++;
		if (busyUs > 0) {
			spinUs(busyUs);
			busyLoops++;
		}
	}

	// H3
	if (burstSize > 0) {
		if (burstOffTick >= 0 && generatorNow >= burstOffTick) {
			driver.testBurstOff();
			burstOffTick = -1;
		}
		if (nextBurstTick == 0) {
			nextBurstTick = (generatorNow / kTicksPerBar + 1) * kTicksPerBar + 6; // just after a downbeat
		}
		if (generatorNow >= nextBurstTick) {
			if (numBursts < kMaxBursts) {
				currentBurst = &bursts[numBursts++];
				*currentBurst = Burst{};
				currentBurst->direness = static_cast<int8_t>(AudioEngine::cpuDireness);
				currentBurst->voicesBefore = static_cast<uint8_t>(std::min<uint32_t>(AudioEngine::getNumVoices(), 255));
				std::fill(std::begin(currentBurst->waited), std::end(currentBurst->waited), -1);
				std::fill(std::begin(currentBurst->order), std::end(currentBurst->order), -1);
				burstRender = renderCount;
				burstStarted = 0;
				// Sounds go in burstSounds before the note-ons, so voiceStarted() can find them as they start.
				currentBurst->notes = 0;
				std::fill(std::begin(burstSounds), std::end(burstSounds), nullptr);
				int32_t sent = driver.testBurst(burstSize, burstSounds);
				currentBurst->notes = static_cast<uint8_t>(sent);
			}
			burstOffTick = generatorNow + 12;
			nextBurstTick = (generatorNow / kTicksPerBar + 1) * kTicksPerBar + 6;
		}
		wantWake(nextBurstTick);
		if (burstOffTick >= 0) {
			wantWake(burstOffTick);
		}
	}

	// #83: the Build roll stand-in
	if (rollEvery > 0) {
		if (nextRollTick == 0 || nextRollTick > generatorNow + kTicksPerBar) {
			nextRollTick = (generatorNow / rollEvery + 1) * rollEvery;
		}
		if (generatorNow >= nextRollTick) {
			int64_t inBar = generatorNow % kTicksPerBar;
			driver.testRollHit(static_cast<uint8_t>(60 + 67 * inBar / kTicksPerBar));
			nextRollTick = (generatorNow / rollEvery + 1) * rollEvery;
		}
		wantWake(nextRollTick);
	}

	// H4
	if (glideStart == 0 && glideLength > 0) {
		glideStart = (generatorNow / kTicksPerBar + 1) * kTicksPerBar; // the next bar line
		glideLogUntil = glideStart + glideLength + kTicksPerBar;
		glideBpmX100 = glideFromX10 * 10;
		setTempo(glideFromX10 / 10.0);
	}
	if (glideStart > 0) {
		if (generatorNow >= glideStart && generatorNow <= glideStart + glideLength) {
			double f = static_cast<double>(generatorNow - glideStart) / static_cast<double>(glideLength);
			double bpm = (glideFromX10 + (glideToX10 - glideFromX10) * f) / 10.0;
			setTempo(bpm);
			glideBpmX100 = static_cast<int32_t>(bpm * 100.0);
		}
		if (generatorNow >= glideStart - 24 && generatorNow <= glideLogUntil && generatorNow % 24 == 0
		    && numGlideSteps < kMaxGlideSteps
		    && (numGlideSteps == 0 || glideSteps[numGlideSteps - 1].step != generatorNow / 24)) {
			glideSteps[numGlideSteps++] =
			    GlideStep{static_cast<int32_t>(generatorNow / 24), AudioEngine::audioSampleTimer,
			              static_cast<uint32_t>(playbackHandler.timeLastTimerTickBig >> 32), glideBpmX100};
		}
		if (generatorNow < glideLogUntil) {
			wantWake(generatorNow + 1); // a tick every tick while the glide runs, and through the bar after it
		}
		else {
			glideStart = -1;
			glideLength = 0;
		}
	}
	return wake;
}

void voiceStarted(const Sound* sound) {
	Burst* b = currentBurst;
	if (b == nullptr || burstSize == 0) {
		return;
	}
	int32_t limit = b->notes != 0 ? b->notes : burstSize;
	for (int32_t i = 0; i < limit; i++) {
		if (b->waited[i] < 0 && burstSounds[i] == sound) {
			b->waited[i] = static_cast<int8_t>(std::min<uint32_t>(renderCount - burstRender, 127));
			b->order[i] = static_cast<int8_t>(burstStarted++);
			return;
		}
	}
}

void voicesCulled(int32_t count) {
	windowCulled = windowCulled + count;
}

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

bool runsOnArrival(int32_t id) {
	return id == kComposeCancel;
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
		TEST_EVENT("clockcfg was magmatch %d clockin %d clockout %d",
		           playbackHandler.tempoMagnitudeMatchingEnabled ? 1 : 0, playbackHandler.midiInClockEnabled ? 1 : 0,
		           playbackHandler.midiOutClockEnabled ? 1 : 0);
		if (c >= 0) {
			playbackHandler.setMidiOutClockMode(c != 0);
		}
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

	case kBusy:
		busyUs = std::clamp<int32_t>(a, 0, 20000);
		decisionTicks = 0;
		busyLoops = 0;
		resetAudioWindow();
		TEST_EVENT("probe busy %d", static_cast<int>(busyUs));
		return true;

	case kH1Report:
		TEST_EVENT("h1 busy %d decisions %u loops %u late %u maxgap %u direness %d culled %u voices %u",
		           static_cast<int>(busyUs), static_cast<unsigned>(decisionTicks), static_cast<unsigned>(busyLoops),
		           static_cast<unsigned>(windowLate), static_cast<unsigned>(toUs(windowMaxGap)),
		           static_cast<int>(windowMaxDireness), static_cast<unsigned>(windowCulled),
		           static_cast<unsigned>(windowMaxVoices));
		decisionTicks = 0;
		busyLoops = 0;
		resetAudioWindow();
		return true;

	case kBurst:
		burstSize = std::clamp<int32_t>(a, 0, kMaxBurstNotes);
		if (burstSize > 0) {
			numBursts = 0; // a new run; "burst 0" stops and keeps the results for h3_dump
		}
		nextBurstTick = 0;
		burstOffTick = -1;
		TEST_EVENT("probe burst %d", static_cast<int>(burstSize));
		return true;

	case kH3Dump:
		TEST_EVENT("h3n %d", static_cast<int>(numBursts));
		for (int32_t i = a; i < std::min<int32_t>(a + std::min<int32_t>(b, 12), numBursts); i++) {
			const Burst& r = bursts[i];
			char waited[64];
			char order[64];
			int32_t wl = 0;
			int32_t ol = 0;
			for (int32_t n = 0; n < r.notes; n++) {
				wl += snprintf(waited + wl, sizeof(waited) - wl, "%d,", r.waited[n]);
				ol += snprintf(order + ol, sizeof(order) - ol, "%d,", r.order[n]);
			}
			TEST_EVENT("h3b %d dir %d vb %d w %s o %s", static_cast<int>(i), r.direness, r.voicesBefore, waited, order);
		}
		return true;

	case kGlide:
		glideFromX10 = a;
		glideToX10 = b;
		glideLength = static_cast<int64_t>(c) * kTicksPerBar; // c bars
		glideStart = 0;                                       // armed: starts at the next bar line
		numGlideSteps = 0;
		TEST_EVENT("probe glide %d %d bars %d", static_cast<int>(a), static_cast<int>(b), static_cast<int>(c));
		return true;

	case kGlideDump:
		TEST_EVENT("h4n %d", static_cast<int>(numGlideSteps));
		for (int32_t i = a; i < std::min<int32_t>(a + std::min<int32_t>(b, 12), numGlideSteps); i++) {
			const GlideStep& g = glideSteps[i];
			TEST_EVENT("h4s %d step %d sample %u tick %u bpm %d", static_cast<int>(i), static_cast<int>(g.step),
			           static_cast<unsigned>(g.sample), static_cast<unsigned>(g.tickSample),
			           static_cast<int>(g.bpmX100));
		}
		return true;

	case kBrowserName: {
		FileItem* item = getCurrentUI() != nullptr ? Browser::getCurrentFileItem() : nullptr;
		TEST_EVENT("browser %s|", item != nullptr && item->displayName != nullptr ? item->displayName : "(none)");
		return true;
	}

	case kRenderBurn:
		renderBurnUs = std::clamp<int32_t>(a, 0, 2000);
		TEST_EVENT("probe renderburn %d", static_cast<int>(renderBurnUs));
		return true;

	case kLaneRoll:
		if (inCardRoutine) {
			return false;
		}
		laneRollCommand(a, b, c);
		return true;

	case kRoster:
		if (inCardRoutine) {
			return false;
		}
		rosterCommand(a);
		return true;

	case kCompose:
		if (inCardRoutine) {
			return false;
		}
		composeCommand(a, b, c, false);
		return true;

	case kComposeCancel:
		compose_shape::cancel();
		return true;

	case kSongSave:
		if (inCardRoutine) {
			return false;
		}
		if (a > 0) {
			// Build the Capture shape into the current song first (stopped): a occurrences over b bars.
			composeCommand(a, b, 0, true);
		}
		songSaveCommand();
		return true;

	case kSongCount:
		songCountCommand();
		return true;

	case kGentestDelete: {
		// Deletes the test files the probes write, and only those.
		static const char* const kPaths[] = {"SONGS/GENTEST_H12.XML"};
		for (const char* path : kPaths) {
			FRESULT result = f_unlink(path);
			TEST_EVENT("gentestdelete %s %d", path, static_cast<int>(result));
		}
		return true;
	}

	case kAudioClean: {
		// What cardReinserted() does to AudioFiles nothing uses: delete them. Reports before and after.
		reportRegions("aclean0");
		int32_t deleted = 0;
		for (int32_t e = 0; e < audioFileManager.audioFiles.getNumElements(); e++) {
			AudioFile* audioFile = static_cast<AudioFile*>(audioFileManager.audioFiles.getElement(e));
			if (audioFile->numReasonsToBeLoaded == 0) {
				audioFileManager.deleteUnusedAudioFileFromMemory(*audioFile, e);
				e--;
				deleted++;
			}
		}
		TEST_EVENT("aclean deleted %d", static_cast<int>(deleted));
		reportRegions("aclean1");
		return true;
	}

	case kTLink:
		if (inCardRoutine) {
			return false;
		}
		silentLinkCommand(a, b);
		return true;

	case kRenderWindow:
		if (a == 0) {
			resetRenderWindow();
		}
		else {
			reportRenderWindow();
		}
		return true;

	case kTHit:
		slotHitCommand(a, b);
		return true;

	case kTRetire:
		if (inCardRoutine) {
			return false;
		}
		retireCommand(a);
		return true;

	case kTTail:
		if (inCardRoutine) {
			return false;
		}
		tailCommand(a, b, c);
		return true;

	case kRoll:
		rollEvery = a;
		nextRollTick = 0;
		TEST_EVENT("roll every %d", static_cast<int>(a));
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
	uint32_t t = now();
	if (lastRenderTicks != 0) {
		uint32_t gap = t - lastRenderTicks;
		if (gap > windowMaxGap) {
			windowMaxGap = gap;
		}
		if (gap > kLateGapTicks) {
			windowLate = windowLate + 1;
		}
	}
	lastRenderTicks = t;
	renderCount = renderCount + 1;
	if (retiring != nullptr) {
		retireRoutineEntered();
	}
	if (AudioEngine::cpuDireness > windowMaxDireness) {
		windowMaxDireness = AudioEngine::cpuDireness;
	}
	uint32_t voices = AudioEngine::getNumVoices();
	if (voices > windowMaxVoices) {
		windowMaxVoices = voices;
	}
	if (renderBurnUs > 0) {
		spinUs(renderBurnUs);
	}
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

uint32_t renderBegin() {
	return now();
}

void renderEnd(uint32_t startTicks, size_t numSamples) {
	uint32_t ticks = now() - startTicks;
	rwRenders = rwRenders + 1;
	rwSamples = rwSamples + numSamples;
	rwTicks = rwTicks + ticks;
	if (ticks > rwMaxTicks) {
		rwMaxTicks = ticks;
	}
	if (tail.on) {
		int32_t repeats = 0;
		bool silent = tailCheckSilent(&repeats);
		tail.lastRepeats = repeats;
		if (repeats > tail.maxRepeats) {
			tail.maxRepeats = repeats;
		}
		if (silent && tail.firstSilentMs < 0) {
			tail.firstSilentMs = static_cast<int32_t>(msSince(tail.startTicks));
		}
		if (!silent && tail.silentNow) {
			tail.flipsBack = tail.flipsBack + 1;
		}
		tail.silentNow = silent;
	}
}

int32_t outputRenderBegin(const Output* output, std::span<StereoSample> buffer) {
	for (int32_t i = 0; i < kNumSlots; i++) {
		if (output != nullptr && slotOutputs[i] == output) {
			if (tail.on && i == tail.slot) {
				std::copy(buffer.begin(), buffer.end(), tailBefore);
			}
			slotStartTicks = now();
			return i;
		}
	}
	return -1;
}

void outputRenderEnd(int32_t slot, std::span<StereoSample> buffer) {
	if (slot < 0) {
		return;
	}
	uint32_t ticks = now() - slotStartTicks;
	rwSlotTicks[slot] = rwSlotTicks[slot] + ticks;
	rwSlotRenders[slot] = rwSlotRenders[slot] + 1;
	if (ticks > rwSlotMaxTicks[slot]) {
		rwSlotMaxTicks[slot] = ticks;
	}
	if (tail.on && slot == tail.slot) {
		uint32_t peak = 0;
		for (size_t n = 0; n < buffer.size(); n++) {
			int64_t l = static_cast<int64_t>(buffer[n].l) - tailBefore[n].l;
			int64_t r = static_cast<int64_t>(buffer[n].r) - tailBefore[n].r;
			peak = std::max<uint32_t>(
			    peak, static_cast<uint32_t>(std::min<int64_t>(std::max(std::abs(l), std::abs(r)), UINT32_MAX)));
		}
		uint32_t bucket = msSince(tail.startTicks) / kTailBucketMs;
		if (bucket < static_cast<uint32_t>(kTailBuckets) && peak > tail.peaks[bucket]) {
			tail.peaks[bucket] = peak;
		}
	}
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
