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

// The steering log (spec §17.2, §17.3): the pieces played in Perform, each a header and the steering that landed in it,
// in one buffer the driver hands it (Perform: a fixed 128 KB block in SDRAM, spec §20). The core appends commands
// where they land (ADR 0005); the Perform driver opens and ends pieces and adds what only it knows (the header's sound
// picks and param snapshot, a re-roll's paths). Capture (#145) reads it back.
//
// Layout: pieces one after another, each aligned for its PieceInfo: a PieceInfo, the header (the piece's opening, then
// its header records), then its records in step order. Records are byte-packed (encodeCommand()): a command takes 4
// bytes as a rule, so busy steering costs little room. Only the newest piece is written; a piece that ended unmarked is
// thrown away when the next one begins, so the buffer always reads marked pieces, then the last piece.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

/// How a piece opens (spec §17.3), as its header holds it: what replay starts the piece's instance with.
struct PieceStart {
	Recipe recipe; ///< At step 0, before its steering: a transition's new roster has the macros it carries over.
	uint32_t seed = 0;
	Takes takes;
	SectionKind first = SectionKind::Intro; ///< Intro, or Groove (SHIFT+PLAY, a cut, a Drop).
	bool onDrop = false;                    ///< The first Groove opens on a Drop (BUILD INTO A DROP).
	bool held = false;                      ///< The Arc's hold, carried over from the outgoing piece (spec §9.7).
};

/// How a piece ended (spec §17.3).
enum class PieceEnd : uint8_t {
	Playing,    ///< Not yet: the current piece.
	Stopped,    ///< The transport stopped, or Perform was left.
	Outro,      ///< End's Outro finished.
	Transition, ///< A transition's start step: the next piece starts there.
};

/// Where a sound pick sits, in a header's picks and a re-roll's paths: a part's preset, or a lane's sample.
constexpr int32_t pickSlot(int32_t part) {
	return part;
}
constexpr int32_t pickSlot(Lane lane) {
	return kMaxParts + static_cast<int32_t>(lane);
}
constexpr int32_t kPickSlots = kMaxParts + kNumLanes;

/// One logged record, as read back.
enum class RecordKind : uint8_t {
	Command,      ///< A command, where it landed.
	SoundPath,    ///< A sound re-roll's new pick: command.target its part, command.value its token, `slot`, `path`.
	SoundDropped, ///< The newest SetSound on command.target never played: a confirm dropped it (spec §12.9).
};
struct LogRecord {
	RecordKind kind = RecordKind::Command;
	int32_t step = 0;
	Command command;
	int32_t slot = 0;
	const char* path = nullptr; ///< Not terminated: `pathLength` chars.
	int32_t pathLength = 0;
};

/// Perform's log: one fixed buffer in SDRAM, allocated on Perform entry (spec §17.2, §20).
constexpr int32_t kPerformLogBytes = 128 * 1024;

/// The bytes a command takes in the log, at most (encodeCommand()).
constexpr int32_t kMaxCommandBytes = 1 + 5 + sizeof(Command::target) + 5 + 3 + sizeof(Command::lane);
static_assert(kMaxCommandBytes == 16,
              "the kind, the delta, the target, the value, the length and the lane at their widest");
/// The bytes a SoundDropped record takes, at most: its tag, the delta, the part.
constexpr int32_t kMaxDroppedBytes = 1 + 5 + 1;
/// Room the log keeps for SoundDropped records alone, one per part: a full log still says a swap it logged never
/// played (spec §17.2).
constexpr int32_t kDroppedReserve = kMaxParts * kMaxDroppedBytes;

/// Packs `command`, stamped `delta` steps after the record before it, into `out` (kMaxCommandBytes at least).
/// @return the bytes written. Byte 0 is the kind, with bit 6 set when the length and lane follow; then the delta
/// (LEB128), the target, the value (LEB128, zigzagged), and the length (LEB128) and lane if any.
int32_t encodeCommand(const Command& command, uint32_t delta, uint8_t* out);

