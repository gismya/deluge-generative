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
#include "gui/ui/browser/browser.h"
#include "gui/ui/ui.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/instrument.h"
#include "model/instrument/kit.h"
#include "model/note/note_row.h"
#include "model/song/clip_iterators.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/file_item.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <cctype>
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
}

void songSaveCommand() {
	const char* path = "SONGS/GENTEST_H12.XML";
	uint32_t t0 = now();
	Error error = StorageManager::createXMLFile(path, smSerializer, false, false);
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
