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

#include "generative/firmware/capture_files.h"
#include "OSLikeStuff/scheduler_api.h"
#include "definitions.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#if ENABLE_SYSEX_TEST
#include "deluge.h"
#include "extern.h"
#include "io/midi/test_port_render.h"
#endif
#include <algorithm>
#include <cstring>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS CardLog cardLog; // its FatFS file objects are 1.7 KB: not internal RAM

namespace {

/// Piece `id`'s file: GENERATIVE/CAPTURE/P0000042.LOG.
void pathOf(uint32_t id, char (&out)[40]) {
	// By hand: snprintf() with a number pulls newlib's stdio, which the firmware doesn't link.
	static_assert(sizeof kCaptureFolder + 13 <= sizeof out);
	std::memcpy(out, kCaptureFolder, sizeof kCaptureFolder - 1);
	char* at = out + sizeof kCaptureFolder - 1;
	*at++ = '/';
	*at++ = 'P';
	for (int32_t digit = 6; digit >= 0; digit--) {
		at[digit] = static_cast<char>('0' + id % 10);
		id /= 10;
	}
	std::memcpy(at + 7, ".LOG", 5);
}

/// The id in a piece file's name, or 0 for a name that isn't one.
uint32_t idOf(const char* name) {
	if (name[0] != 'P' || std::strlen(name) != 12 || std::strcmp(name + 8, ".LOG") != 0) {
		return 0;
	}
	uint32_t id = 0;
	for (int32_t i = 1; i < 8; i++) {
		if (name[i] < '0' || name[i] > '9') {
			return 0;
		}
		id = id * 10 + static_cast<uint32_t>(name[i] - '0');
	}
	return id;
}

/// Reads all of `path` into a buffer from allocExternal(). @return FR_OK, with `*bytes` its length.
FRESULT readAll(const char* path, uint8_t** out, int32_t* bytes) {
	*out = nullptr;
	*bytes = 0;
	FIL file;
	FRESULT result = f_open(&file, path, FA_READ);
	if (result != FR_OK) {
		return result;
	}
	const auto size = static_cast<int32_t>(f_size(&file));
	auto* buffer = static_cast<uint8_t*>(GeneralMemoryAllocator::get().allocExternal(std::max<int32_t>(size, 4)));
	if (buffer == nullptr) {
		f_close(&file);
		return FR_NOT_ENOUGH_CORE;
	}
	UINT read = 0;
	result = f_read(&file, buffer, static_cast<UINT>(size), &read);
	f_close(&file);
	if (result != FR_OK || static_cast<int32_t>(read) != size) {
		delugeDealloc(buffer);
		return result != FR_OK ? result : FR_DISK_ERR;
	}
	*out = buffer;
	*bytes = size;
	return FR_OK;
}

void taskEntry() {
	cardLog.step(false);
}

#if ENABLE_SYSEX_TEST
constexpr uint32_t kTicksPerUs = DELUGE_CLOCKS_PER / 1'000'000;
/// Runs `call`, its ticks into `max` (and `total`, `count`).
template <typename Call>
auto timed(uint32_t& max, Call call, uint32_t* total = nullptr, uint32_t* count = nullptr) {
	const uint32_t started = getSystemTicks();
	const auto result = call();
	const uint32_t ticks = getSystemTicks() - started;
	max = std::max(max, ticks);
	if (total != nullptr) {
		*total += ticks;
		(*count)++;
	}
	return result;
}
#define CARD_TIMED(field, call) timed(window_.field, [&] { return call; })
#define CARD_WRITE_TIMED(call) timed(window_.writeMax, [&] { return call; }, &window_.writeTotal, &window_.writes)
bool neverDone() {
	return false;
}
#else
#define CARD_TIMED(field, call) (call)
#define CARD_WRITE_TIMED(call) (call)
#endif

} // namespace

