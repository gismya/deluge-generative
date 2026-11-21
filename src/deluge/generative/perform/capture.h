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

// Capture marks as Perform shows them (spec §17.4, §13.5, §13.6): RECORD and MIDI RECORD toggle the current piece's
// mark in the steering log; the status line's dot, the RECORD LED, the CAPTURE peek and the CAPTURE menu's rows are
// read from it. A mark acts at once: it never enters the core (§13.5). The words are here, as oled_page's are.

#pragma once

#include "generative/perform/queue_text.h"
#include <cstdint>

namespace deluge::generative {
class SteeringLog;
}

namespace deluge::generative::perform {

/// What a mark press did.
enum class MarkOutcome : uint8_t {
	Marked,
	Unmarked,
	LogFull, ///< a mark refused: the log is full (spec §17.2), with the OLED message
	NoPiece, ///< nothing to mark: no piece played yet, or the newest found no room to open
};

/// RECORD (or MIDI RECORD): toggles the current piece's mark (spec §17.4).
MarkOutcome pressMark(SteeringLog& log);
/// A CAPTURE menu row's press: toggles piece `piece`'s mark. An earlier piece unmarked stays in the log until the
/// next piece begins, so a second press marks it again.
MarkOutcome pressMark(SteeringLog& log, int32_t piece);

/// What the dot, the LED and the peek read.
struct CaptureStatus {
	bool piece = false;  ///< there is a current piece
	bool marked = false; ///< the current piece is marked
	bool full = false;   ///< the log is full: new marks are refused
	int32_t markedPieces = 0;
	int32_t usedPercent = 0;
	bool operator==(const CaptureStatus&) const = default;
};
CaptureStatus captureStatus(const SteeringLog& log);

/// The status line's Capture dot (spec §13.5): filled = marked, hollow = a piece that isn't, blinking = log full.
enum class CaptureDot : uint8_t { None, Hollow, Filled };
CaptureDot captureDot(const CaptureStatus& status);

/// The RECORD LED (spec §13.5): on = marked, blinking = log full.
enum class RecordLed : uint8_t { Off, On, Blink };
RecordLed recordLed(const CaptureStatus& status);

/// The CAPTURE peek's three lines, a label and its value right-aligned.
struct CaptureLines {
	static constexpr int32_t kLines = 3;
	char label[kLines][kQueueLineChars + 1] = {};
	char value[kLines][12] = {};
};
void captureLines(const CaptureStatus& status, CaptureLines& out);

/// A CAPTURE menu row: the piece's number in the set and its genre ("2 MELODIC TECHNO"), and MARKED or -.
struct CaptureRow {
	char label[kQueueLineChars + 1] = {};
	char value[8] = {};
};
void captureRow(const SteeringLog& log, int32_t piece, CaptureRow& out);

} // namespace deluge::generative::perform
