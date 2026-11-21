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
#include <algorithm>
#include <cstring>

namespace deluge::generative {

namespace {

static_assert(static_cast<int32_t>(kLastCommandKind) < 0x40, "a kind fits the tag's low 6 bits");
constexpr uint8_t kExtended = 0x40; ///< The command's length and lane follow.
constexpr int32_t kMaxPathRecord = 1 + 5 + 4 + kMaxSoundPath;

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

GEN_STEP_CODE SteeringLog::PieceInfo& SteeringLog::newest() const {
	return *reinterpret_cast<PieceInfo*>(buffer_ + newest_);
}

void SteeringLog::beginPiece(const PieceStart& start) {
	if (pieces_ > 0) {
		PieceInfo& last = newest();
		if (last.ended == PieceEnd::Playing) {
			endPiece(lastStep_ + 1, PieceEnd::Stopped);
		}
		if (!last.marked) {
			used_ = newest_; // thrown away (spec §17.3)
			pieces_--;
		}
	}
	open_ = false;
	newestWithHeader_ = -1;
	const int32_t at = alignUp(used_);
	if (at + static_cast<int32_t>(sizeof(PieceInfo)) > room()) {
		newest_ = pieces_ > 0 ? offsetOf(pieces_ - 1) : 0;
		cut_ = true; // no room even for its opening: nothing logs, and marks are refused
		return;
	}
	newest_ = at;
	PieceInfo& info = newest();
	info = PieceInfo{};
	info.bytes = sizeof(PieceInfo);
	info.endStep = -1;
	info.cutAt = -1;
	info.headerBytes = sizeof(PieceInfo);
	info.ended = PieceEnd::Playing;
	info.marked = false;
	info.start = start;
	used_ = at + static_cast<int32_t>(sizeof(PieceInfo));
	pieces_++;
	newestWithHeader_ = pieces_ - 1;
	open_ = true;
	cut_ = false;
	inRecords_ = false;
	lastStep_ = 0;
	stepBefore_ = 0;
	dropsOnStep_ = 0;
	stepFrom_ = used_;
	for (int32_t& landed : landed_) {
		landed = -1;
	}
}

uint8_t* SteeringLog::writeHeader(uint8_t tag, const void* data, int32_t length) {
	if (!open_ || cut_ || inRecords_ || length > 0xFFFF) {
		return nullptr;
	}
	if (used_ + 3 + length > room()) {
		cutHeader();
		return nullptr;
	}
	uint8_t* at = buffer_ + used_;
	at[0] = tag;
	at[1] = static_cast<uint8_t>(length);
	at[2] = static_cast<uint8_t>(length >> 8);
	if (data != nullptr) {
		std::memcpy(at + 3, data, static_cast<size_t>(length));
	}
	used_ += 3 + length;
	PieceInfo& info = newest();
	info.bytes = used_ - newest_;
	info.headerBytes = static_cast<uint16_t>(info.bytes);
	stepFrom_ = used_;
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

void SteeringLog::endPiece(int32_t step, PieceEnd why) {
	if (!open_) {
		return;
	}
	PieceInfo& info = newest();
	info.endStep = step;
	info.ended = why;
	open_ = false;
}

void SteeringLog::mark(bool on) {
	if (pieces_ > 0) {
		newest().marked = on;
	}
}

void SteeringLog::cutHeader() {
	// A piece with half a snapshot can't be built: none of the header stays (spec §17.2).
	PieceInfo& info = newest();
	used_ = newest_ + static_cast<int32_t>(sizeof(PieceInfo));
	info.headerBytes = sizeof(PieceInfo);
	info.bytes = used_ - newest_;
	info.cutAt = 0;
	cut_ = true;
	newestWithHeader_ = -1;
}

GEN_STEP_CODE void SteeringLog::cut(int32_t step) {
	// The piece's span ends before `step`: that step's records go, so every logged step is whole (spec §17.2). The
	// header stays: stepFrom_ is past it before the first record.
	PieceInfo& info = newest();
	used_ = stepFrom_;
	lastStep_ = stepBefore_;
	// A swap dropped on that step whose SetSound landed before it stays said, on the step before.
	for (int32_t p = 0; p < kMaxParts; p++) {
		if ((dropsOnStep_ & (1 << p)) != 0 && dropLanding_[p] < step) {
			writeDropped(0, static_cast<uint8_t>(p));
		}
	}
	dropsOnStep_ = 0;
	info.bytes = used_ - newest_;
	info.cutAt = step;
	cut_ = true;
}

GEN_STEP_CODE void SteeringLog::writeDropped(uint32_t delta, uint8_t part) {
	uint8_t* at = buffer_ + used_;
	*at++ = kSoundDroppedTag;
	at = putVarint(at, delta);
	*at++ = part;
	used_ = static_cast<int32_t>(at - buffer_);
	newest().bytes = used_ - newest_;
}

GEN_STEP_CODE bool SteeringLog::write(int32_t step, const uint8_t* data, int32_t length) {
	if (!open_ || cut_) {
		return false;
	}
	if (!inRecords_ || step != lastStep_) {
		stepBefore_ = lastStep_;
		stepFrom_ = used_;
		dropsOnStep_ = 0;
	}
	if (used_ + length > room()) {
		cut(step);
		return false;
	}
	std::memcpy(buffer_ + used_, data, static_cast<size_t>(length));
	used_ += length;
	newest().bytes = used_ - newest_;
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

bool SteeringLog::appendSoundDropped(int32_t step, uint8_t part) {
	if (!open_ || part >= kMaxParts || landed_[part] < 0) {
		return false;
	}
	PieceInfo& info = newest();
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
	if (used_ + length > capacity_) {
		// A guard: with one drop per landing the reserve can't run out (no SetSound lands once the records' room is
		// gone, so at most one a part waits for its drop). Were it to, nothing more logs; nothing is rewound, so the
		// drops already written stay.
		if (!cut_) {
			info.cutAt = lastStep_ + 1;
			cut_ = true;
		}
		return false;
	}
	if (stamp != lastStep_) {
		stepBefore_ = lastStep_;
		stepFrom_ = used_;
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

// ---- Reading ----

int32_t SteeringLog::offsetOf(int32_t i) const {
	int32_t at = 0;
	for (int32_t p = 0; p < i; p++) {
		at = alignUp(at + reinterpret_cast<const PieceInfo*>(buffer_ + at)->bytes);
	}
	return at;
}

SteeringLog::Piece SteeringLog::piece(int32_t i) const {
	return Piece(buffer_ + offsetOf(i));
}

void SteeringLog::clear() {
	used_ = 0;
	pieces_ = 0;
	newest_ = 0;
	newestWithHeader_ = -1;
	open_ = false;
	cut_ = false;
	inRecords_ = false;
}

const PieceStart& SteeringLog::Piece::start() const {
	return reinterpret_cast<const PieceInfo*>(base_)->start;
}
PieceEnd SteeringLog::Piece::ended() const {
	return reinterpret_cast<const PieceInfo*>(base_)->ended;
}
int32_t SteeringLog::Piece::endStep() const {
	return reinterpret_cast<const PieceInfo*>(base_)->endStep;
}
int32_t SteeringLog::Piece::cutAt() const {
	return reinterpret_cast<const PieceInfo*>(base_)->cutAt;
}
bool SteeringLog::Piece::marked() const {
	return reinterpret_cast<const PieceInfo*>(base_)->marked;
}
int32_t SteeringLog::Piece::bytes() const {
	return reinterpret_cast<const PieceInfo*>(base_)->bytes;
}
int32_t SteeringLog::Piece::headerBytes() const {
	return reinterpret_cast<const PieceInfo*>(base_)->headerBytes;
}
const uint8_t* SteeringLog::Piece::records() const {
	return base_ + headerBytes();
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
	out.command.kind = static_cast<CommandKind>(tag & ~kExtended);
	out.command.target = *at++;
	uint32_t zigzag = 0;
	at = getVarint(at, zigzag);
	out.command.value = static_cast<int32_t>((zigzag >> 1) ^ (0u - (zigzag & 1)));
	if ((tag & kExtended) != 0) {
		uint32_t length = 0;
		at = getVarint(at, length);
		out.command.length = static_cast<uint16_t>(length);
		out.command.lane = static_cast<Lane>(*at++);
	}
	return at;
}

} // namespace deluge::generative