Error CardLog::enter(SteeringLog& log) {
#if ENABLE_SYSEX_TEST
	const bool off = off_;
	const uint32_t stallMs = stallMs_;
#endif
	*this = CardLog{};
	log_ = &log;
#if ENABLE_SYSEX_TEST
	off_ = off;
	stallMs_ = stallMs;
#endif
	Error error = StorageManager::initSD();
	if (error == Error::NONE) {
		f_mkdir("GENERATIVE"); // it may be there already
		const FRESULT made = f_mkdir(kCaptureFolder);
		if (made != FR_OK && made != FR_EXIST) {
			error = fresultToDelugeErrorCode(made);
		}
	}
	uint32_t highest = 0;
	int32_t adopted = 0;
	int32_t deleted = 0;
	DIR dir;
	if (error == Error::NONE && f_opendir(&dir, kCaptureFolder) == FR_OK) {
		FILINFO info;
		// Names first: deleting while reading the folder could skip entries.
		static constexpr int32_t kMaxFiles = 256;
		uint32_t ids[kMaxFiles];
		int32_t found = 0;
		while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0 && found < kMaxFiles) {
			const uint32_t id = (info.fattrib & AM_DIR) != 0 ? 0 : idOf(info.fname);
			if (id != 0) {
				ids[found++] = id;
			}
		}
		f_closedir(&dir);
		// Oldest first, so adopted pieces keep their set order.
		std::sort(ids, ids + found);
		for (int32_t i = 0; i < found; i++) {
			highest = std::max(highest, ids[i]);
			char path[40];
			pathOf(ids[i], path);
			uint8_t* image = nullptr;
			int32_t bytes = 0;
			bool keep = false;
			if (readAll(path, &image, &bytes) == FR_OK && SteeringLog::readImage(image, bytes)) {
				const auto* info = reinterpret_cast<const PieceInfo*>(image);
				keep = info->marked && info->id == ids[i] && log.adopt(*info);
			}
			if (image != nullptr) {
				delugeDealloc(image);
			}
			if (keep) {
				adopted++;
			}
			else {
				f_unlink(path);
				deleted++;
			}
		}
	}
	log.continueIds(highest + 1);
	TEST_EVENT("gen_card_log enter adopted=%ld deleted=%ld next=%lu error=%d", static_cast<long>(adopted),
	           static_cast<long>(deleted), static_cast<unsigned long>(highest + 1), static_cast<int>(error));
	seenOpened_ = log.opened();
	openedAt_ = getSystemTime();
	lastSync_ = getSystemTime();
	// Low priority and the card's resource, as the audio recorder's slow routine: it never runs inside a card wait,
	// and never in the tick or the render (spec §17.2).
	task_ = addRepeatingTask(taskEntry, 30, 0.002, 0.005, 0.05, "capture log", RESOURCE_SD);
	taskAdded_ = task_ >= 0;
	return error;
}

void CardLog::finish() {
	if (taskAdded_) {
		removeTask(task_);
		taskAdded_ = false;
	}
	if (log_ == nullptr) {
		return;
	}
	for (int32_t guard = 0; guard < 100000; guard++) {
		bool busy = writing_ >= 0;
		for (int32_t s = 0; s < kMaxLogPieces && !busy; s++) {
			busy = log_->used(s)
			       && (!log_->allTaken(s) || log_->dropped(s) || (!closed_[s] && !failed_[s])
			           || (!failed_[s] && version_[s] != log_->infoVersion(s)));
		}
		if (!busy) {
			break;
		}
		step(true);
	}
	dropSpare();
}

void CardLog::forget() {
	finish();
	log_ = nullptr;
}

void CardLog::openFor(int32_t slot, uint32_t id) {
	char path[40];
	pathOf(id, path);
	const bool usedSpare = spare_ >= 0 && spareId_ == id;
	if (usedSpare) {
		writing_ = spare_; // made ahead: no file creation at the piece change (spec §17.2)
		spare_ = -1;
	}
	else {
		writing_ = spare_ == 0 ? 1 : 0;
		const FRESULT result = CARD_TIMED(openMax, f_open(&files_[writing_], path, FA_WRITE | FA_CREATE_ALWAYS));
		if (result != FR_OK) {
			writing_ = -1;
			fail(slot, result);
			return;
		}
	}
	writingSlot_ = slot;
	closed_[slot] = false;
	version_[slot] = log_->infoVersion(slot) - 1;
	lastSync_ = getSystemTime();
	TEST_EVENT("gen_card_log open piece=%lu spare=%d", static_cast<unsigned long>(id), usedSpare ? 1 : 0);
}

