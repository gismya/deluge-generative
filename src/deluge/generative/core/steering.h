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

// Steering into the core (spec §2.6, ADR 0005). UI handlers push commands into a fixed queue; the core drains it at
// each step decision, applies each command where it lands and logs it there. The queue and the log live outside the
// Generator instance blocks, because a log spans pieces and transitions; the driver owns both.
//
// One producer (task context) and one consumer (the tick). The tick and the UI never pre-empt each other: the audio
// routine is a cooperative task that runs only where UI work yields, and nothing here yields. So plain fields are
// enough, and a coalescing push may rewrite a queued entry in place.

#pragma once

#include <cstdint>

namespace deluge::generative {

/// What a command does. Append new kinds at the end: logs store the value.
enum class CommandKind : uint8_t {
	SetMacro, ///< target: Macro. value: macro units. Continuous.
	SetKey,   ///< target: key root, 0 = C. value: Scale. Lands on the next phrase line or an Arc Drop (spec §9.7).
	SetTake,  ///< target: part. value: take, 0 to kMaxTakes - 1. Lands at the next phrase line (spec §6.6).
	/// target: SectionKind (Groove, Break or Build). Lands at the next phrase line; a Build at the bar line that lets
	/// it end on one (spec §9.6). value: the Build's length in steps, drawn by the core when it queues the force, so
	/// the log holds it. The same force again while it waits cancels it.
	ForceSection,
	SetHold, ///< value: 1 holds the Arc, 0 releases it. Lands on the next undecided step (spec §9.6).
};

/// Continuous values coalesce by key, so a control gets at most one value per step. Discrete ones never do.
constexpr bool isContinuous(CommandKind kind) {
	return kind == CommandKind::SetMacro;
}

/// Whether a command waits in the core's pending list for its line (spec §2.6), instead of landing on the next
/// undecided step.
constexpr bool isQuantised(CommandKind kind) {
	return kind == CommandKind::SetKey || kind == CommandKind::SetTake || kind == CommandKind::ForceSection;
}

struct Command {
	CommandKind kind = CommandKind::SetMacro;
	uint8_t target = 0; ///< A part, a lane or a macro, by kind.
	int32_t value = 0;

	[[nodiscard]] constexpr bool sameKey(const Command& other) const {
		return kind == other.kind && target == other.target;
	}
	constexpr bool operator==(const Command&) const = default;
};

class SteeringQueue {
public:
	static constexpr int32_t kCapacity = 64; ///< Spec §2.6, §20.

	/// Queues a command. A continuous one replaces a queued command with the same key, keeping its place.
	/// @return false if the queue is full and nothing was coalesced: a discrete command is refused (the caller shows
	/// the stock popup).
	bool push(const Command& command) {
		if (isContinuous(command.kind)) {
			for (int32_t i = 0; i < count_; i++) {
				Command& queued = entries_[(head_ + i) % kCapacity];
				if (queued.sameKey(command)) {
					queued.value = command.value;
					return true;
				}
			}
		}
		if (count_ == kCapacity) {
			return false;
		}
		entries_[(head_ + count_) % kCapacity] = command;
		count_++;
		return true;
	}

	/// Takes the oldest command.
	bool pop(Command* out) {
		if (count_ == 0) {
			return false;
		}
		*out = entries_[head_];
		head_ = (head_ + 1) % kCapacity;
		count_--;
		return true;
	}

	[[nodiscard]] int32_t size() const { return count_; }
	void clear() { head_ = count_ = 0; }

private:
	Command entries_[kCapacity];
	int32_t head_ = 0;
	int32_t count_ = 0;
};

/// The core's pending list (spec §2.6): quantised commands waiting in the instance state for their line. One slot for
/// the key change, one per part's take and one for the force; a later command replaces the one in its slot, so turning
/// the key editor detent by detent queues only where it stops.
class PendingList {
public:
	/// A key change, every part's take and a force, with room for the mutes, solos and End still to come.
	static constexpr int32_t kCapacity = 16;

	/// Queues a command, replacing the one in its slot. @return false if the list is full, and nothing changed.
	bool add(const Command& command) {
		Command* slot = find(command);
		if (slot == nullptr) {
			if (count_ == kCapacity) {
				return false;
			}
			slot = &entries_[count_++];
		}
		*slot = command;
		return true;
	}

	/// The waiting command in `command`'s slot, or nullptr.
	[[nodiscard]] const Command* waiting(const Command& command) const {
		int32_t i = indexOf(command);
		return i < 0 ? nullptr : &entries_[i];
	}
	[[nodiscard]] bool has(CommandKind kind) const {
		for (int32_t i = 0; i < count_; i++) {
			if (entries_[i].kind == kind) {
				return true;
			}
		}
		return false;
	}

	/// Drops the command in `command`'s slot, if any.
	void cancel(const Command& command) {
		land([&](const Command& waiting) { return sameSlot(waiting, command); });
	}

	/// Hands each waiting command to `lands(command)`, oldest first. One for which it returns true leaves the list.
	template <typename Lands>
	void land(Lands lands) {
		for (int32_t i = 0; i < count_;) {
			if (lands(entries_[i])) {
				for (int32_t j = i + 1; j < count_; j++) {
					entries_[j - 1] = entries_[j];
				}
				count_--;
			}
			else {
				i++;
			}
		}
	}

	[[nodiscard]] int32_t size() const { return count_; }
	void clear() { count_ = 0; }

private:
	/// A key change and a force each have one slot whatever their target; a take has one per part.
	static constexpr bool sameSlot(const Command& a, const Command& b) {
		bool oneSlot = a.kind == CommandKind::SetKey || a.kind == CommandKind::ForceSection;
		return oneSlot ? a.kind == b.kind : a.sameKey(b);
	}
	/// The index of the command in `command`'s slot, or -1.
	[[nodiscard]] int32_t indexOf(const Command& command) const {
		for (int32_t i = 0; i < count_; i++) {
			if (sameSlot(entries_[i], command)) {
				return i;
			}
		}
		return -1;
	}
	Command* find(const Command& command) {
		int32_t i = indexOf(command);
		return i < 0 ? nullptr : &entries_[i];
	}

	Command entries_[kCapacity];
	int32_t count_ = 0;
};

/// One logged command, stamped with the step where it took effect (spec §17.2).
struct LogEntry {
	int32_t step = 0;
	Command command;
};

/// The steering log, over a buffer the driver hands it (Perform: a fixed SDRAM block, spec §20).
class SteeringLog {
public:
	SteeringLog(LogEntry* buffer, int32_t capacity) : entries_(buffer), capacity_(capacity) {}

	/// Logs a command where it landed. @return false once the log is full; the entry is lost.
	bool append(int32_t step, const Command& command) {
		if (count_ == capacity_) {
			full_ = true;
			return false;
		}
		entries_[count_++] = LogEntry{step, command};
		return true;
	}

	[[nodiscard]] int32_t size() const { return count_; }
	[[nodiscard]] bool full() const { return full_; }
	[[nodiscard]] const LogEntry& at(int32_t i) const { return entries_[i]; }
	void clear() {
		count_ = 0;
		full_ = false;
	}

private:
	LogEntry* entries_;
	int32_t capacity_;
	int32_t count_ = 0;
	bool full_ = false;
};

} // namespace deluge::generative
