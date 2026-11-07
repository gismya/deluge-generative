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

// The queue as Perform's OLED shows it (spec §13.5): the status line's inverted tag for the next action to land
// (">BR3", "+" when more are queued), and QUEUE's lines, each action named with its countdown. Read from task context
// between ticks (spec §2.8).

#pragma once

#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/steering.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// An action waiting in the pending list, and the step it lands on.
struct QueuedAction {
	Command command;
	int32_t landsAt = 0;
};

/// Fills `out` with the actions waiting in `generator`'s pending list, soonest first, the older first on a tie.
/// @return How many it wrote.
int32_t queuedByTime(const Generator& generator, std::span<QueuedAction> out);

/// The short tag of a queued action: MUT, SOL, KEY, TAK, END, or a forced Section's two letters (GR, BR, BU).
const char* queueTag(const Command& command);

/// Names an action for QUEUE: "ACID MUTE", "DRUMS TAKE 2", "BREAK", "OUTRO", "KEY D DORIAN".
void queueLabel(const Command& command, const Genre& genre, std::span<char> out);

/// The time left until an action lands, `ticks` Generator ticks away: whole bars rounded up, and under a bar whole
/// beats rounded up, at least 1. Short: "3" and "2B"; in words: "3 BARS" and "2 BEATS".
void countdown(int32_t ticks, bool words, std::span<char> out);

/// A key root's name, "C" to "B", and a Scale's in capitals, as QUEUE and RECIPE show them.
const char* keyName(uint8_t root);
const char* scaleName(Scale scale);

/// The status line's tag at `clock` (Generator ticks, as playing): the next action to land and its countdown, with
/// "+" when more are queued, as ">MUT3B+". Empty when nothing is queued.
void statusTag(const Generator& generator, int32_t clock, std::span<char> out);

} // namespace deluge::generative::perform
