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

#include "generative/perform/capture.h"
#include "generative/core/genre.h"
#include "generative/core/steering_log.h"
#include "generative/perform/text.h"

namespace deluge::generative::perform {

MarkOutcome pressMark(SteeringLog& log) {
	return pressMark(log, log.current());
}

MarkOutcome pressMark(SteeringLog& log, int32_t piece) {
	if (piece < 0 || piece >= log.pieces()) {
		// No current piece: none played yet, or the newest found no room to open, which the log being full says.
		return log.full() ? MarkOutcome::LogFull : MarkOutcome::NoPiece;
	}
	const bool on = !log.piece(piece).marked();
	if (!log.markPiece(piece, on)) {
		return MarkOutcome::LogFull; // the only refusal of an existing piece
	}
	return on ? MarkOutcome::Marked : MarkOutcome::Unmarked;
}

CaptureStatus captureStatus(const SteeringLog& log) {
	CaptureStatus out;
	const int32_t current = log.current();
	out.piece = current >= 0;
	out.marked = out.piece && log.piece(current).marked();
	out.full = log.full();
	out.markedPieces = log.markedPieces();
	out.usedPercent = log.capacity() > 0 ? static_cast<int32_t>(int64_t{log.used()} * 100 / log.capacity()) : 0;
	return out;
}

CaptureDot captureDot(const CaptureStatus& status) {
	if (status.marked) {
		return CaptureDot::Filled;
	}
	return status.piece || status.full ? CaptureDot::Hollow : CaptureDot::None;
}

RecordLed recordLed(const CaptureStatus& status) {
	if (status.full) {
		return RecordLed::Blink;
	}
	return status.marked ? RecordLed::On : RecordLed::Off;
}

void captureLines(const CaptureStatus& status, CaptureLines& out) {
	out = CaptureLines{};
	Text(out.label[0]).add("THIS PIECE");
	Text(out.value[0]).add(!status.piece ? "NONE" : status.marked ? "MARKED" : "NOT MARKED");
	Text(out.label[1]).add("MARKED PIECES");
	Text(out.value[1]).add(status.markedPieces);
	Text(out.label[2]).add("LOG");
	if (status.full) {
		Text(out.value[2]).add("FULL");
	}
	else {
		Text(out.value[2]).add(status.usedPercent).add("% USED");
	}
}

void captureRow(const SteeringLog& log, int32_t piece, CaptureRow& out) {
	out = CaptureRow{};
	if (piece < 0 || piece >= log.pieces()) {
		return;
	}
	const SteeringLog::Piece shown = log.piece(piece);
	const Genre* genre = shown.start().recipe.genre;
	Text(out.label).add(piece + 1).add(" ").add(genre != nullptr ? genre->oledName : "");
	Text(out.value).add(shown.marked() ? "MARKED" : "-");
}

} // namespace deluge::generative::perform
