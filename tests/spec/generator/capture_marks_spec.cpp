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

// Capture marks (spec §17.4, §13.5, §13.6): what RECORD does to the steering log, what the status line's dot, the
// RECORD LED and the CAPTURE peek read from it, and the CAPTURE menu's rows.

#include "generative/core/steering_log.h"
#include "generative/perform/capture.h"
#include "harness.h"
#include <cppspec.hpp>
#include <string>
#include <vector>

using namespace deluge::generative;
using namespace deluge::generative::perform;
using namespace harness;

namespace {

/// A buffer of `bytes` and its log, with nothing in it.
struct Buffer {
	explicit Buffer(int32_t size) : bytes(static_cast<size_t>(size)), log(bytes.data(), size) {}
	std::vector<uint8_t> bytes;
	SteeringLog log;
};

PieceStart startOf(const Genre& genre) {
	PieceStart start;
	start.recipe = acidRecipe();
	start.recipe.genre = &genre;
	return start;
}

/// Fills the current piece's log until it is cut.
void fill(SteeringLog& log) {
	int32_t step = 0;
	while (log.append(step, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), 1})) {
		step++;
	}
}

std::string row(const SteeringLog& log, int32_t piece) {
	CaptureRow out;
	captureRow(log, piece, out);
	return std::string{out.label} + "|" + out.value;
}

/// The leave menu as text: its title, then each option.
std::string menu(const SteeringLog& log) {
	static const char* const kNames[] = {"MARKED + LAST", "MARKED ONLY", "MARKED", "LAST", "NOTHING", "LEAVE", "CANCEL"};
	LeaveMenu out;
	leaveMenu(log, out);
	std::string text = out.title;
	for (int32_t i = 0; i < out.count; i++) {
		text += std::string{"|"} + kNames[static_cast<int32_t>(out.options[i])];
	}
	return text;
}

/// The pieces `option` keeps, in set order.
std::vector<int32_t> kept(const SteeringLog& log, LeaveOption option) {
	std::vector<int32_t> out;
	for (int32_t i = nextKept(log, option, -1); i >= 0; i = nextKept(log, option, i)) {
		out.push_back(i);
	}
	return out;
}

/// A piece of `genre` that plays `steps` steps and stops, marked if `marked`.
void play(SteeringLog& log, const Genre& genre, int32_t steps, bool marked) {
	log.beginPiece(startOf(genre));
	log.append(0, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), 1});
	log.mark(marked);
	log.endPiece(steps, PieceEnd::Stopped);
}

} // namespace

