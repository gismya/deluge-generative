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

#include "generative/perform/clip_page.h"
#include "generative/perform/genre_menu.h"
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
	LogFull, ///< a mark refused: the log is full (spec §17.2), or so full the newest piece found no room to open
	NoPiece, ///< nothing to mark: no piece played yet
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

// ---- Leaving (spec §13.8, §17.7) ----

/// A leave menu option, as it shows.
enum class LeaveOption : uint8_t {
	MarkedAndLast, ///< MARKED + LAST: every marked piece and the last one
	MarkedOnly,    ///< MARKED ONLY: the last piece, unmarked, goes
	Marked,        ///< MARKED: the last piece is itself marked
	Last,          ///< LAST: no marks
	Nothing,
	Leave,  ///< LEAVE: no piece was played
	Cancel, ///< back to Perform, which never stopped
};

/// The leave menu (spec §13.8): its title and options, from the log as it is when BACK is held.
struct LeaveMenu {
	static constexpr int32_t kMaxOptions = 4;
	static constexpr int32_t kTitleChars = 16;
	char title[kTitleChars + 1] = {};
	LeaveOption options[kMaxOptions] = {};
	int32_t count = 0;
};
void leaveMenu(const SteeringLog& log, LeaveMenu& out);

/// Whether `option` keeps piece `piece` (spec §17.7): the marked pieces and, unless MARKED ONLY, the last piece. A
/// piece with nothing logged (its header cut, or stopped before its first step) builds nothing and is never kept.
/// An earlier piece unmarked in CAPTURE ▸ is still in the log until the next piece begins: never kept.
[[nodiscard]] bool keeps(const SteeringLog& log, LeaveOption option, int32_t piece);
/// The pieces `option` keeps, in set order: the first after `after` (-1 for the first), or -1 past the last.
[[nodiscard]] int32_t nextKept(const SteeringLog& log, LeaveOption option, int32_t after);

/// A Capture song's name (spec §17.7): Compose's (`GEN <GENRE> <seed code>`) plus ` LIVE`; with `letter` (0 none, 1
/// for B up to kCaptureLetters - 1 for Z) the letter of a name the card had already, `… LIVE B`; and for a split
/// piece's second song on (`song` from 1) its number, `… LIVE 2` or `… LIVE B 2`. Numbers are a piece's songs only.
constexpr int32_t kCaptureLetters = 26; ///< None, then B to Z
constexpr int32_t kCaptureNameChars = 4 + kMaxGenreOledName + 1 + kSeedCodeChars + 5 + 2 + 4;
void captureSongName(const Genre& genre, uint32_t seed, int32_t letter, int32_t song,
                     char (&out)[kCaptureNameChars + 1]);
/// The letter whose set a song file `stem` (its name without the extension) is in, for Capture songs named `base`
/// (captureSongName() with no letter and song 1, `GEN ACID TECHNO 1A2B3C4 LIVE`): `base` or `base N` is 0, `base B`
/// or `base B N` is 1, and so on, any case. -1 for any other file.
[[nodiscard]] int32_t captureLetterOf(const char* base, const char* stem);
/// The first letter whose whole set is free (spec §17.7): bit L of `taken` set for each letter a file on the card is
/// in. -1 when every one is taken.
[[nodiscard]] int32_t freeCaptureLetter(uint32_t taken);

/// The Capture screen's title (spec §17.7): `BUILDING 2 OF 3`, `SAVING 2 OF 3`; and after a failure `1 OF 3 SAVED`.
/// It counts songs: a split piece adds one as it splits.
constexpr int32_t kCaptureTitleChars = 21;
enum class CaptureStage : uint8_t { Building, Saving, Saved };
void captureTitle(CaptureStage stage, int32_t song, int32_t songs, char (&out)[kCaptureTitleChars + 1]);

} // namespace deluge::generative::perform
