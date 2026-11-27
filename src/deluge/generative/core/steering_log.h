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

// The steering log (spec §17.2, §17.3): the pieces played in Perform, each a header and the steering that landed in it.
// The core appends commands where they land (ADR 0005); the Perform driver opens and ends pieces and adds what only it
// knows (the header's sound picks and param snapshot, a re-roll's paths). Capture (#145) reads it back.
//
// Each piece is an image: a PieceInfo, the header records, then its records in step order. Records are byte-packed
// (encodeCommand()): a command takes 4 bytes as a rule. The image streams to a store (Perform: a file on the card,
// spec §17.2) through RAM: the playing piece's opening and header stay whole in RAM (AS IT BEGAN reads them), and its
// records pass through a ring that a consumer outside the tick drains. A table in RAM knows every piece's info: the
// marked ones, then the last piece; one that ended unmarked leaves it when the next one begins.

#pragma once

#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/core/types.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>

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
	/// An Arc move (a force or End, or a transition) waits in its pending slot from this step: command, as it waits.
	/// The Arc's look-ahead reads it until it lands or stops waiting, so replay puts it back (spec §2.6, §17.2).
	Waiting,
	/// Nothing waits in command.kind's slot from this step (ForceSection for the force or End, Transition): what
	/// waited was cancelled. One that lands leaves its slot with its Command record.
	NotWaiting,
	/// Nothing happened: the piece got to this step (SteeringLog::kReachedTag). Readers never hand one over.
	Reached,
	/// A non-param setting's file path (oscillator sample/wavetable, SteeringLog::kSettingPathTag = 0x85):
	/// command.target its part, command.lane its Kit row (or Lane::None), command.length its OutputSetting id, `path`.
	SettingPath,
};
struct LogRecord {
	RecordKind kind = RecordKind::Command;
	int32_t step = 0;
	Command command;
	int32_t slot = 0;
	const char* path = nullptr; ///< Not terminated: `pathLength` chars.
	int32_t pathLength = 0;
};

/// Perform's log in RAM (spec §17.2, §20): the playing piece's opening and header, kept whole for AS IT BEGAN, and a
/// ring its records pass through on their way to the card. 16 KB between them, in SDRAM.
constexpr int32_t kLogHeaderBytes = 9 * 1024;
constexpr int32_t kLogRingBytes = 7 * 1024; ///< *tuning*: about 1,400 busy steps of card stall
constexpr int32_t kPerformLogBytes = kLogHeaderBytes + kLogRingBytes;
/// Pieces the log knows at once: the marked ones, the last one, and those still on their way to the store.
constexpr int32_t kMaxLogPieces = 32;

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

/// A piece's first bytes, in RAM and in its image (its file on the card): what it is and how far it got.
struct PieceInfo {
	static constexpr uint32_t kMagic = 0x474C4F47; ///< "GOLG"
	/// Goes up whenever the image's layout or a record's encoding changes: an image of another format can't be read.
	/// 4: kMaxParts raised from 6 to 7 (PieceStart's [kMaxParts] arrays and lane pick slots moved).
	static constexpr uint16_t kFormat = 4;
	uint32_t magic = kMagic;
	uint16_t format = kFormat;
	uint8_t genre = 0;         ///< In an image: start.recipe.genre's index in allGenres() (the pointer isn't stored).
	uint16_t genreVersion = 0; ///< In an image: that genre's version, which a retune moves.
	uint32_t id = 0;           ///< Counts the pieces logged: its file's number.
	int32_t bytes = 0;         ///< The image's bytes: this, the header records, the records.
	int32_t endStep = -1;
	int32_t cutAt = -1;
	int32_t records = 0;
	uint16_t headerBytes = 0;
	PieceEnd ended = PieceEnd::Playing;
	bool marked = false;
	PieceStart start;
};

/// The bytes a consumer stores next (SteeringLog::nextChunk()): `length` bytes of piece `id`'s image from `offset`.
struct LogChunk {
	int32_t slot = -1;
	uint32_t id = 0;
	int32_t offset = 0;
	const uint8_t* data = nullptr; ///< nullptr: the piece was thrown away, so its bytes need no storing (skip them).
	int32_t length = 0;
};

