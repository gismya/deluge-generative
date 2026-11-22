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

#include "generative/core/steering_log.h"
#include "generative/core/genre.h"
#include <algorithm>
#include <cstring>
#include <span>

namespace deluge::generative {

namespace {

static_assert(static_cast<int32_t>(kLastCommandKind) < 0x40, "a kind fits the tag's low 6 bits");
constexpr uint8_t kExtended = 0x40; ///< The command's length and lane follow.
constexpr int32_t kMaxPathRecord = 1 + 5 + 4 + kMaxSoundPath;
constexpr int32_t kMaxSettingPathRecord = 1 + 5 + 1 + 1 + 3 + 1 + kMaxSoundPath;

GEN_STEP_CODE uint8_t* putVarint(uint8_t* out, uint32_t value) {
	while (value >= 0x80) {
		*out++ = static_cast<uint8_t>(value | 0x80);
		value >>= 7;
	}
	*out++ = static_cast<uint8_t>(value);
	return out;
}

/// The bytes putVarint() writes for `value`.
constexpr int32_t varintLength(uint32_t value) {
	int32_t bytes = 1;
	while (value >= 0x80) {
		value >>= 7;
		bytes++;
	}
	return bytes;
}
static_assert(2 + varintLength(UINT32_MAX) == kMaxDroppedBytes, "a SoundDropped record at its widest");

const uint8_t* getVarint(const uint8_t* at, uint32_t& value) {
	value = 0;
	for (int32_t shift = 0;; shift += 7) {
		const uint8_t byte = *at++;
		value |= static_cast<uint32_t>(byte & 0x7F) << shift;
		if ((byte & 0x80) == 0) {
			return at;
		}
	}
}

int32_t pathLength(const char* path) {
	int32_t length = 0;
	while (length < kMaxSoundPath - 1 && path[length] != 0) {
		length++;
	}
	return length;
}

} // namespace

GEN_STEP_CODE int32_t encodeCommand(const Command& command, uint32_t delta, uint8_t* out) {
	const bool extended = command.length != 0 || command.lane != Lane::None;
	uint8_t* at = out;
	*at++ = static_cast<uint8_t>(static_cast<uint8_t>(command.kind) | (extended ? kExtended : 0));
	at = putVarint(at, delta);
	*at++ = command.target;
	const auto value = static_cast<uint32_t>(command.value);
	at = putVarint(at, (value << 1) ^ static_cast<uint32_t>(command.value >> 31));
	if (extended) {
		at = putVarint(at, command.length);
		*at++ = static_cast<uint8_t>(command.lane);
	}
	return static_cast<int32_t>(at - out);
}

// ---- Pieces ----

SteeringLog::SteeringLog(uint8_t* buffer, int32_t headerBytes, int32_t ringBytes) {
	slots_ = reinterpret_cast<Slot*>(buffer);
	const Slot empty{};
	for (int32_t s = 0; s < kMaxLogPieces; s++) {
		std::memcpy(static_cast<void*>(&slots_[s]), &empty, sizeof empty); // raw memory: Slot is trivially copyable
	}
	header_ = buffer + static_cast<int32_t>(sizeof(Slot)) * kMaxLogPieces;
	ring_ = header_ + headerBytes;
	headerCapacity_ = headerBytes;
	ringBytes_ = ringBytes;
}

void SteeringLog::beginPiece(const PieceStart& start) {
	if (slots_ == nullptr) {
		return;
	}
	if (newest_ >= 0 && current_ && newest().info.ended == PieceEnd::Playing) {
		endPiece(lastStep_ + 1, PieceEnd::Stopped);
	}
	dropUnmarked(); // the last piece, and any unmarked in the CAPTURE menu (spec §17.3, §17.4)
	opened_++;
	open_ = false;
	newestWithHeader_ = -1;
	current_ = false;
	storeFailed_ = false;
	int32_t free = -1;
	for (int32_t s = 0; s < kMaxLogPieces && free < 0; s++) {
		free = slots_[s].state == Slot::Free ? s : -1;
	}
	// The header in RAM still on its way to the store, for a piece kept: a new header can't go over it (a piece that
	// lasted less than a store's write). One thrown away needs it no more.
	const bool headerBusy = headerOwner_ >= 0 && slots_[headerOwner_].state == Slot::Live
	                        && slots_[headerOwner_].taken < slots_[headerOwner_].info.headerBytes;
	if (free < 0 || headerBusy || headerCapacity_ < static_cast<int32_t>(sizeof(PieceInfo))) {
		// No room to open: nothing logs, and marks are refused (spec §17.2).
		newest_ = -1;
		cut_ = true;
		return;
	}
	Slot& slot = slots_[free];
	slot = Slot{};
	slot.state = Slot::Live;
	slot.seq = ++seq_;
	slot.ringFrom = head_;
	PieceInfo& info = slot.info;
	info.id = nextId_++;
	info.bytes = sizeof(PieceInfo);
	info.headerBytes = sizeof(PieceInfo);
	info.start = start;
	std::memcpy(header_, &info, sizeof info);
	pieces_++;
	newest_ = free;
	headerOwner_ = free;
	headerWhole_ = false;
	newestWithHeader_ = pieces_ - 1;
	open_ = true;
	cut_ = false;
	current_ = true;
	markedAtCut_ = false;
	recordsFrom_ = 0;
	inRecords_ = false;
	lastStep_ = 0;
	stepBefore_ = 0;
	dropsOnStep_ = 0;
	stepFrom_ = head_;
	for (int32_t& landed : landed_) {
		landed = -1;
	}
}

void SteeringLog::headerDone() {
	if (newest_ >= 0 && headerOwner_ == newest_) {
		headerWhole_ = true;
	}
}

uint8_t* SteeringLog::writeHeader(uint8_t tag, const void* data, int32_t length) {
	if (!open_ || cut_ || inRecords_ || headerWhole_ || length > 0xFFFF) {
		return nullptr;
	}
	PieceInfo& info = newest().info;
	if (info.headerBytes + 3 + length > headerCapacity_) {
		cutHeader();
		return nullptr;
	}
	uint8_t* at = header_ + info.headerBytes;
	at[0] = tag;
	at[1] = static_cast<uint8_t>(length);
	at[2] = static_cast<uint8_t>(length >> 8);
	if (data != nullptr) {
		std::memcpy(at + 3, data, static_cast<size_t>(length));
	}
	info.headerBytes = static_cast<uint16_t>(info.headerBytes + 3 + length);
	info.bytes = info.headerBytes;
	// The copy in RAM reads as the header alone (newestHeader()).
	auto* copy = reinterpret_cast<PieceInfo*>(header_);
	copy->headerBytes = info.headerBytes;
	copy->bytes = info.headerBytes;
	return at + 3;
}

void SteeringLog::addPick(int32_t slot, const char* path) {
	if (path == nullptr || path[0] == 0 || slot < 0 || slot >= kPickSlots) {
		return;
	}
	uint8_t record[kMaxSoundPath];
	const int32_t length = pathLength(path);
	record[0] = static_cast<uint8_t>(slot);
	std::memcpy(record + 1, path, static_cast<size_t>(length));
	writeHeader(kPickTag, record, 1 + length);
}

void SteeringLog::addSnapshot(const void* data, int32_t length) {
	writeHeader(kSnapshotTag, data, length);
}

uint8_t* SteeringLog::snapshotSpace(int32_t length) {
	return writeHeader(kSnapshotTag, nullptr, length);
}

void SteeringLog::addSettings(const void* data, int32_t length) {
	writeHeader(kSettingsTag, data, length);
}

uint8_t* SteeringLog::settingsSpace(int32_t length) {
	return writeHeader(kSettingsTag, nullptr, length);
}

void SteeringLog::endPiece(int32_t step, PieceEnd why) {
	if (!open_) {
		return;
	}
	Slot& slot = newest();
	slot.info.endStep = step;
	slot.info.ended = why;
	open_ = false;
	headerDone();
	commit_ = head_;
	touched(slot);
}

GEN_STEP_CODE void SteeringLog::commitBefore(int32_t step) {
	if (newest_ < 0) {
		commit_ = head_;
		return;
	}
	headerDone(); // the opening's header records come before step 0's decision
	if (logging() && step - lastStep_ > kReachedEvery && step > 0) {
		// A quiet stretch: say how far the piece got, so a piece read back after a power-off ends there, not at its
		// last steering (readImage()). Never cuts: with no room it is left out.
		uint8_t record[1 + 5];
		uint8_t* at = record;
		*at++ = kReachedTag;
		at = putVarint(at, static_cast<uint32_t>(step - 1 - lastStep_));
		const auto length = static_cast<int32_t>(at - record);
		if (length <= room()) {
			stepBefore_ = lastStep_;
			stepFrom_ = head_;
			recordsFrom_ = newest().info.records;
			dropsOnStep_ = 0;
			put(record, length);
			newest().info.bytes += length;
			inRecords_ = true;
			lastStep_ = step - 1;
		}
	}
	commit_ = open_ && inRecords_ && lastStep_ >= step ? stepFrom_ : head_;
}

bool SteeringLog::mark(bool on) {
	return markPiece(current(), on);
}

bool SteeringLog::markPiece(int32_t i, bool on) {
	if (i < 0 || i >= pieces_) {
		return false;
	}
	if (i == current() && on && cut_ && !markedAtCut_) {
		return false; // a full log refuses a new mark (spec §17.2); one the cut found stays the player's to put back
	}
	Slot& slot = slots_[slotOf(i)];
	if (slot.info.marked != on) {
		slot.info.marked = on;
		touched(slot);
	}
	return true;
}

int32_t SteeringLog::markedPieces() const {
	int32_t marked = 0;
	for (int32_t s = 0; s < kMaxLogPieces && slots_ != nullptr; s++) {
		marked += slots_[s].state == Slot::Live && slots_[s].info.marked ? 1 : 0;
	}
	return marked;
}

void SteeringLog::dropUnmarked() {
	// Pieces only ever end before the next begins, so every piece here has ended. A dropped one's slot stays the
	// consumer's until it lets go (released()); its bytes still in RAM it skips.
	for (int32_t s = 0; s < kMaxLogPieces; s++) {
		if (slots_[s].state == Slot::Live && !slots_[s].info.marked) {
			slots_[s].state = Slot::Dropped;
			pieces_--;
		}
	}
	newest_ = -1;
}

void SteeringLog::cutHeader() {
	// A piece with half a snapshot can't be built: none of the header stays (spec §17.2).
	Slot& slot = newest();
	PieceInfo& info = slot.info;
	markedAtCut_ = info.marked;
	info.headerBytes = sizeof(PieceInfo);
	info.bytes = sizeof(PieceInfo);
	info.cutAt = 0;
	auto* copy = reinterpret_cast<PieceInfo*>(header_);
	copy->headerBytes = info.headerBytes;
	copy->bytes = info.headerBytes;
	headerWhole_ = true;
	cut_ = true;
	newestWithHeader_ = -1;
	touched(slot);
}

GEN_STEP_CODE void SteeringLog::cut(int32_t step) {
	// The piece's span ends before `step`: that step's records go, so every logged step is whole (spec §17.2). Only
	// records not yet final can go: commitBefore() never passes the step being decided.
	Slot& slot = newest();
	PieceInfo& info = slot.info;
	markedAtCut_ = info.marked;
	if (static_cast<int32_t>(stepFrom_ - commit_) >= 0) {
		head_ = stepFrom_;
		info.records = recordsFrom_;
		lastStep_ = stepBefore_;
		info.bytes = info.headerBytes + static_cast<int32_t>(head_ - slot.ringFrom);
		// A swap dropped on that step whose SetSound landed before it stays said, on the step before.
		for (int32_t p = 0; p < kMaxParts; p++) {
			if ((dropsOnStep_ & (1 << p)) != 0 && dropLanding_[p] < step) {
				writeDropped(0, static_cast<uint8_t>(p));
			}
		}
	}
	else {
		step = lastStep_ + 1; // a guard: the step's records are already on their way, so they stay
	}
	dropsOnStep_ = 0;
	info.cutAt = step;
	cut_ = true;
	touched(slot);
}

GEN_STEP_CODE void SteeringLog::put(const uint8_t* data, int32_t length) {
	const auto at = static_cast<int32_t>(head_ % static_cast<uint32_t>(ringBytes_));
	const int32_t first = std::min(length, ringBytes_ - at);
	std::memcpy(ring_ + at, data, static_cast<size_t>(first));
	if (first < length) {
		std::memcpy(ring_, data + first, static_cast<size_t>(length - first));
	}
	head_ += static_cast<uint32_t>(length);
}

GEN_STEP_CODE void SteeringLog::writeDropped(uint32_t delta, uint8_t part) {
	uint8_t record[kMaxDroppedBytes];
	uint8_t* at = record;
	*at++ = kSoundDroppedTag;
	at = putVarint(at, delta);
	*at++ = part;
	const auto length = static_cast<int32_t>(at - record);
	put(record, length);
	newest().info.bytes += length;
	newest().info.records++;
}

GEN_STEP_CODE bool SteeringLog::write(int32_t step, const uint8_t* data, int32_t length) {
	if (!open_ || cut_) {
		return false;
	}
	headerDone();
	if (!inRecords_ || step != lastStep_) {
		stepBefore_ = lastStep_;
		stepFrom_ = head_;
		recordsFrom_ = newest().info.records;
		dropsOnStep_ = 0;
	}
	if (length > room()) {
		cut(step);
		return false;
	}
	put(data, length);
	newest().info.bytes += length;
	newest().info.records++;
	inRecords_ = true;
	lastStep_ = step;
	return true;
}

GEN_STEP_CODE bool SteeringLog::append(int32_t step, const Command& command) {
	if (!open_) {
		return false;
	}
	// A SetSound is its part's newest landing even when it doesn't fit: its step is then at or past the cut, so a
	// drop of it writes nothing, and none goes to the landing before it, which was heard.
	if (command.kind == CommandKind::SetSound && command.target < kMaxParts) {
		landed_[command.target] = step;
	}
	if (cut_) {
		return false;
	}
	uint8_t record[kMaxCommandBytes];
	const int32_t length = encodeCommand(command, static_cast<uint32_t>(step - lastStep_), record);
	return write(step, record, length);
}

bool SteeringLog::appendSoundPath(int32_t step, uint8_t part, uint8_t token, int32_t slot, const char* path) {
	if (!open_ || cut_) {
		return false;
	}
	uint8_t record[kMaxPathRecord];
	uint8_t* at = record;
	*at++ = kSoundPathTag;
	at = putVarint(at, static_cast<uint32_t>(step - lastStep_));
	*at++ = part;
	*at++ = token;
	*at++ = static_cast<uint8_t>(slot);
	const int32_t length = path != nullptr ? pathLength(path) : 0;
	*at++ = static_cast<uint8_t>(length);
	std::memcpy(at, path, static_cast<size_t>(length));
	at += length;
	return write(step, record, static_cast<int32_t>(at - record));
}

GEN_STEP_CODE bool SteeringLog::appendWaiting(int32_t step, const Command& command, bool waits) {
	if (!open_ || cut_) {
		return false;
	}
	uint8_t record[kMaxCommandBytes];
	uint8_t* at = record;
	*at++ = waits ? kWaitingTag : kNotWaitingTag;
	at = putVarint(at, static_cast<uint32_t>(step - lastStep_));
	*at++ = static_cast<uint8_t>(command.kind);
	if (waits) {
		*at++ = command.target;
		const auto value = static_cast<uint32_t>(command.value);
		at = putVarint(at, (value << 1) ^ static_cast<uint32_t>(command.value >> 31));
	}
	return write(step, record, static_cast<int32_t>(at - record));
}

bool SteeringLog::appendSettingPath(int32_t step, uint8_t part, Lane lane, uint16_t setting, const char* path) {
	if (!open_ || cut_) {
		return false;
	}
	uint8_t record[kMaxSettingPathRecord];
	uint8_t* at = record;
	*at++ = kSettingPathTag;
	at = putVarint(at, static_cast<uint32_t>(step - lastStep_));
	*at++ = part;
	*at++ = static_cast<uint8_t>(lane);
	at = putVarint(at, setting);
	const int32_t length = path != nullptr ? pathLength(path) : 0;
	*at++ = static_cast<uint8_t>(length);
	if (length > 0) {
		std::memcpy(at, path, static_cast<size_t>(length));
		at += length;
	}
	return write(step, record, static_cast<int32_t>(at - record));
}

bool SteeringLog::appendSoundDropped(int32_t step, uint8_t part) {
	if (!open_ || part >= kMaxParts || landed_[part] < 0) {
		return false;
	}
	Slot& slot = newest();
	PieceInfo& info = slot.info;
	const int32_t landed = landed_[part];
	if (cut_ && landed >= info.cutAt) {
		return false; // its SetSound went with the cut: nothing logged says it played
	}
	// After the SetSound and every record since; inside the span when the log was cut since.
	int32_t stamp = std::max(step, lastStep_);
	if (cut_) {
		stamp = std::max(std::min(stamp, info.cutAt - 1), lastStep_);
	}
	const int32_t length = 2 + varintLength(static_cast<uint32_t>(stamp - lastStep_)); // the tag, delta and part
	if (length > ringBytes_ - used()) {
		// A guard: with one drop per landing the reserve can't run out (no SetSound lands once the records' room is
		// gone, so at most one a part waits for its drop). Were it to, nothing more logs; nothing is rewound, so the
		// drops already written stay.
		if (!cut_) {
			info.cutAt = lastStep_ + 1;
			cut_ = true;
			markedAtCut_ = info.marked;
			touched(slot);
		}
		return false;
	}
	headerDone();
	if (stamp != lastStep_) {
		stepBefore_ = lastStep_;
		stepFrom_ = head_;
		recordsFrom_ = info.records;
		dropsOnStep_ = 0;
	}
	writeDropped(static_cast<uint32_t>(stamp - lastStep_), part);
	inRecords_ = true;
	lastStep_ = stamp;
	dropsOnStep_ |= static_cast<uint8_t>(1 << part);
	dropLanding_[part] = landed;
	landed_[part] = -1; // one drop per landing: the reserve holds one per part
	return true;
}

// ---- The store's side ----

bool SteeringLog::nextChunk(LogChunk& out, int32_t maxBytes) const {
	// The oldest piece not wholly taken goes first: the ring holds pieces' records in set order.
	int32_t oldest = -1;
	for (int32_t s = 0; s < kMaxLogPieces && slots_ != nullptr; s++) {
		const Slot& slot = slots_[s];
		if (slot.state != Slot::Free && !allTaken(s) && (oldest < 0 || slot.seq < slots_[oldest].seq)) {
			oldest = s;
		}
	}
	if (oldest < 0 || maxBytes <= 0) {
		return false;
	}
	const Slot& slot = slots_[oldest];
	const bool skip = slot.state == Slot::Dropped;
	const bool growing = oldest == newest_ && open_;
	out = LogChunk{oldest, slot.info.id, slot.taken, nullptr, 0};
	const int32_t headerBytes = slot.info.headerBytes;
	if (slot.taken < headerBytes) {
		if (growing && !headerWhole_) {
			return false; // the header isn't whole yet
		}
		// The opening alone first, so the consumer can store imageInfo() in its place.
		const int32_t end = slot.taken < static_cast<int32_t>(sizeof(PieceInfo))
		                        ? static_cast<int32_t>(sizeof(PieceInfo))
		                        : headerBytes;
		out.length = std::min(maxBytes, end - slot.taken);
		out.data = skip || headerOwner_ != oldest ? nullptr : header_ + slot.taken;
		return true;
	}
	const uint32_t from = slot.ringFrom + static_cast<uint32_t>(slot.taken - headerBytes);
	const uint32_t to = growing ? commit_ : slot.ringFrom + static_cast<uint32_t>(slot.info.bytes - headerBytes);
	if (static_cast<int32_t>(to - from) <= 0) {
		return false;
	}
	const auto at = static_cast<int32_t>(from % static_cast<uint32_t>(ringBytes_));
	out.length = std::min({maxBytes, static_cast<int32_t>(to - from), ringBytes_ - at});
	out.data = skip ? nullptr : ring_ + at;
	return true;
}

void SteeringLog::took(const LogChunk& chunk) {
	Slot& slot = slots_[chunk.slot];
	if (chunk.offset >= slot.info.headerBytes) {
		tail_ += static_cast<uint32_t>(chunk.length);
	}
	slot.taken += chunk.length;
}

int32_t SteeringLog::ready(int32_t slot) const {
	const Slot& s = slots_[slot];
	const bool growing = slot == newest_ && open_;
	int32_t out = 0;
	if (s.taken < s.info.headerBytes && !(growing && !headerWhole_)) {
		out += s.info.headerBytes - s.taken;
	}
	if (s.taken >= s.info.headerBytes || !(growing && !headerWhole_)) {
		const uint32_t from = s.ringFrom + static_cast<uint32_t>(std::max<int32_t>(0, s.taken - s.info.headerBytes));
		const uint32_t to = growing ? commit_ : s.ringFrom + static_cast<uint32_t>(s.info.bytes - s.info.headerBytes);
		out += std::max<int32_t>(0, static_cast<int32_t>(to - from));
	}
	return out;
}

bool SteeringLog::allTaken(int32_t slot) const {
	const Slot& s = slots_[slot];
	const bool growing = slot == newest_ && open_;
	return !growing && s.taken >= s.info.bytes;
}

void SteeringLog::imageInfo(int32_t slot, PieceInfo& out) const {
	out = slots_[slot].info;
	// What reached the store: a step's end, as every commit is (readImage() trusts the file up to here).
	out.bytes = std::min(out.bytes, std::max<int32_t>(slots_[slot].taken, sizeof(PieceInfo)));
	const Genre* genre = out.start.recipe.genre;
	out.genre = 0;
	out.genreVersion = 0;
	const std::span<const Genre* const> genres = allGenres();
	for (size_t g = 0; g < genres.size(); g++) {
		if (genres[g] == genre) {
			out.genre = static_cast<uint8_t>(g);
			out.genreVersion = genre->version;
		}
	}
	out.start.recipe.genre = nullptr;
}

void SteeringLog::released(int32_t slot) {
	if (slots_[slot].state == Slot::Dropped) {
		slots_[slot].state = Slot::Free;
	}
}

void SteeringLog::storeFailed(int32_t slot) {
	if (slot != newest_) {
		return; // a closed-file rewrite failure on an adopted/dropped piece doesn't poison the playing piece
	}
	storeFailed_ = true;
	if (open_ && !cut_) {
		Slot& s = newest();
		markedAtCut_ = s.info.marked;
		s.info.cutAt = inRecords_ ? lastStep_ + 1 : 0;
		cut_ = true;
		touched(s);
	}
}

int32_t SteeringLog::adopt(const PieceInfo& info) {
	for (int32_t s = 0; s < kMaxLogPieces && slots_ != nullptr; s++) {
		Slot& slot = slots_[s];
		if (slot.state == Slot::Free) {
			slot = Slot{};
			slot.state = Slot::Live;
			slot.seq = ++seq_;
			slot.info = info;
			slot.info.marked = true;
			slot.taken = info.bytes; // on the card already
			slot.ringFrom = head_;
			pieces_++;
			nextId_ = std::max(nextId_, info.id + 1);
			return s;
		}
	}
	return -1;
}

int32_t SteeringLog::copyUntaken(int32_t slot, int32_t from, uint8_t* out) const {
	const Slot& s = slots_[slot];
	int32_t copied = 0;
	for (int32_t at = std::max(from, s.taken); at < s.info.bytes; at++) {
		if (at < s.info.headerBytes) {
			out[copied++] = headerOwner_ == slot ? header_[at] : 0;
		}
		else {
			const uint32_t pos = s.ringFrom + static_cast<uint32_t>(at - s.info.headerBytes);
			out[copied++] = ring_[pos % static_cast<uint32_t>(ringBytes_)];
		}
	}
	return copied;
}

// ---- Reading ----

int32_t SteeringLog::slotOf(int32_t i) const {
	for (int32_t s = 0; s < kMaxLogPieces; s++) {
		if (slots_[s].state != Slot::Live) {
			continue;
		}
		int32_t before = 0;
		for (int32_t t = 0; t < kMaxLogPieces; t++) {
			before += slots_[t].state == Slot::Live && slots_[t].seq < slots_[s].seq ? 1 : 0;
		}
		if (before == i) {
			return s;
		}
	}
	return 0; // callers ask for one of pieces()
}

void SteeringLog::clear() {
	for (int32_t s = 0; s < kMaxLogPieces && slots_ != nullptr; s++) {
		slots_[s] = Slot{};
	}
	pieces_ = 0;
	newest_ = -1;
	headerOwner_ = -1;
	headerWhole_ = false;
	newestWithHeader_ = -1;
	head_ = commit_ = tail_ = 0;
	open_ = false;
	cut_ = false;
	current_ = false;
	markedAtCut_ = false;
	storeFailed_ = false;
	inRecords_ = false;
}

int32_t SteeringLog::PieceSummary::spanEnd() const {
	const int32_t cut = cutAt();
	const int32_t end = endStep();
	return cut >= 0 && (end < 0 || cut < end) ? cut : end;
}

bool SteeringLog::readImage(uint8_t* image, int32_t length) {
	if (length < static_cast<int32_t>(sizeof(PieceInfo))) {
		return false;
	}
	auto* info = reinterpret_cast<PieceInfo*>(image);
	const std::span<const Genre* const> genres = allGenres();
	if (info->magic != PieceInfo::kMagic || info->format != PieceInfo::kFormat || info->genre >= genres.size()
	    || genres[info->genre]->version != info->genreVersion) {
		return false;
	}
	info->start.recipe.genre = genres[info->genre];
	if (info->ended != PieceEnd::Playing && info->bytes == length) {
		return true; // whole
	}
	// It never ended there, or less of it got there than its info says: it ends at its last whole step (spec §17.2).
	if (info->headerBytes < static_cast<int32_t>(sizeof(PieceInfo)) || info->headerBytes > length) {
		info->headerBytes = sizeof(PieceInfo);
		info->bytes = sizeof(PieceInfo);
		info->records = 0;
		info->cutAt = 0;
	}
	else {
		// Up to `known` (its info's bytes, written at its last flush) the file ends on a step's end; past it a step
		// may be torn, so the last one there goes unless a Reached record closes it.
		const int32_t known = std::clamp<int32_t>(info->bytes, info->headerBytes, length);
		const uint8_t* at = image + info->headerBytes;
		const uint8_t* end = image + length;
		const uint8_t* stepStart = at; // where the last step's records began
		int32_t step = 0;
		int32_t lastStep = -1;
		int32_t records = 0;
		int32_t recordsBefore = 0;
		bool lastReached = false;
		while (at < end) {
			const uint8_t* next = Piece::recordEnd(at, end);
			if (next == nullptr) {
				break; // a record the store has only part of
			}
			LogRecord record;
			const uint8_t tag = at[0];
			Piece::decode(at, step, record);
			if (record.step != lastStep) {
				stepStart = at;
				recordsBefore = records;
				lastStep = record.step;
			}
			step = record.step;
			lastReached = tag == kReachedTag;
			records += lastReached ? 0 : 1;
			at = next;
		}
		if (lastStep < 0) {
			info->cutAt = 0; // nothing says it got past its step 0
			info->bytes = info->headerBytes;
			info->records = 0;
		}
		else if (lastReached || stepStart - image < known) {
			info->cutAt = lastStep + 1; // its last step is whole
			info->bytes = static_cast<int32_t>(at - image);
			info->records = records;
		}
		else {
			info->cutAt = lastStep; // the last step's records may not all be there: it goes whole
			info->bytes = static_cast<int32_t>(stepStart - image);
			info->records = recordsBefore;
		}
	}
	if (info->ended == PieceEnd::Playing || info->endStep < 0 || info->endStep > info->cutAt) {
		info->ended = PieceEnd::Stopped;
		info->endStep = info->cutAt;
	}
	return true;
}

bool SteeringLog::Piece::pick(int32_t slot, char* out) const {
	out[0] = 0;
	bool found = false;
	forEachHeader([&](uint8_t tag, const uint8_t* data, int32_t length) {
		if (tag == kPickTag && data[0] == slot) {
			std::memcpy(out, data + 1, static_cast<size_t>(length - 1));
			out[length - 1] = 0;
			found = true;
		}
	});
	return found;
}

const uint8_t* SteeringLog::Piece::decode(const uint8_t* at, int32_t step, LogRecord& out) {
	out = LogRecord{};
	const uint8_t tag = *at++;
	uint32_t delta = 0;
	at = getVarint(at, delta);
	out.step = step + static_cast<int32_t>(delta);
	if (tag == kReachedTag) {
		out.kind = RecordKind::Reached;
		return at;
	}
	if (tag == kSoundPathTag || tag == kSoundDroppedTag) {
		out.kind = tag == kSoundPathTag ? RecordKind::SoundPath : RecordKind::SoundDropped;
		out.command.kind = CommandKind::SetSound;
		out.command.target = *at++;
		if (tag == kSoundPathTag) {
			out.command.value = *at++;
			out.slot = *at++;
			out.pathLength = *at++;
			out.path = reinterpret_cast<const char*>(at);
			at += out.pathLength;
		}
		return at;
	}
	if (tag == kSettingPathTag) {
		out.kind = RecordKind::SettingPath;
		out.command.kind = CommandKind::SetSetting;
		out.command.target = *at++;
		out.command.lane = static_cast<Lane>(*at++);
		uint32_t setting = 0;
		at = getVarint(at, setting);
		out.command.length = static_cast<uint16_t>(setting);
		out.pathLength = *at++;
		out.path = reinterpret_cast<const char*>(at);
		at += out.pathLength;
		return at;
	}
	if (tag == kWaitingTag || tag == kNotWaitingTag) {
		out.kind = tag == kWaitingTag ? RecordKind::Waiting : RecordKind::NotWaiting;
		out.command.kind = static_cast<CommandKind>(*at++);
		if (tag == kNotWaitingTag) {
			return at;
		}
	}
	else {
		out.command.kind = static_cast<CommandKind>(tag & ~kExtended);
	}
	out.command.target = *at++;
	uint32_t zigzag = 0;
	at = getVarint(at, zigzag);
	out.command.value = static_cast<int32_t>((zigzag >> 1) ^ (0u - (zigzag & 1)));
	if (out.kind == RecordKind::Command && (tag & kExtended) != 0) {
		uint32_t length = 0;
		at = getVarint(at, length);
		out.command.length = static_cast<uint16_t>(length);
		out.command.lane = static_cast<Lane>(*at++);
	}
	return at;
}

const uint8_t* SteeringLog::Piece::recordEnd(const uint8_t* at, const uint8_t* end) {
	auto varint = [&](const uint8_t* p) -> const uint8_t* {
		for (int32_t i = 0; i < 5; i++) {
			if (p >= end) {
				return nullptr;
			}
			if ((*p++ & 0x80) == 0) {
				return p;
			}
		}
		return nullptr;
	};
	auto bytes = [&](const uint8_t* p, int32_t n) -> const uint8_t* {
		return p != nullptr && end - p >= n ? p + n : nullptr;
	};
	if (at >= end) {
		return nullptr;
	}
	const uint8_t tag = *at++;
	at = varint(at);
	if (at == nullptr) {
		return nullptr;
	}
	switch (tag) {
	case kReachedTag:
		return at;
	case kSoundDroppedTag:
		return bytes(at, 1);
	case kSoundPathTag: {
		const uint8_t* length = bytes(at, 3);
		return length != nullptr && length < end ? bytes(length + 1, *length) : nullptr;
	}
	case kSettingPathTag: {
		at = bytes(at, 2);                         // part, lane
		at = at != nullptr ? varint(at) : nullptr; // setting id
		return at != nullptr && at < end ? bytes(at + 1, *at) : nullptr;
	}
	case kNotWaitingTag:
		return bytes(at, 1);
	case kWaitingTag:
		at = bytes(at, 2);
		return at != nullptr ? varint(at) : nullptr;
	default:
		at = bytes(at, 1);
		at = at != nullptr ? varint(at) : nullptr;
		if (at != nullptr && (tag & kExtended) != 0) {
			at = varint(at);
			at = bytes(at, 1);
		}
		return at;
	}
}

} // namespace deluge::generative