// clang-format off
describe capture_marks("Capture marks", $ {
	it("RECORD marks the current piece, and again unmarks it", _ {
		Buffer b(4096);
		expect(pressMark(b.log) == MarkOutcome::NoPiece).to_be_true();
		b.log.beginPiece(startOf(genres::technoAcid));
		expect(pressMark(b.log) == MarkOutcome::Marked).to_be_true();
		expect(b.log.piece(0).marked()).to_be_true();
		expect(pressMark(b.log) == MarkOutcome::Unmarked).to_be_true();
		expect(b.log.piece(0).marked()).to_be_false();
		// Stopped, it marks the last piece, which the leave menu offers.
		b.log.endPiece(40, PieceEnd::Stopped);
		expect(pressMark(b.log) == MarkOutcome::Marked).to_be_true();
	});

	it("RECORD's mark is refused once the log is full, unless the cut found the piece marked; its unmark isn't", _ {
		Buffer b(1024);
		b.log.beginPiece(startOf(genres::technoAcid));
		fill(b.log);
		expect(pressMark(b.log) == MarkOutcome::LogFull).to_be_true();
		expect(b.log.piece(0).marked()).to_be_false();

		Buffer c(1024);
		c.log.beginPiece(startOf(genres::technoAcid));
		pressMark(c.log);
		fill(c.log);
		// Marked when the log cut it: a second press puts the mark back, so a double press loses nothing.
		expect(pressMark(c.log) == MarkOutcome::Unmarked).to_be_true();
		expect(pressMark(c.log) == MarkOutcome::Marked).to_be_true();

		// A piece with no room even to open: no current piece, and the press says the log is full.
		Buffer d(1024);
		d.log.beginPiece(startOf(genres::technoAcid));
		pressMark(d.log);
		fill(d.log);
		d.log.endPiece(500, PieceEnd::Stopped);
		d.log.beginPiece(startOf(genres::technoAcid));
		expect(d.log.current()).to_equal(-1);
		expect(pressMark(d.log) == MarkOutcome::LogFull).to_be_true();
		expect(d.log.piece(0).marked()).to_be_true();
	});

	it("status: whether there is a current piece, whether it's marked, the marks, the log's use and whether it's full", _ {
		Buffer b(1024);
		CaptureStatus status = captureStatus(b.log);
		expect(status.piece || status.marked || status.full).to_be_false();
		expect(status.usedPercent).to_equal(0);
		b.log.beginPiece(startOf(genres::technoAcid));
		pressMark(b.log);
		status = captureStatus(b.log);
		expect(status.piece && status.marked).to_be_true();
		expect(status.markedPieces).to_equal(1);
		expect(status.usedPercent).to_be_greater_than(0);
		fill(b.log);
		status = captureStatus(b.log);
		expect(status.full).to_be_true();
		expect(status.usedPercent).to_be_greater_than(90);
	});

	it("the dot: hollow for an unmarked piece, filled for a marked one, none before a piece, blinking when full", _ {
		Buffer b(1024);
		expect(captureDot(captureStatus(b.log)) == CaptureDot::None).to_be_true();
		b.log.beginPiece(startOf(genres::technoAcid));
		expect(captureDot(captureStatus(b.log)) == CaptureDot::Hollow).to_be_true();
		pressMark(b.log);
		expect(captureDot(captureStatus(b.log)) == CaptureDot::Filled).to_be_true();
		expect(captureStatus(b.log).full).to_be_false();
		fill(b.log);
		expect(captureDot(captureStatus(b.log)) == CaptureDot::Filled).to_be_true();
		expect(captureStatus(b.log).full).to_be_true();
		// The next piece found no room to open: no current piece, the log still full.
		b.log.endPiece(500, PieceEnd::Stopped);
		b.log.beginPiece(startOf(genres::technoAcid));
		expect(captureDot(captureStatus(b.log)) == CaptureDot::Hollow).to_be_true();
		expect(captureStatus(b.log).full).to_be_true();
	});

	it("the RECORD LED: on while the current piece is marked, blinking while the log is full", _ {
		Buffer b(1024);
		expect(recordLed(captureStatus(b.log)) == RecordLed::Off).to_be_true();
		b.log.beginPiece(startOf(genres::technoAcid));
		expect(recordLed(captureStatus(b.log)) == RecordLed::Off).to_be_true();
		pressMark(b.log);
		expect(recordLed(captureStatus(b.log)) == RecordLed::On).to_be_true();
		fill(b.log);
		expect(recordLed(captureStatus(b.log)) == RecordLed::Blink).to_be_true();
	});

	it("the CAPTURE peek: this piece, the marks and the log", _ {
		Buffer b(1024);
		CaptureLines lines;
		captureLines(captureStatus(b.log), lines);
		expect(std::string{lines.label[0]} + "|" + lines.value[0]).to_equal(std::string{"THIS PIECE|NONE"});
		expect(std::string{lines.label[1]} + "|" + lines.value[1]).to_equal(std::string{"MARKED PIECES|0"});
		expect(std::string{lines.label[2]} + "|" + lines.value[2]).to_equal(std::string{"LOG|0% USED"});
		b.log.beginPiece(startOf(genres::technoAcid));
		captureLines(captureStatus(b.log), lines);
		expect(std::string{lines.value[0]}).to_equal(std::string{"NOT MARKED"});
		pressMark(b.log);
		fill(b.log);
		captureLines(captureStatus(b.log), lines);
		expect(std::string{lines.value[0]}).to_equal(std::string{"MARKED"});
		expect(std::string{lines.value[1]}).to_equal(std::string{"1"});
		expect(std::string{lines.value[2]}).to_equal(std::string{"FULL"});
	});

	it("the CAPTURE menu: a row per piece in the log, numbered in set order, with its genre and whether it's marked", _ {
		Buffer b(8192);
		b.log.beginPiece(startOf(genres::technoAcid));
		pressMark(b.log);
		b.log.endPiece(64, PieceEnd::Stopped);
		b.log.beginPiece(startOf(genres::technoMelodic));
		expect(row(b.log, 0)).to_equal(std::string{"1 ACID TECHNO|MARKED"});
		expect(row(b.log, 1)).to_equal(std::string{"2 MELODIC TECHNO|-"});
		// A press on an earlier row unmarks it, and another marks it again, until the next piece begins.
		expect(pressMark(b.log, 0) == MarkOutcome::Unmarked).to_be_true();
		expect(row(b.log, 0)).to_equal(std::string{"1 ACID TECHNO|-"});
		expect(pressMark(b.log, 0) == MarkOutcome::Marked).to_be_true();
		pressMark(b.log, 0);
		b.log.endPiece(64, PieceEnd::Stopped);
		pressMark(b.log, 1);
		b.log.beginPiece(startOf(genres::technoAcid));
		expect(b.log.pieces()).to_equal(2);
		expect(row(b.log, 0)).to_equal(std::string{"1 MELODIC TECHNO|MARKED"});
	});

	context("leaving (§13.8, §17.7)", _ {
		it("no piece played: LEAVE PERFORM?, and nothing is kept", _ {
			Buffer b(4096);
			expect(menu(b.log)).to_equal(std::string{"LEAVE PERFORM?|LEAVE|CANCEL"});
			expect(kept(b.log, LeaveOption::Leave).empty()).to_be_true();
		});

		it("no marks: LAST keeps the last piece, playing or stopped", _ {
			Buffer b(16384);
			play(b.log, genres::technoAcid, 40, false);
			b.log.beginPiece(startOf(genres::technoMelodic)); // still playing as the menu opens
			b.log.append(0, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), 1});
			expect(menu(b.log)).to_equal(std::string{"LEAVE, KEEP:|LAST|NOTHING|CANCEL"});
			b.log.endPiece(12, PieceEnd::Stopped); // a choice stops the transport
			expect(kept(b.log, LeaveOption::Last) == std::vector<int32_t>{0}).to_be_true();
			expect(kept(b.log, LeaveOption::Nothing).empty()).to_be_true();
		});

		it("marks and an unmarked last piece: MARKED + LAST or MARKED ONLY, in set order", _ {
			Buffer b(32768);
			play(b.log, genres::technoAcid, 40, true);
			play(b.log, genres::technoMelodic, 40, true);
			play(b.log, genres::technoAcid, 20, false);
			expect(menu(b.log)).to_equal(std::string{"KEEP: 2 MARKED|MARKED + LAST|MARKED ONLY|NOTHING|CANCEL"});
			expect(kept(b.log, LeaveOption::MarkedAndLast) == std::vector<int32_t>{0, 1, 2}).to_be_true();
			expect(kept(b.log, LeaveOption::MarkedOnly) == std::vector<int32_t>{0, 1}).to_be_true();
		});

		it("the last piece itself marked: MARKED keeps every mark", _ {
			Buffer b(32768);
			play(b.log, genres::technoAcid, 40, true);
			play(b.log, genres::technoMelodic, 40, true);
			expect(menu(b.log)).to_equal(std::string{"KEEP: 2 MARKED|MARKED|NOTHING|CANCEL"});
			expect(kept(b.log, LeaveOption::Marked) == std::vector<int32_t>{0, 1}).to_be_true();
		});

		it("an earlier piece unmarked in CAPTURE ▸, still in the log, isn't kept", _ {
			Buffer b(32768);
			play(b.log, genres::technoAcid, 40, true);
			play(b.log, genres::technoMelodic, 40, true);
			play(b.log, genres::technoAcid, 20, false);
			expect(pressMark(b.log, 0) == MarkOutcome::Unmarked).to_be_true();
			expect(b.log.pieces()).to_equal(3);
			expect(menu(b.log)).to_equal(std::string{"KEEP: 1 MARKED|MARKED + LAST|MARKED ONLY|NOTHING|CANCEL"});
			expect(kept(b.log, LeaveOption::MarkedAndLast) == std::vector<int32_t>{1, 2}).to_be_true();
		});

		it("a piece with nothing logged builds nothing: stopped on its first step, or its header cut", _ {
			Buffer b(16384);
			play(b.log, genres::technoAcid, 40, true);
			b.log.beginPiece(startOf(genres::technoMelodic));
			b.log.endPiece(0, PieceEnd::Stopped);
			expect(menu(b.log)).to_equal(std::string{"KEEP: 1 MARKED|MARKED|NOTHING|CANCEL"});
			expect(kept(b.log, LeaveOption::Marked) == std::vector<int32_t>{0}).to_be_true();
		});

		it("names a song GEN <GENRE> <seed code> LIVE, and a split piece's later songs LIVE 2, LIVE 3", _ {
			char name[kCaptureNameChars + 1];
			captureSongName(genres::technoAcid, 0x1A2B3C4, 1, name);
			char code[kSeedCodeChars + 1];
			seedCode(0x1A2B3C4, code);
			expect(std::string{name}).to_equal("GEN " + std::string{genres::technoAcid.oledName} + " " + code + " LIVE");
			captureSongName(genres::technoAcid, 0x1A2B3C4, 3, name);
			expect(std::string{name}).to_equal("GEN " + std::string{genres::technoAcid.oledName} + " " + code +
			                                   " LIVE 3");
		});

		it("titles the screen BUILDING n OF m, SAVING n OF m, and after a failure n OF m SAVED", _ {
			char title[kCaptureTitleChars + 1];
			captureTitle(CaptureStage::Building, 2, 3, title);
			expect(std::string{title}).to_equal("BUILDING 2 OF 3");
			captureTitle(CaptureStage::Saving, 12, 13, title);
			expect(std::string{title}).to_equal("SAVING 12 OF 13");
			captureTitle(CaptureStage::Saved, 1, 3, title);
			expect(std::string{title}).to_equal("1 OF 3 SAVED");
		});
	});
});

CPPSPEC_SPEC(capture_marks)