class SteeringLog {
public:
	/// The buffer a log over `headerBytes` and `ringBytes` takes: its piece table, then the two.
	static constexpr int32_t bytesFor(int32_t headerBytes, int32_t ringBytes) {
		return static_cast<int32_t>(sizeof(Slot)) * kMaxLogPieces + headerBytes + ringBytes;
	}
	/// Empty, over nothing: logs nothing.
	SteeringLog() = default;
	/// Over `buffer` (bytesFor() bytes, aligned for a pointer). Empty.
	SteeringLog(uint8_t* buffer, int32_t headerBytes, int32_t ringBytes);

	// ---- Pieces (spec §17.3): the Perform driver, in the tick ----

	/// A piece starts, from `start`. Every piece that ended and isn't marked is thrown away first (spec §17.3,
	/// §17.4): it leaves the table here, and its file stays on the card until a clean (spec §17.4). No piece opens,
	/// and nothing logs until the next one, if the table is full or the last header is still on its way out.
	void beginPiece(const PieceStart& start);
	/// Header records, after beginPiece() and before the piece's first record. A sound pick, by pickSlot(): an empty
	/// path is skipped.
	void addPick(int32_t slot, const char* path);
	/// A block of the param snapshot (spec §17.2), in the driver's own layout. Up to 65,535 bytes.
	void addSnapshot(const void* data, int32_t length);
	/// As addSnapshot(), for a caller that writes the block in place: its `length` bytes, to fill before anything
	/// else is logged, or nullptr where addSnapshot() would have cut the piece.
	uint8_t* snapshotSpace(int32_t length);
	/// A block of non-param sound-editor settings for an edited Output (kSettingsTag = 0x92, spec §17.2).
	void addSettings(const void* data, int32_t length);
	/// As addSettings(), for a caller that writes the block in place.
	uint8_t* settingsSpace(int32_t length);
	/// The current piece ends before `step`: its first step not played. Everything it logged may go to the store.
	void endPiece(int32_t step, PieceEnd why);
	/// The driver is about to decide `step`: every record before it is final, and may go to the store (spec §17.2).
	void commitBefore(int32_t step);
	/// Marks the current piece to keep, or unmarks it (spec §17.4). @return false, changing nothing, with no current
	/// piece, or for a mark while the log is full(), unless the piece was marked when it was cut (a second press puts
	/// back the mark the first took off).
	bool mark(bool on);
	/// Marks piece `i` or unmarks it: the current one as mark() does; an earlier one (the CAPTURE menu) stays in the
	/// log until the next piece begins, so it can be marked again until then, full or not. @return false if refused.
	bool markPiece(int32_t i, bool on);

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
	/// An Arc move's pending slot changed on `step`: `command` waits there now (RecordKind::Waiting), or, with `waits`
	/// false, nothing waits in its slot (RecordKind::NotWaiting).
	bool appendWaiting(int32_t step, const Command& command, bool waits);
	/// A non-param setting's file path (oscillator sample or wavetable, kSettingPathTag = 0x85), logged on `step`.
	bool appendSettingPath(int32_t step, uint8_t part, Lane lane, uint16_t setting, const char* path);

	// ---- The store's side: a consumer that never runs in the tick (firmware: a main-loop task, spec §17.2) ----
	//
	// The tick may run inside the consumer's card calls, never between them. So a chunk's bytes stay put until took()
	// (the ring's room isn't reused, and no piece opens over a header not yet taken), and a slot stays until
	// released().