bool CardLog::writeInfo(FIL& file, int32_t slot) {
	PieceInfo info;
	log_->imageInfo(slot, info);
	const FSIZE_t end = f_tell(&file);
	UINT written = 0;
	if (f_lseek(&file, 0) != FR_OK || f_write(&file, &info, sizeof info, &written) != FR_OK || written != sizeof info
	    || f_lseek(&file, std::max<FSIZE_t>(end, sizeof info)) != FR_OK) {
		return false;
	}
	version_[slot] = log_->infoVersion(slot);
	return true;
}

void CardLog::closeWriting() {
	if (writing_ < 0) {
		return;
	}
	const int32_t slot = writingSlot_;
	FIL& file = files_[writing_];
	bool ok = true;
	if (!log_->dropped(slot) && !failed_[slot]) {
		ok = writeInfo(file, slot); // the piece's end, its mark and its cut (spec §17.4)
	}
	const FRESULT closed = CARD_TIMED(closeMax, f_close(&file)); // f_close syncs
	writing_ = -1;
	writingSlot_ = -1;
	if (!ok || closed != FR_OK) {
		fail(slot, closed != FR_OK ? closed : FR_DISK_ERR);
		return;
	}
	closed_[slot] = true;
	TEST_EVENT("gen_card_log closed piece=%lu bytes=%ld dropped=%d", static_cast<unsigned long>(log_->idOf(slot)),
	           static_cast<long>(log_->taken(slot)), log_->dropped(slot) ? 1 : 0);
}

void CardLog::fail(int32_t slot, FRESULT result) {
	if (writing_ >= 0 && writingSlot_ == slot) {
		f_close(&files_[writing_]);
		writing_ = -1;
		writingSlot_ = -1;
	}
	if (!failed_[slot]) {
		failed_[slot] = true;
		log_->storeFailed(slot); // the playing piece is cut as on a full log (spec §17.2)
		const Error error = fresultToDelugeErrorCode(result);
		TEST_EVENT("gen_card_log failed piece=%lu result=%d", static_cast<unsigned long>(log_->idOf(slot)),
		           static_cast<int>(result));
		if (!errorShown_) {
			errorShown_ = true;
			display->displayError(error); // the stock card error, once (spec §17.2)
		}
	}
}

