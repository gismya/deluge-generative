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
// The steering log on the card (spec §17.2): each piece's image streams to its own file in GENERATIVE/CAPTURE/, written
// by a low-priority main-loop task that holds the card, never by the tick or the render. The folder is cleaned of
// unmarked files at Perform's entry and when a leave ends; marked files found at entry come back as marked pieces
// (spec §17.4), so a power-off loses at most what wasn't flushed yet. Capture reads each kept piece back from its file.

#pragma once

#include "definitions_cxx.hpp"
#include "fatfs/ff.h"
#include "generative/core/steering_log.h"
#include <cstdint>

namespace deluge::generative::firmware {

constexpr char kCaptureFolder[] = "GENERATIVE/CAPTURE";
/// A write takes this much of a piece at a time (spec §17.2, the card-write spike, #447): one f_write of 2–6 ms.
constexpr int32_t kLogChunkBytes = 4096;
/// A piece still playing is written out and synced at least this often (*tuning*): a power-off loses at most this.
constexpr double kLogFlushSeconds = 10.0;
/// A spare file for the next piece is made only this long after a piece opened: away from the piece change.
constexpr double kLogSpareAfterSeconds = 2.0;

class CardLog {
public:
	/// Perform's entry, from its job, with `log` empty: the folder made, unmarked or unreadable files deleted, marked
	/// ones adopted into `log` as earlier marked pieces, and the task started. A card error leaves the log working
	/// in RAM: its pieces fail to store (storeFailed()), as on a card pulled later.
	Error enter(SteeringLog& log);
	/// Stops the task and stores everything the log still holds, every file closed with its info (from a job or the
	/// UI, after the last piece ended). The log stays readable.
	void finish();
	/// Forgets the log (it is freed next): finish() first.
	void forget();
	/// Reads piece `id`'s file into a buffer from allocExternal(), made readable (SteeringLog::readImage()).
	/// `*image` is the caller's to delugeDealloc(). FILE_CORRUPTED for a file that isn't an image this firmware reads.
	Error load(uint32_t id, uint8_t** image, int32_t* bytes);
	/// Deletes piece `id`'s file (Capture saved its song).
	void remove(uint32_t id);
	/// Deletes every piece file, or (`keepMarked`) every one whose info isn't marked (spec §17.4). From a job.
	void clean(bool keepMarked);
	/// The task's step: at most one card operation of note (spec §17.2).
	void step(bool finishing);

#if ENABLE_SYSEX_TEST
	/// Test port: the log takes every byte and writes nothing (no file is opened): the card log off, for an on/off
	/// comparison. Set before Perform's entry.
	void setOff(bool on) { off_ = on; }
	/// Test port: every 2 s the task holds the card for `ms` without touching it, as a card that stays busy does
	/// (currentlyAccessingCard set, the scheduler yielded to: no Cluster loads meanwhile). 0: off.
	void setStall(uint32_t ms) { stallMs_ = ms; }
	/// gen_card_log_ops, gen_stream and gen_gaps for the window since the last (with gen_h16, every 4 bars), then a
	/// new window. Before ControlTiming resets the render timing.
	void report();
#endif

private:
	void openFor(int32_t slot, uint32_t id);
	void closeWriting();
	void fail(int32_t slot, FRESULT result);
	bool writeInfo(FIL& file, int32_t slot);
	void makeSpare();
	void dropSpare();

	// In SDRAM, zeroed at boot: every member reads right as zero until enter() sets it up.
	SteeringLog* log_ = nullptr;
	bool taskAdded_ = false;
	int8_t task_ = 0;
	FIL files_[2];
	FIL other_;            ///< A closed piece's file, opened again for its info
	int32_t writing_ = -1; ///< files_ index open for the oldest piece's image, or -1
	int32_t writingSlot_ = -1;
	int32_t spare_ = -1; ///< files_ index made ahead for the next piece, or -1
	uint32_t spareId_ = 0;
	bool flushing_ = false;
	bool dirty_ = false; ///< Written since the last sync
	double lastSync_ = 0;
	uint32_t seenOpened_ = 0;
	double openedAt_ = 0;
	bool failed_[kMaxLogPieces] = {};
	bool closed_[kMaxLogPieces] = {};      ///< Its file is closed, every byte in it.
	uint32_t version_[kMaxLogPieces] = {}; ///< The info version its file holds.
	bool errorShown_ = false;
#if ENABLE_SYSEX_TEST
	bool off_ = false;
	uint32_t stallMs_ = 0;
	double lastStall_ = 0;
	struct Window {
		uint32_t writes = 0;
		uint32_t writeTotal = 0; ///< ticks
		uint32_t writeMax = 0;
		uint32_t syncs = 0;
		uint32_t syncMax = 0;
		uint32_t opens = 0;
		uint32_t openMax = 0;
		uint32_t closeMax = 0;
		uint32_t stepMax = 0; ///< the longest step: the main loop blocked
		int32_t ringMax = 0;
		uint32_t stalls = 0;
	} window_;
	void stallNow();
#endif
};

extern CardLog cardLog;

} // namespace deluge::generative::firmware