class SteeringLog {
public:
	/// Over `bytes` of `buffer`, aligned for a pointer. Empty.
	SteeringLog(uint8_t* buffer, int32_t bytes) : buffer_(buffer), capacity_(bytes) {}

	// ---- Pieces (spec §17.3): the Perform driver ----

	/// A piece starts, from `start`. The newest piece, if it ended and isn't marked, is thrown away first (spec
	/// §17.3). If even the opening doesn't fit, no piece opens: nothing logs until the next one.
	void beginPiece(const PieceStart& start);
	/// Header records, after beginPiece() and before the piece's first record. A sound pick, by pickSlot(): an empty
	/// path is skipped.
	void addPick(int32_t slot, const char* path);
	/// A block of the param snapshot (spec §17.2), in the driver's own layout. Up to 65,535 bytes.
	void addSnapshot(const void* data, int32_t length);
	/// As addSnapshot(), for a caller that writes the block in place: its `length` bytes, to fill before anything
	/// else is logged, or nullptr where addSnapshot() would have cut the piece.
	uint8_t* snapshotSpace(int32_t length);
	/// The current piece ends before `step`: its first step not played.
	void endPiece(int32_t step, PieceEnd why);
	/// Marks the newest piece to keep, or unmarks it (spec §17.4).
	void mark(bool on);

	// ---- Records, in step order within the piece ----

	/// Logs a command where it landed. @return false if no piece logs (none open, or it was cut): the entry is lost.
	bool append(int32_t step, const Command& command);
	/// A sound re-roll's new pick, logged with the SetSound that landed it: one record per pick slot.
	bool appendSoundPath(int32_t step, uint8_t part, uint8_t token, int32_t slot, const char* path);
	/// The newest SetSound logged on `part` never played: its swap was dropped. Stamped no earlier than the record
	/// before it, and inside the piece's span: a log cut since the SetSound still takes it, from the room kept for it
	/// (kDroppedReserve), which holds one for every part: a landing gets one drop. Nothing is written when the SetSound
	/// itself was cut away, or for a landing already dropped.
	bool appendSoundDropped(int32_t step, uint8_t part);

	// ---- Reading ----

	/// One piece in the log, read in place.
	class Piece {
	public:
		[[nodiscard]] const PieceStart& start() const;
		[[nodiscard]] PieceEnd ended() const;
		/// The step it ended before (endPiece()), or -1 while it plays.
		[[nodiscard]] int32_t endStep() const;
		/// Where logging stopped because the log was full: the first step not logged, or -1 (spec §17.2).
		[[nodiscard]] int32_t cutAt() const;
		[[nodiscard]] bool marked() const;
		/// All its bytes, and the header's (the opening and the header records).
		[[nodiscard]] int32_t bytes() const;
		[[nodiscard]] int32_t headerBytes() const;
		/// Copies the pick in `slot` into `out` (kMaxSoundPath), terminated. @return false, with `out` empty, if the
		/// header has none.
		bool pick(int32_t slot, char* out) const;
		/// Hands each snapshot block to `found(data, length)`.
		template <typename Found>
		void forEachSnapshot(Found found) const {
			forEachHeader([&](uint8_t tag, const uint8_t* data, int32_t length) {
				if (tag == kSnapshotTag) {
					found(static_cast<const void*>(data), length);
				}
			});
		}
		/// Hands each record to `found(const LogRecord&)`, in order.
		template <typename Found>
		void forEachRecord(Found found) const {
			const uint8_t* at = records();
			const uint8_t* end = base_ + bytes();
			int32_t step = 0;
			LogRecord record;
			while (at < end) {
				at = decode(at, step, record);
				step = record.step;
				found(record);
			}
		}

	private:
		friend class SteeringLog;
		explicit Piece(const uint8_t* base) : base_(base) {}
		template <typename Found>
		void forEachHeader(Found found) const;
		[[nodiscard]] const uint8_t* records() const;
		static const uint8_t* decode(const uint8_t* at, int32_t step, LogRecord& out);
		const uint8_t* base_;
	};