void CardLog::makeSpare() {
	const uint32_t id = log_->nextId();
	char path[40];
	pathOf(id, path);
	const int32_t index = writing_ == 0 ? 1 : 0;
	if (CARD_TIMED(openMax, f_open(&files_[index], path, FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK) {
		spare_ = index;
		spareId_ = id;
	}
}

void CardLog::dropSpare() {
	if (spare_ < 0) {
		return;
	}
	char path[40];
	pathOf(spareId_, path);
	f_close(&files_[spare_]);
	f_unlink(path); // empty: no piece came to it
	spare_ = -1;
	spareId_ = 0;
}

void CardLog::step(bool finishing) {
	if (log_ == nullptr) {
		return;
	}
#if ENABLE_SYSEX_TEST
	const uint32_t stepStarted = getSystemTicks();
	window_.ringMax = std::max(window_.ringMax, log_->used());
	if (off_) {
		// The card log off: every byte taken, nothing written (an on/off comparison).
		LogChunk chunk;
		while (log_->nextChunk(chunk, INT32_MAX)) {
			log_->took(chunk);
		}
		for (int32_t s = 0; s < kMaxLogPieces; s++) {
			if (log_->used(s) && log_->dropped(s) && log_->allTaken(s)) {
				log_->released(s);
			}
		}
		return;
	}
	if (stallMs_ != 0 && !finishing && getSystemTime() - lastStall_ >= 2.0) {
		stallNow();
	}
	struct StepTimer {
		Window& window;
		uint32_t started;
		~StepTimer() { window.stepMax = std::max(window.stepMax, getSystemTicks() - started); }
	} stepTimer{window_, stepStarted};
#endif
	const double now = getSystemTime();
	if (log_->opened() != seenOpened_) {
		seenOpened_ = log_->opened();
		openedAt_ = now;
		errorShown_ = false; // a new piece shows its own card error
	}
	// A thrown-away piece's slot goes back once nothing of it is left to skip.
	for (int32_t s = 0; s < kMaxLogPieces; s++) {
		if (log_->used(s) && log_->dropped(s) && log_->allTaken(s) && writingSlot_ != s) {
			log_->released(s);
			failed_[s] = false;
			closed_[s] = false;
		}
	}
	LogChunk chunk;
	if (log_->nextChunk(chunk, kLogChunkBytes)) {
		const int32_t slot = chunk.slot;
		if (chunk.data == nullptr || failed_[slot]) {
			log_->took(chunk); // thrown away, or its file failed: nothing more goes to the card
		}
		else {
			if (writingSlot_ != slot) {
				closeWriting(); // never with bytes left: the ring holds pieces in set order
				openFor(slot, chunk.id);
				if (writing_ < 0) {
					return;
				}
			}
			flushing_ = flushing_ || now - lastSync_ >= kLogFlushSeconds;
			// 4 KB at a time while it plays; all of it once it ends, or for a timed flush (spec §17.2).
			const bool worth = finishing || flushing_ || !log_->growing(slot) || log_->ready(slot) >= kLogChunkBytes;
			if (!worth) {
				return;
			}
			UINT written = 0;
			FRESULT result = FR_OK;
			if (chunk.offset == 0) {
				PieceInfo info;
				log_->imageInfo(slot, info); // the opening as the image holds it (the genre as its index)
				result = CARD_WRITE_TIMED(f_write(&files_[writing_], &info, sizeof info, &written));
				written = written == sizeof info ? static_cast<UINT>(chunk.length) : 0;
			}
			else {
				result =
				    CARD_WRITE_TIMED(f_write(&files_[writing_], chunk.data, static_cast<UINT>(chunk.length), &written));
			}
			if (result != FR_OK || static_cast<int32_t>(written) != chunk.length) {
				fail(slot, result != FR_OK ? result : FR_DENIED); // a full card writes short
				log_->took(chunk);
				return;
			}
			log_->took(chunk);
			dirty_ = true;
			return;
		}
	}
	if (writing_ >= 0) {
		const int32_t slot = writingSlot_;
		if (log_->allTaken(slot)) {
			closeWriting(); // the piece's end: its info, then f_close
			return;
		}
		if (flushing_ || now - lastSync_ >= kLogFlushSeconds) {
			// A timed flush: everything ready is written (above); its info and an f_sync.
			flushing_ = false;
			lastSync_ = now;
			if (!dirty_ && version_[slot] == log_->infoVersion(slot)) {
				return; // nothing new since the last sync
			}
			dirty_ = false;
			if (!writeInfo(files_[writing_], slot)) {
				fail(slot, FR_DISK_ERR);
				return;
			}
			const FRESULT synced = CARD_TIMED(syncMax, f_sync(&files_[writing_]));
			TEST_EVENT("gen_card_log flush piece=%lu bytes=%ld", static_cast<unsigned long>(log_->idOf(slot)),
			           static_cast<long>(log_->taken(slot)));
#if ENABLE_SYSEX_TEST
			window_.syncs++;
#endif
			if (synced != FR_OK) {
				fail(slot, synced);
			}
			return;
		}
	}
	// A mark changed on a piece whose file is closed (CAPTURE ▸, or RECORD while stopped): its info again.
	for (int32_t s = 0; s < kMaxLogPieces; s++) {
		if (log_->used(s) && !log_->dropped(s) && closed_[s] && !failed_[s] && version_[s] != log_->infoVersion(s)) {
			char path[40];
			pathOf(log_->idOf(s), path);
			FRESULT result = f_open(&other_, path, FA_WRITE | FA_OPEN_EXISTING);
			if (result == FR_OK) {
				result = writeInfo(other_, s) ? FR_OK : FR_DISK_ERR;
				const FRESULT closed = f_close(&other_);
				result = result != FR_OK ? result : closed;
			}
			if (result != FR_OK) {
				fail(s, result);
			}
			return;
		}
	}
	// The next piece's file, made ahead in a quiet moment (spec §17.2).
	if (!finishing && spare_ < 0 && now - openedAt_ >= kLogSpareAfterSeconds) {
		makeSpare();
	}
}

#if ENABLE_SYSEX_TEST
void CardLog::stallNow() {
	lastStall_ = getSystemTime();
	const uint32_t late = TestPort::renderTiming.lateClusters;
	const uint32_t started = getSystemTicks();
	currentlyAccessingCard = 1;
	yieldingRoutineWithTimeoutForSD(neverDone, stallMs_ / 1000.);
	currentlyAccessingCard = 0;
	window_.stalls++;
	TEST_EVENT("gen_card_stall ms=%lu us=%lu late=%lu", static_cast<unsigned long>(stallMs_),
	           static_cast<unsigned long>((getSystemTicks() - started) / kTicksPerUs),
	           static_cast<unsigned long>(TestPort::renderTiming.lateClusters - late));
}

void CardLog::report() {
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
	if (log_ != nullptr && !off_) {
		TEST_EVENT("gen_card_log_ops writes=%lu mean_us=%lu max_us=%lu syncs=%lu sync_us=%lu open_us=%lu close_us=%lu "
		           "step_us=%lu ring_max=%ld stalls=%lu",
		           static_cast<unsigned long>(window_.writes),
		           window_.writes > 0 ? us(window_.writeTotal / window_.writes) : 0UL, us(window_.writeMax),
		           static_cast<unsigned long>(window_.syncs), us(window_.syncMax), us(window_.openMax),
		           us(window_.closeMax), us(window_.stepMax), static_cast<long>(window_.ringMax),
		           static_cast<unsigned long>(window_.stalls));
	}
	window_ = {};
}
#endif

void CardLog::remove(uint32_t id) {
	char path[40];
	pathOf(id, path);
	const FRESULT result = f_unlink(path);
	TEST_EVENT("gen_card_log removed piece=%lu result=%d", static_cast<unsigned long>(id), static_cast<int>(result));
}

void CardLog::clean(bool keepMarked) {
	if (StorageManager::initSD() != Error::NONE) {
		return;
	}
	DIR dir;
	if (f_opendir(&dir, kCaptureFolder) != FR_OK) {
		return;
	}
	static constexpr int32_t kMaxFiles = 256;
	uint32_t ids[kMaxFiles];
	int32_t found = 0;
	FILINFO info;
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0 && found < kMaxFiles) {
		const uint32_t id = (info.fattrib & AM_DIR) != 0 ? 0 : idOf(info.fname);
		if (id != 0) {
			ids[found++] = id;
		}
	}
	f_closedir(&dir);
	int32_t deleted = 0;
	for (int32_t i = 0; i < found; i++) {
		char path[40];
		pathOf(ids[i], path);
		bool keep = false;
		if (keepMarked) {
			FIL file;
			PieceInfo piece;
			UINT read = 0;
			if (f_open(&file, path, FA_READ) == FR_OK) {
				keep = f_read(&file, &piece, sizeof piece, &read) == FR_OK && read == sizeof piece
				       && piece.magic == PieceInfo::kMagic && piece.marked;
				f_close(&file);
			}
		}
		if (!keep) {
			f_unlink(path);
			deleted++;
		}
	}
	TEST_EVENT("gen_card_log clean keep_marked=%d deleted=%ld", keepMarked ? 1 : 0, static_cast<long>(deleted));
}

Error CardLog::load(uint32_t id, uint8_t** image, int32_t* bytes) {
	char path[40];
	pathOf(id, path);
	const FRESULT result = readAll(path, image, bytes);
	if (result != FR_OK) {
		return fresultToDelugeErrorCode(result);
	}
	if (!SteeringLog::readImage(*image, *bytes)) {
		delugeDealloc(*image);
		*image = nullptr;
		return Error::FILE_CORRUPTED;
	}
	return Error::NONE;
}

} // namespace deluge::generative::firmware
