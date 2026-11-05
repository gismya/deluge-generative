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

#include "generative/debug/poc_probe.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "OSLikeStuff/timers_interrupts/clock_type.h"
#include "definitions.h"
#include "generative/debug/hw_probe.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "memory/memory_region.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <limits>

namespace deluge::generative::probe {

namespace {

constexpr int64_t kTicksPerStep = 24;
constexpr int32_t kStepsPerBar = 16;
constexpr uint8_t kDrums = 0;
constexpr uint8_t kBass = 1;
constexpr uint8_t kKick = 0;
constexpr uint8_t kClosedHat = 2;
constexpr uint8_t kOpenHat = 3;

// ---- Audio gaps ----

/// A render later than this after the previous one has run the 128-sample output buffer dry.
constexpr uint32_t kLateGapTicks =
    static_cast<uint32_t>(static_cast<uint64_t>(SSI_TX_BUFFER_NUM_SAMPLES) * DELUGE_CLOCKS_PER / kSampleRate);

volatile uint32_t lastAudioTicks = 0;
volatile uint32_t maxGapTicks = 0;
volatile uint32_t lateRenders = 0;
volatile bool watchingAudio = false; ///< On from the first Generator start, so boot's long gaps aren't counted.

uint32_t toUs(uint32_t ticks) {
	return static_cast<uint32_t>(static_cast<uint64_t>(ticks) * 1000000u / DELUGE_CLOCKS_PER);
}

// ---- Voice starts ----

uint32_t refused = 0;        ///< allowedToStartVoice() refusals, all sounds
uint32_t pendingStarted = 0; ///< pending notes started by a later render, all sounds
uint32_t refusedBeforeNoteOn = 0;

// ---- One bar of Generator output ----

struct Bar {
	int32_t index = -1;
	uint32_t downbeatSample = 0;
	int32_t lastGridStep = -1;
	uint32_t lastGridSample = 0;
	uint32_t longSum = 0; ///< 16th intervals that start on an even step (the long ones under 16th swing)
	uint32_t shortSum = 0;
	uint32_t longCount = 0;
	uint32_t shortCount = 0;
	uint32_t minInterval = std::numeric_limits<uint32_t>::max();
	uint32_t maxInterval = 0;
	uint32_t grids = 0; ///< grid lines the hook ran on
	uint32_t notes = 0;
	uint32_t deferred = 0; ///< note-ons whose voice start the per-render limit pushed to a later render
	uint32_t kickDeferred = 0;
	uint32_t bassDeferred = 0;
	uint32_t hatsTogether = 0;        ///< open hat note-ons on the same tick as a closed hat
	uint32_t openRingingAtClosed = 0; ///< closed hat note-ons while the open hat still had a voice
	uint32_t maxVoices = 0;
	uint32_t refusedAtStart = 0;
	uint32_t pendingAtStart = 0;
};

Bar bar;
uint32_t previousDownbeatSample = 0;
bool havePreviousDownbeat = false;
int32_t held = 0; ///< Generator note-ons not yet matched by a note-off
int32_t lastStep = -1;
uint32_t skippedSteps = 0;
uint32_t backwardSteps = 0;
int32_t lastClosedHatTick = std::numeric_limits<int32_t>::min();

void flushBar() {
	if (bar.index < 0) {
		return;
	}
	uint32_t longAverage = bar.longCount ? bar.longSum / bar.longCount : 0;
	uint32_t shortAverage = bar.shortCount ? bar.shortSum / bar.shortCount : 0;
	uint32_t minInterval = bar.grids > 1 ? bar.minInterval : 0;
	TEST_EVENT("bar %d long %u short %u lo %u hi %u grid %u", static_cast<int>(bar.index),
	           static_cast<unsigned>(longAverage), static_cast<unsigned>(shortAverage),
	           static_cast<unsigned>(minInterval), static_cast<unsigned>(bar.maxInterval),
	           static_cast<unsigned>(bar.grids));
	uint32_t gap = maxGapTicks;
	uint32_t late = lateRenders;
	maxGapTicks = 0;
	lateRenders = 0;
	TEST_EVENT("barv %d n %u def %u dk %u db %u hat2 %u ohr %u vmax %u ref %u late %u skip %u back %u gap %u lr %u",
	           static_cast<int>(bar.index), static_cast<unsigned>(bar.notes), static_cast<unsigned>(bar.deferred),
	           static_cast<unsigned>(bar.kickDeferred), static_cast<unsigned>(bar.bassDeferred),
	           static_cast<unsigned>(bar.hatsTogether), static_cast<unsigned>(bar.openRingingAtClosed),
	           static_cast<unsigned>(bar.maxVoices), static_cast<unsigned>(refused - bar.refusedAtStart),
	           static_cast<unsigned>(pendingStarted - bar.pendingAtStart), static_cast<unsigned>(skippedSteps),
	           static_cast<unsigned>(backwardSteps), static_cast<unsigned>(toUs(gap)), static_cast<unsigned>(late));
}

void startBar(int32_t index, uint32_t sample) {
	if (havePreviousDownbeat) {
		TEST_EVENT("downbeat %d sample %u len %u", static_cast<int>(index), static_cast<unsigned>(sample),
		           static_cast<unsigned>(sample - previousDownbeatSample));
	}
	else {
		TEST_EVENT("downbeat %d sample %u", static_cast<int>(index), static_cast<unsigned>(sample));
	}
	previousDownbeatSample = sample;
	havePreviousDownbeat = true;
	bar = Bar{};
	bar.index = index;
	bar.downbeatSample = sample;
	bar.refusedAtStart = refused;
	bar.pendingAtStart = pendingStarted;
}

// ---- Memory ----

uint32_t freeIn(int32_t region) {
	MemoryRegion& r = GeneralMemoryAllocator::get().regions[region];
	uint32_t free = 0;
	for (int32_t j = 0; j < r.emptySpaces.getNumElements(); j++) {
		free += static_cast<EmptySpaceRecord*>(r.emptySpaces.getElementAddress(j))->length;
	}
	return free;
}

double nextMemoryAt = 0;
constexpr double kMemoryInterval = 15.0;

} // namespace

void audioRoutineEntered() {
	uint32_t now = getSystemTicks();
	if (watchingAudio) {
		uint32_t gap = now - lastAudioTicks;
		if (gap > maxGapTicks) {
			maxGapTicks = gap;
		}
		if (gap > kLateGapTicks) {
			lateRenders = lateRenders + 1;
		}
	}
	lastAudioTicks = now;
	hwprobe::audioRoutineEntered();
}

void voiceStartRefused() {
	refused++;
}

void pendingVoiceStarted() {
	pendingStarted++;
}

void generatorStarted(int64_t swungPos, int64_t originTick, int64_t barLength) {
	TEST_EVENT("gen_start pos %d origin %d barlen %d", static_cast<int>(swungPos), static_cast<int>(originTick),
	           static_cast<int>(barLength));
	hwprobe::generatorStarted(originTick);
	bar = Bar{};
	havePreviousDownbeat = false;
	lastStep = -1;
	lastClosedHatTick = std::numeric_limits<int32_t>::min();
	if (!watchingAudio) {
		lastAudioTicks = getSystemTicks();
		maxGapTicks = 0;
		lateRenders = 0;
		watchingAudio = true;
	}
}

void generatorStopped() {
	flushBar();
	bar = Bar{};
	hwprobe::generatorStopped();
	TEST_EVENT("gen_stop held %d skip %u back %u", static_cast<int>(held), static_cast<unsigned>(skippedSteps),
	           static_cast<unsigned>(backwardSteps));
	held = 0;
}

void noteOnBefore() {
	refusedBeforeNoteOn = refused;
}

void noteOnAfter(int32_t tick, uint8_t part, uint8_t lane, bool openHatRinging) {
	held++;
	bar.notes++;
	if (refused != refusedBeforeNoteOn) {
		bar.deferred++;
		if (part == kDrums && lane == kKick) {
			bar.kickDeferred++;
		}
		if (part == kBass) {
			bar.bassDeferred++;
		}
	}
	if (part == kDrums && lane == kClosedHat) {
		lastClosedHatTick = tick;
		if (openHatRinging) {
			bar.openRingingAtClosed++;
		}
	}
	if (part == kDrums && lane == kOpenHat && tick == lastClosedHatTick) {
		bar.hatsTogether++;
	}
	bar.maxVoices = std::max<uint32_t>(bar.maxVoices, AudioEngine::getNumVoices());
}

void noteOff() {
	held--;
}

void hook(int64_t generatorNow, int32_t currentStep) {
	if (currentStep >= 0) {
		if (lastStep >= 0 && currentStep > lastStep + 1) {
			skippedSteps += currentStep - lastStep - 1;
		}
		else if (currentStep < lastStep) {
			backwardSteps++;
		}
		lastStep = currentStep;
	}

	if (generatorNow < 0 || generatorNow % kTicksPerStep != 0) {
		return;
	}
	int32_t step = static_cast<int32_t>(generatorNow / kTicksPerStep);
	if (step == bar.lastGridStep) {
		return; // The hook ran twice on one tick.
	}
	uint32_t sample = AudioEngine::audioSampleTimer;
	if (step % kStepsPerBar == 0) {
		flushBar();
		startBar(step / kStepsPerBar, sample);
	}
	if (bar.lastGridStep >= 0 && step == bar.lastGridStep + 1) {
		uint32_t interval = sample - bar.lastGridSample;
		if (bar.lastGridStep % 2 == 0) {
			bar.longSum += interval;
			bar.longCount++;
		}
		else {
			bar.shortSum += interval;
			bar.shortCount++;
		}
		bar.minInterval = std::min(bar.minInterval, interval);
		bar.maxInterval = std::max(bar.maxInterval, interval);
	}
	bar.lastGridStep = step;
	bar.lastGridSample = sample;
	bar.grids++;
}

void uiTick() {
	double now = getSystemTime();
	if (now < nextMemoryAt) {
		return;
	}
	nextMemoryAt = now + kMemoryInterval;
	TEST_EVENT("mem fast %u ext %u voices %d", static_cast<unsigned>(freeIn(MEMORY_REGION_INTERNAL)),
	           static_cast<unsigned>(freeIn(MEMORY_REGION_EXTERNAL)), static_cast<int>(AudioEngine::getNumVoices()));
}

void entered(double seconds, int32_t error) {
	TEST_EVENT("perform_enter ms %d err %d fast %u", static_cast<int>(seconds * 1000.0), static_cast<int>(error),
	           static_cast<unsigned>(freeIn(MEMORY_REGION_INTERNAL)));
}

void recipeSet(int32_t energy, int32_t density) {
	TEST_EVENT("recipe energy %d density %d", static_cast<int>(energy), static_cast<int>(density));
}

} // namespace deluge::generative::probe

#endif // ENABLE_SYSEX_TEST
