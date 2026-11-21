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

#if ENABLE_SYSEX_TEST

#include "generative/firmware/card_log_spike.h"
#include "OSLikeStuff/scheduler_api.h"
#include "deluge.h"
#include "extern.h"
#include "fatfs/fatfs.hpp"
#include "io/midi/test_port.h"
#include "io/midi/test_port_render.h"
#include "memory/general_memory_allocator.h"
#include <algorithm>
#include <cstring>

namespace deluge::generative::firmware::card_log_spike {
namespace {

constexpr uint32_t kHeaderBytes = 7 * 1024;
constexpr uint32_t kMaxChunk = 16 * 1024;
constexpr uint32_t kTicksPerUs = DELUGE_CLOCKS_PER / 1'000'000;

// Set by holds (MIDI routine) and read by the task.
uint32_t chunk = 0;
bool syncEach = false;
uint32_t bytesPerStep = 5;

// Written by the tick (which also runs inside the task's card calls, from routineForSD()), read by the task.
volatile uint32_t produced = 0;   ///< Synthetic log bytes so far
volatile uint32_t pieceSeq = 0;   ///< Pieces opened so far
volatile uint32_t pieceStart = 0; ///< `produced` at the newest piece's opening

// The task's own.
bool taskAdded = false;
char* buffer = nullptr;
FIL file;
bool fileOpen = false;
uint32_t filePiece = 0; ///< pieceSeq of the open file
uint32_t consumed = 0;  ///< `produced` written to the card or dropped
uint32_t fileIndex = 0; ///< CLOG0 / CLOG1 alternate: the previous piece's file is deleted at the next opening
bool hadFile[2] = {};

struct Window {
	uint32_t writes = 0;
	uint32_t bytes = 0;
	uint32_t writeMax = 0; ///< ticks
	uint32_t writeTotal = 0;
	uint32_t syncs = 0;
	uint32_t syncMax = 0;
	uint32_t openMax = 0;  ///< f_open of a new piece's file
	uint32_t closeMax = 0; ///< the piece's end: remainder write, f_sync, f_close
	uint32_t unlinkMax = 0;
	uint32_t opMax = 0;    ///< the longest single run of the task (all its card calls)
	uint32_t hist[6] = {}; ///< writes under 1, 2, 5, 10, 20 ms, and the rest
	uint32_t backlogMax = 0;
	uint32_t errors = 0;
};
Window window;
uint32_t worstEver = 0; ///< ticks, since the hold went on

// Stall test (#448 condition 5): every 2 s the task holds the card for `stallMs` without touching it, as a card
// that stays busy does (currentlyAccessingCard set, the scheduler yielded to), so no Cluster loads meanwhile.
// `steady`: every 500 ms one 4 KB write and an f_sync to /GENTEST/CSTEADY.BIN.
uint32_t stallMs = 0;
bool steady = false;
uint32_t lastStall = 0;
uint32_t lastSteady = 0;
FIL steadyFile;
bool steadyOpen = false;
uint32_t stalls = 0;
uint32_t stallMaxUs = 0;
bool neverDone() {
	return false;
}

const char* path(uint32_t index) {
	return index == 0 ? "/GENTEST/CLOG0.BIN" : "/GENTEST/CLOG1.BIN";
}

uint32_t since(uint32_t start) {
	return getSystemTicks() - start;
}

void noteWrite(uint32_t ticks, uint32_t bytes) {
	window.writes++;
	window.bytes += bytes;
	window.writeTotal += ticks;
	window.writeMax = std::max(window.writeMax, ticks);
	const uint32_t us = ticks / kTicksPerUs;
	const uint32_t bucket = us < 1000 ? 0 : us < 2000 ? 1 : us < 5000 ? 2 : us < 10000 ? 3 : us < 20000 ? 4 : 5;
	window.hist[bucket]++;
}

bool writeOut(uint32_t bytes) {
	const uint32_t t = getSystemTicks();
	UINT written = 0;
	const FRESULT r = f_write(&file, buffer, bytes, &written);
	const uint32_t ticks = since(t);
	noteWrite(ticks, bytes);
	TEST_EVENT("gen_cardlog_write bytes=%lu us=%lu", static_cast<unsigned long>(bytes),
	           static_cast<unsigned long>(ticks / kTicksPerUs));
	if (r != FR_OK || written != bytes) {
		window.errors++;
		return false;
	}
	return true;
}

void syncNow() {
	const uint32_t t = getSystemTicks();
	if (f_sync(&file) != FR_OK) {
		window.errors++;
	}
	const uint32_t ticks = since(t);
	window.syncs++;
	window.syncMax = std::max(window.syncMax, ticks);
}

/// The piece's end: the rest of its bytes, then f_sync and f_close (f_close syncs).
void closeFile(uint32_t upTo) {
	if (!fileOpen) {
		return;
	}
	const uint32_t t = getSystemTicks();
	uint32_t left = upTo - consumed;
	while (left > 0) {
		const uint32_t n = std::min(left, kMaxChunk);
		writeOut(n);
		left -= n;
	}
	consumed = upTo;
	if (f_close(&file) != FR_OK) {
		window.errors++;
	}
	fileOpen = false;
	window.closeMax = std::max(window.closeMax, since(t));
}

void unlinkFile(uint32_t index) {
	if (!hadFile[index]) {
		return;
	}
	const uint32_t t = getSystemTicks();
	f_unlink(path(index));
	window.unlinkMax = std::max(window.unlinkMax, since(t));
	hadFile[index] = false;
}

void openFile() {
	fileIndex ^= 1;
	unlinkFile(fileIndex); // the file of the piece before the last: as an unmarked piece's at the next opening
	const uint32_t t = getSystemTicks();
	if (f_open(&file, path(fileIndex), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
		fileOpen = true;
		hadFile[fileIndex] = true;
	}
	else {
		window.errors++;
	}
	window.openMax = std::max(window.openMax, since(t));
}

void stallTask() {
	const uint32_t now = getSystemTicks();
	if (stallMs != 0 && since(lastStall) / kTicksPerUs >= 2'000'000) {
		lastStall = now;
		const uint32_t late = TestPort::renderTiming.lateClusters;
		const uint32_t unloaded = TestPort::renderTiming.unloadedStarts;
		currentlyAccessingCard = 1;
		yieldingRoutineWithTimeoutForSD(neverDone, stallMs / 1000.);
		currentlyAccessingCard = 0;
		const uint32_t us = since(now) / kTicksPerUs;
		stalls++;
		stallMaxUs = std::max(stallMaxUs, us);
		TEST_EVENT("gen_stall n=%lu ms=%lu us=%lu late=%lu unloaded=%lu late_total=%lu",
		           static_cast<unsigned long>(stalls), static_cast<unsigned long>(stallMs),
		           static_cast<unsigned long>(us),
		           static_cast<unsigned long>(TestPort::renderTiming.lateClusters - late),
		           static_cast<unsigned long>(TestPort::renderTiming.unloadedStarts - unloaded),
		           static_cast<unsigned long>(TestPort::renderTiming.lateClusters));
	}
	if (steady && buffer != nullptr && since(lastSteady) / kTicksPerUs >= 500'000) {
		lastSteady = now;
		if (!steadyOpen) {
			steadyOpen = f_open(&steadyFile, "/GENTEST/CSTEADY.BIN", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
		}
		if (steadyOpen) {
			const uint32_t late = TestPort::renderTiming.lateClusters;
			UINT written = 0;
			f_write(&steadyFile, buffer, 4096, &written);
			f_sync(&steadyFile);
			const uint32_t us = since(now) / kTicksPerUs;
			stalls++;
			stallMaxUs = std::max(stallMaxUs, us);
			if (us > 20'000 || TestPort::renderTiming.lateClusters != late) {
				TEST_EVENT("gen_steady us=%lu late=%lu late_total=%lu", static_cast<unsigned long>(us),
				           static_cast<unsigned long>(TestPort::renderTiming.lateClusters - late),
				           static_cast<unsigned long>(TestPort::renderTiming.lateClusters));
			}
		}
	}
	if (!steady && steadyOpen) {
		f_close(&steadyFile);
		f_unlink("/GENTEST/CSTEADY.BIN");
		steadyOpen = false;
		TEST_EVENT("gen_steady cleaned ops=%lu max_us=%lu late_total=%lu unloaded_total=%lu",
		           static_cast<unsigned long>(stalls), static_cast<unsigned long>(stallMaxUs),
		           static_cast<unsigned long>(TestPort::renderTiming.lateClusters),
		           static_cast<unsigned long>(TestPort::renderTiming.unloadedStarts));
	}
}

void task() {
	const uint32_t t = getSystemTicks();
	stallTask();
	if (chunk == 0) {
		if (fileOpen || hadFile[0] || hadFile[1]) {
			closeFile(consumed);
			unlinkFile(0);
			unlinkFile(1);
			TEST_EVENT("gen_cardlog cleaned");
		}
		consumed = produced;
		filePiece = pieceSeq;
		return;
	}
	if (buffer == nullptr) {
		return;
	}
	if (pieceSeq != filePiece) {
		// A piece opened: the last one ends where the new one began, and its rest goes out before the new file opens.
		const uint32_t start = pieceStart;
		closeFile(start);
		consumed = start;
		openFile();
		filePiece = pieceSeq;
	}
	if (!fileOpen) {
		consumed = produced;
		return;
	}
	const uint32_t backlog = produced - consumed;
	window.backlogMax = std::max(window.backlogMax, backlog);
	if (backlog >= chunk) {
		writeOut(chunk);
		consumed += chunk;
		if (syncEach) {
			syncNow();
		}
	}
	const uint32_t ticks = since(t);
	window.opMax = std::max(window.opMax, ticks);
	worstEver = std::max(worstEver, ticks);
}

} // namespace

void setChunk(uint32_t bytes) {
	if (bytes != 0 && buffer == nullptr) {
		void* p = GeneralMemoryAllocator::get().allocLowSpeed(kMaxChunk + CACHE_LINE_SIZE * 2);
		if (p == nullptr) {
			return;
		}
		buffer = static_cast<char*>(p) + CACHE_LINE_SIZE;
		for (uint32_t i = 0; i < kMaxChunk; i++) {
			buffer[i] = static_cast<char>('A' + i % 26);
		}
	}
	if (!taskAdded) {
		// Low priority, the card's resource, as the audio recorder's slow routine: never inside a card wait.
		taskAdded = addRepeatingTask(task, 30, 0.002, 0.005, 0.05, "card log spike", RESOURCE_SD) >= 0;
	}
	chunk = std::min(bytes, kMaxChunk);
	window = {};
	worstEver = 0;
}

void setStall(uint32_t ms) {
	setChunk(chunk); // allocates the buffer and adds the task
	if (buffer == nullptr) {
		void* p = GeneralMemoryAllocator::get().allocLowSpeed(kMaxChunk + CACHE_LINE_SIZE * 2);
		if (p != nullptr) {
			buffer = static_cast<char*>(p) + CACHE_LINE_SIZE;
			for (uint32_t i = 0; i < kMaxChunk; i++) {
				buffer[i] = static_cast<char>('A' + i % 26);
			}
		}
	}
	stallMs = ms;
	stalls = 0;
	stallMaxUs = 0;
	TEST_EVENT("gen_stall set ms=%lu late_total=%lu unloaded_total=%lu", static_cast<unsigned long>(ms),
	           static_cast<unsigned long>(TestPort::renderTiming.lateClusters),
	           static_cast<unsigned long>(TestPort::renderTiming.unloadedStarts));
}

void setSteady(bool on) {
	setStall(stallMs);
	steady = on;
}

void setSyncEach(bool on) {
	syncEach = on;
}

void setBusy(bool on) {
	bytesPerStep = on ? 64 : 5;
}

void stepsDecided(int32_t steps) {
	produced = produced + bytesPerStep * static_cast<uint32_t>(steps);
}

void pieceOpening() {
	pieceStart = produced;
	produced = produced + kHeaderBytes;
	pieceSeq = pieceSeq + 1;
}

void report() {
	const TestPort::RenderTiming& r = TestPort::renderTiming;
	auto us = [](uint32_t ticks) { return static_cast<unsigned long>(ticks / kTicksPerUs); };
	TEST_EVENT("gen_stream late=%lu unloaded=%lu drained_max=%lu gap_max_us=%lu render=%lu/%lu/%lu/%lu/%lu/%lu/%lu",
	           static_cast<unsigned long>(r.lateClusters), static_cast<unsigned long>(r.unloadedStarts),
	           static_cast<unsigned long>(r.drainedMax), us(r.gapMaxCounts), static_cast<unsigned long>(r.hist[0]),
	           static_cast<unsigned long>(r.hist[1]), static_cast<unsigned long>(r.hist[2]),
	           static_cast<unsigned long>(r.hist[3]), static_cast<unsigned long>(r.hist[4]),
	           static_cast<unsigned long>(r.hist[5]), static_cast<unsigned long>(r.hist[6]));
	TEST_EVENT("gen_gaps %lu/%lu/%lu/%lu/%lu/%lu", static_cast<unsigned long>(r.gaps[0]),
	           static_cast<unsigned long>(r.gaps[1]), static_cast<unsigned long>(r.gaps[2]),
	           static_cast<unsigned long>(r.gaps[3]), static_cast<unsigned long>(r.gaps[4]),
	           static_cast<unsigned long>(r.gaps[5]));
	if (chunk != 0) {
		TEST_EVENT("gen_cardlog chunk=%lu sync=%s writes=%lu bytes=%lu mean_us=%lu max_us=%lu "
		           "hist=%lu/%lu/%lu/%lu/%lu/%lu",
		           static_cast<unsigned long>(chunk), syncEach ? "each" : "end",
		           static_cast<unsigned long>(window.writes), static_cast<unsigned long>(window.bytes),
		           window.writes > 0 ? us(window.writeTotal / window.writes) : 0UL, us(window.writeMax),
		           static_cast<unsigned long>(window.hist[0]), static_cast<unsigned long>(window.hist[1]),
		           static_cast<unsigned long>(window.hist[2]), static_cast<unsigned long>(window.hist[3]),
		           static_cast<unsigned long>(window.hist[4]), static_cast<unsigned long>(window.hist[5]));
		TEST_EVENT("gen_cardlog_ops syncs=%lu sync=%lu open=%lu close=%lu unlink=%lu "
		           "task=%lu worst=%lu backlog=%lu err=%lu",
		           static_cast<unsigned long>(window.syncs), us(window.syncMax), us(window.openMax),
		           us(window.closeMax), us(window.unlinkMax), us(window.opMax), us(worstEver),
		           static_cast<unsigned long>(window.backlogMax), static_cast<unsigned long>(window.errors));
	}
	window = {};
}

} // namespace deluge::generative::firmware::card_log_spike

#endif
