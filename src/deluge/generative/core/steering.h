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
};

/// Continuous values coalesce by key, so a control gets at most one value per step. Discrete ones never do.
constexpr bool isContinuous(CommandKind kind) {
	return kind == CommandKind::SetMacro;
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