	/// The next bytes to store, at most `maxBytes`: the oldest piece's, in image order. @return false with none ready
	/// (the playing piece's header goes once whole, its records once their step is past, commitBefore()).
	bool nextChunk(LogChunk& out, int32_t maxBytes) const;
	/// `chunk` is stored (or skipped): its room is free.
	void took(const LogChunk& chunk);
	/// Bytes of `slot`'s image ready to take now (final, not taken yet).
	[[nodiscard]] int32_t ready(int32_t slot) const;
	/// Whether `slot`'s piece is still being written (it plays).
	[[nodiscard]] bool growing(int32_t slot) const { return slot == newest_ && open_; }
	/// Whether `slot` is the newest piece (playing, or stopped as the last piece).
	[[nodiscard]] bool isNewest(int32_t slot) const { return slot == newest_; }
	/// Whether `slot`'s piece has ended and every byte of it was taken: its file can be closed.
	[[nodiscard]] bool allTaken(int32_t slot) const;
	/// Goes up whenever a slot's info changes (a mark, an end, a cut): the consumer writes it into the file again.
	[[nodiscard]] uint32_t infoVersion(int32_t slot) const { return slots_[slot].version; }
	/// `slot`'s info as its image holds it (the genre as its index).
	void imageInfo(int32_t slot, PieceInfo& out) const;
	/// Whether `slot` holds a piece thrown away (it needs no file), or one still in the table.
	[[nodiscard]] bool dropped(int32_t slot) const { return slots_[slot].state == Slot::Dropped; }
	[[nodiscard]] bool used(int32_t slot) const { return slots_[slot].state != Slot::Free; }
	/// The consumer is done with a thrown-away piece: its slot may hold another.
	void released(int32_t slot);
	/// Storing `slot`'s piece failed (a card error, a full card, the card pulled): if it is the piece playing, it is
	/// cut as on a full log (spec §17.2). What reached the store is still read back up to its last whole step.
	void storeFailed(int32_t slot);
	/// A piece the store kept from before (a marked file found at entry, spec §17.4): ended, marked, in the table
	/// before any piece of this run. @return its slot, or -1 if the table is full.
	int32_t adopt(const PieceInfo& info);
	/// The next piece's id: a consumer creates its file ahead (spec §17.2).
	[[nodiscard]] uint32_t nextId() const { return nextId_; }
	/// Ids go on from `id` (past every file the store holds).
	void continueIds(uint32_t id) { nextId_ = id; }
	/// `slot`'s id and how much of it was taken (its file's length).
	[[nodiscard]] uint32_t idOf(int32_t slot) const { return slots_[slot].info.id; }
	[[nodiscard]] int32_t taken(int32_t slot) const { return slots_[slot].taken; }
	/// `slot`'s opening and header records, in bytes.
	[[nodiscard]] int32_t headerBytes(int32_t slot) const { return slots_[slot].info.headerBytes; }
	/// Copies the bytes of `slot`'s image from `from` to its end that are still in RAM (not taken yet) into `out`:
	/// the harness reads a piece while it plays. @return the bytes copied.
	int32_t copyUntaken(int32_t slot, int32_t from, uint8_t* out) const;

	// ---- Reading ----

	/// What the table knows of a piece: its opening and how far it got.
	class PieceSummary {
	public:
		explicit PieceSummary(const PieceInfo& info) : info_(&info) {}
		[[nodiscard]] const PieceStart& start() const { return info_->start; }
		[[nodiscard]] PieceEnd ended() const { return info_->ended; }
		/// The step it ended before (endPiece()), or -1 while it plays.
		[[nodiscard]] int32_t endStep() const { return info_->endStep; }
		/// Where logging stopped because the log was full or the store failed: the first step not logged, or -1.
		[[nodiscard]] int32_t cutAt() const { return info_->cutAt; }
		/// The first step past its span (spec §17.2): where it ended, or where it was cut if that came first. -1
		/// while it plays uncut.
		[[nodiscard]] int32_t spanEnd() const;
		[[nodiscard]] bool marked() const { return info_->marked; }
		/// All its bytes, and the header's (the opening and the header records).
		[[nodiscard]] int32_t bytes() const { return info_->bytes; }
		/// Its records, counted as they are written.
		[[nodiscard]] int32_t recordCount() const { return info_->records; }
		[[nodiscard]] int32_t headerBytes() const { return info_->headerBytes; }
		[[nodiscard]] uint32_t id() const { return info_->id; }
		[[nodiscard]] const PieceInfo& info() const { return *info_; }

	protected:
		const PieceInfo* info_;
	};