	[[nodiscard]] int32_t pieces() const { return pieces_; }
	[[nodiscard]] Piece piece(int32_t i) const;
	/// Whether the newest piece stopped logging because the log was full, or found no room to open: new marks are
	/// refused (spec §17.2).
	[[nodiscard]] bool full() const { return cut_; }
	/// Bytes in use, of capacity().
	[[nodiscard]] int32_t used() const { return used_; }
	[[nodiscard]] int32_t capacity() const { return capacity_; }
	/// Whether a piece is open for records.
	[[nodiscard]] bool logging() const { return open_ && !cut_; }

	/// Forgets every piece.
	void clear();

	static constexpr uint8_t kSoundPathTag = 0x80;
	static constexpr uint8_t kSoundDroppedTag = 0x81;
	static constexpr uint8_t kPickTag = 0x90;
	static constexpr uint8_t kSnapshotTag = 0x91;

private:
	/// Each piece's first bytes.
	struct PieceInfo {
		int32_t bytes;
		int32_t endStep;
		int32_t cutAt;
		uint16_t headerBytes;
		PieceEnd ended;
		bool marked;
		PieceStart start;
	};

	[[nodiscard]] PieceInfo& newest() const;
	/// Piece `i`'s offset: each starts aligned for its PieceInfo.
	[[nodiscard]] int32_t offsetOf(int32_t i) const;
	static constexpr int32_t alignUp(int32_t offset) {
		constexpr auto a = static_cast<int32_t>(alignof(PieceInfo));
		return (offset + a - 1) & ~(a - 1);
	}
	/// Writes `length` bytes of a record stamped `step` from `data`, or cuts the piece at that step.
	bool write(int32_t step, const uint8_t* data, int32_t length);
	/// The bytes every record but a SoundDropped may fill.
	[[nodiscard]] int32_t room() const { return capacity_ - kDroppedReserve; }
	/// Writes a header record (its bytes from `data`, if not nullptr), or cuts the piece at step 0. @return where its
	/// bytes are, or nullptr.
	uint8_t* writeHeader(uint8_t tag, const void* data, int32_t length);
	/// The records' cut: the step `step` didn't fit, and its records go whole.
	void cut(int32_t step);
	/// A header that didn't fit: none of it stays, and the piece is cut at its step 0.
	void cutHeader();
	/// Writes a SoundDropped record for `part`, `delta` steps after the record before. The caller has made room.
	void writeDropped(uint32_t delta, uint8_t part);

	uint8_t* buffer_;
	int32_t capacity_;
	int32_t used_ = 0;
	int32_t pieces_ = 0;
	int32_t newest_ = 0; ///< The newest piece's offset.
	/// The current piece's state for writing: open, cut, its records' last step, and where that step's records began.
	bool open_ = false;
	bool cut_ = false;
	bool inRecords_ = false;
	int32_t lastStep_ = 0;
	int32_t stepFrom_ = 0;
	/// The step of the last record before lastStep_'s: a cut that drops lastStep_'s records goes back to it.
	int32_t stepBefore_ = 0;
	/// The step each part's newest SetSound landed on in the current piece, logged or refused (-1 for none, or once
	/// dropped), for appendSoundDropped(). beginPiece() sets it.
	int32_t landed_[kMaxParts] = {};
	/// The parts whose SoundDropped sits on lastStep_, and the step each one's SetSound landed on: a cut of that step
	/// writes them again on the step before (cut()), so a later record can't take the drop and leave its SetSound.
	uint8_t dropsOnStep_ = 0;
	static_assert(kMaxParts <= 8, "dropsOnStep_ holds a bit a part");
	int32_t dropLanding_[kMaxParts] = {};
};

template <typename Found>
void SteeringLog::Piece::forEachHeader(Found found) const {
	const uint8_t* at = base_ + sizeof(PieceInfo);
	const uint8_t* end = base_ + headerBytes();
	while (at < end) {
		const uint8_t tag = at[0];
		const int32_t length = at[1] | at[2] << 8;
		found(tag, at + 3, length);
		at += 3 + length;
	}
}

} // namespace deluge::generative