	/// One piece's image, read in place: its info, header records and records (a file read back, or the playing
	/// piece's header in RAM, which has no records).
	class Piece : public PieceSummary {
	public:
		/// Over an image of `bytes`, aligned for PieceInfo, its info fixed by readImage().
		explicit Piece(const uint8_t* base) : PieceSummary(*reinterpret_cast<const PieceInfo*>(base)), base_(base) {}
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
		/// Hands each non-param settings header block (kSettingsTag = 0x92) to `found(data, length)`.
		template <typename Found>
		void forEachSettings(Found found) const {
			forEachHeader([&](uint8_t tag, const uint8_t* data, int32_t length) {
				if (tag == kSettingsTag) {
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
				if (record.kind != RecordKind::Reached) {
					found(record);
				}
			}
		}
		/// Reads the records one at a time, in order, as forEachRecord() hands them on: for a reader that stops between
		/// steps (Capture's replay, a chunk of steps a call). Valid while the image doesn't change.
		class Reader {
		public:
			/// Reads nothing.
			Reader() = default;
			/// The next record, without taking it. @return false past the last.
			bool peek(LogRecord& out) const {
				const uint8_t* at = at_;
				int32_t step = step_;
				while (at < end_) {
					at = decode(at, step, out);
					step = out.step;
					if (out.kind != RecordKind::Reached) {
						return true;
					}
				}
				return false;
			}
			/// Takes the next record. @return false past the last.
			bool next(LogRecord& out) {
				while (at_ < end_) {
					at_ = decode(at_, step_, out);
					step_ = out.step;
					if (out.kind != RecordKind::Reached) {
						return true;
					}
				}
				return false;
			}

		private:
			friend class Piece;
			Reader(const uint8_t* at, const uint8_t* end) : at_(at), end_(end) {}
			const uint8_t* at_ = nullptr;
			const uint8_t* end_ = nullptr;
			int32_t step_ = 0;
		};
		[[nodiscard]] Reader reader() const { return Reader(records(), base_ + bytes()); }

	private:
		friend class SteeringLog;
		template <typename Found>
		void forEachHeader(Found found) const;
		[[nodiscard]] const uint8_t* records() const { return base_ + headerBytes(); }
		static const uint8_t* decode(const uint8_t* at, int32_t step, LogRecord& out);
		/// Past the record at `at`, or nullptr if it runs past `end` (a record a store has only part of).
		static const uint8_t* recordEnd(const uint8_t* at, const uint8_t* end);
		const uint8_t* base_;
	};

	/// Makes `length` bytes read back from a store into an image a Piece can read, in place: the genre's pointer back
	/// from its index; a piece that never ended there (a power-off, a card error) ends at its last whole step, cut
	/// there (spec §17.2); a header not whole leaves nothing but the opening, cut at step 0. @return false for bytes
	/// that aren't an image of this format, or whose genre or its version this firmware doesn't have.
	static bool readImage(uint8_t* image, int32_t length);

	/// The pieces in the table, in set order: marked ones, then the current one.
	[[nodiscard]] int32_t pieces() const { return pieces_; }
	[[nodiscard]] PieceSummary piece(int32_t i) const { return PieceSummary(slots_[slotOf(i)].info); }
	/// Piece `i`'s slot (the consumer's and the store's handle on it).
	[[nodiscard]] int32_t slotOf(int32_t i) const;
	/// The newest piece's header, read in place (spec §13.2's AS IT BEGAN), if the last beginPiece() opened it and
	/// its header is whole: the piece playing, or the last one played. -1 if the last one found no room to open, or
	/// its header was cut, and once cleared.
	[[nodiscard]] int32_t newestWithHeader() const { return newestWithHeader_; }
	/// The newest piece's opening and header records (newestWithHeader() >= 0): it has no records.
	[[nodiscard]] Piece newestHeader() const { return Piece(header_); }
	/// Counts the pieces opened: a new piece can replace a dropped one and keep pieces() and the marks the same.
	[[nodiscard]] uint32_t opened() const { return opened_; }
	/// The current piece, the newest: playing, or the last piece after it ended. -1 before any, or when the newest
	/// found no room to open (the pieces left are marked ones, which a RECORD press doesn't touch).
	[[nodiscard]] int32_t current() const { return current_ ? pieces_ - 1 : -1; }
	/// The pieces marked, the current one among them.
	[[nodiscard]] int32_t markedPieces() const;
	/// Whether the newest piece stopped logging because the log was full or the store failed, or found no room to
	/// open: new marks are refused (spec §17.2).
	[[nodiscard]] bool full() const { return cut_; }
	/// Whether the store failed since the last piece began (its card error is the store's to show).
	[[nodiscard]] bool storeFailing() const { return storeFailed_; }
	/// The ring's bytes not yet taken, of ringCapacity().
	[[nodiscard]] int32_t used() const { return static_cast<int32_t>(head_ - tail_); }
	[[nodiscard]] int32_t ringCapacity() const { return ringBytes_; }
	/// Whether a piece is open for records.
	[[nodiscard]] bool logging() const { return open_ && !cut_; }
	/// Forgets every piece.
	void clear();

	static constexpr uint8_t kSoundPathTag = 0x80;
	static constexpr uint8_t kSoundDroppedTag = 0x81;
	static constexpr uint8_t kWaitingTag = 0x82;
	static constexpr uint8_t kNotWaitingTag = 0x83;
	/// Nothing happened: the piece got this far (commitBefore()). Readers skip it.
	static constexpr uint8_t kReachedTag = 0x84;
	/// A non-param setting's file path (oscillator sample or wavetable).
	static constexpr uint8_t kSettingPathTag = 0x85;
	/// A quiet piece says how far it got about once a bar.
	static constexpr int32_t kReachedEvery = 16;
	static constexpr uint8_t kPickTag = 0x90;
	static constexpr uint8_t kSnapshotTag = 0x91;
	static constexpr uint8_t kSettingsTag = 0x92;

private:
	/// A piece in the table, and how far the consumer has taken it.
	struct Slot {
		enum State : uint8_t { Free, Live, Dropped };
		PieceInfo info;
		State state = Free;
		uint32_t version = 0;
		uint32_t ringFrom = 0; ///< Where its records start in the ring (head_'s count).
		int32_t taken = 0;     ///< Image bytes the consumer took.
		uint32_t seq = 0;      ///< Set order.
	};
	static_assert(std::is_trivially_copyable_v<PieceInfo>, "an image's info is stored and read back as bytes");
	static_assert(offsetof(PieceInfo, magic) == 0 && offsetof(PieceInfo, format) == 4,
	              "magic and format stay at fixed offsets so readImage() rejects older formats cleanly");
	[[nodiscard]] Slot& newest() const { return slots_[newest_]; }
	/// Writes `length` bytes of a record stamped `step` from `data`, or cuts the piece at that step.
	bool write(int32_t step, const uint8_t* data, int32_t length);
	/// Puts `length` bytes at the ring's head.
	void put(const uint8_t* data, int32_t length);
	/// The ring's room for every record but a SoundDropped.
	[[nodiscard]] int32_t room() const { return ringBytes_ - kDroppedReserve - used(); }
	/// Writes a header record (its bytes from `data`, if not nullptr), or cuts the piece at step 0. @return where its
	/// bytes are, or nullptr.
	uint8_t* writeHeader(uint8_t tag, const void* data, int32_t length);
	/// The header is whole: its bytes may go to the store.
	void headerDone();
	/// Throws away every piece that isn't marked: out of the table, its slot the consumer's until released().
	void dropUnmarked();
	/// The records' cut: the step `step` didn't fit, and its records go whole.
	void cut(int32_t step);
	/// A header that didn't fit: none of it stays, and the piece is cut at its step 0.
	void cutHeader();
	/// Writes a SoundDropped record for `part`, `delta` steps after the record before. The caller has made room.
	void writeDropped(uint32_t delta, uint8_t part);
	/// The slot the newest's info changed in: the consumer writes it again.
	void touched(Slot& slot) { slot.version++; }

	Slot* slots_ = nullptr;
	uint8_t* header_ = nullptr; ///< The playing piece's opening (a PieceInfo copy) and header records.
	uint8_t* ring_ = nullptr;
	int32_t headerCapacity_ = 0;
	int32_t ringBytes_ = 0;
	int32_t pieces_ = 0;
	uint32_t seq_ = 0;
	uint32_t opened_ = 0;
	uint32_t nextId_ = 1;
	int32_t newest_ = -1;      ///< The newest piece's slot.
	int32_t headerOwner_ = -1; ///< The slot whose header is in header_.
	bool headerWhole_ = false; ///< Its header may go to the store.
	int32_t newestWithHeader_ = -1;
	// The ring: head_ written, commit_ final, tail_ taken (counts of bytes, wrapping at 2^32).
	uint32_t head_ = 0;
	uint32_t commit_ = 0;
	uint32_t tail_ = 0;
	/// The current piece's state for writing: open, cut, its records' last step, and where that step's records began.
	bool open_ = false;
	bool cut_ = false;
	bool current_ = false;     ///< The newest piece is the current one (current()).
	bool markedAtCut_ = false; ///< The current piece was marked when the log cut it: a mark may come back.
	bool storeFailed_ = false;
	int32_t recordsFrom_ = 0; ///< The newest piece's records before lastStep_'s.
	bool inRecords_ = false;
	int32_t lastStep_ = 0;
	uint32_t stepFrom_ = 0; ///< The ring's head where lastStep_'s records began.
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
