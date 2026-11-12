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

// Perform's OLED (spec §13.5): the status line, the peeks, and the PART, MACROS and ARC pages, composed from a piece
// played step by step.

#include "generative/perform/oled_page.h"
#include "stream_helpers.h"
#include <cppspec.hpp>
#include <string>

using namespace deluge::generative;
using namespace deluge::generative::perform;
using namespace harness;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;
constexpr const char* kAcidPreset = "SYNTHS/Bass/165 Acid.XML";

/// A Techno piece played step by step, with the OLED composed as the Perform view does.
struct Piece : StepwisePiece<> {
	Recipe recipe = technoRecipe();
	SectionCount count;
	ArcMapState arc;
	bool stopped = false;
	bool ext = false;
	OledPeek peek = OledPeek::None;
	GridPage page = GridPage::Song;
	PartTitleInput title{
	    .part = kAcid, .parts = partCount(genres::techno), .genre = &genres::techno, .preset = kAcidPreset};
	ControlLevel levels[kNumSoundControls] = {};

	explicit Piece(bool skipIntro = true) : StepwisePiece({.seed = 3, .skipIntro = skipIntro}) {}
	OledInput input() {
		OledInput in;
		in.piece = stopped ? nullptr : driver.playing();
		in.recipe = &recipe;
		if (const Event* section = lastSection(); section != nullptr && !stopped) {
			in.heard = {
			    .known = true, .kind = section->section, .occurrence = section->occurrence, .tick = section->tick};
		}
		in.clock = driver.clock();
		in.phraseSteps = phraseSteps();
		in.ext = ext;
		in.history = &driver.history();
		arc.refreshPlan(in.piece, 0);
		in.arcState = &arc;
		in.peek = peek;
		in.page = page;
		in.title = title;
		std::copy_n(levels, kNumSoundControls, in.levels);
		return in;
	}
	OledScreen screen() {
		OledScreen out;
		composeOled(input(), count, out);
		return out;
	}
	std::string left() { return screen().left; }
	std::string right() { return screen().right; }
	std::string line(int32_t i) { return screen().lines[i]; }
	std::string sound() { return screen().sound; }
	std::string take() { return screen().take; }
};

Command hold() {
	return {CommandKind::SetHold, 0, kPress};
}

} // namespace

// clang-format off
describe oled_page("Perform's OLED", $ {
	it("status line: says STOPPED with nothing playing, and STARTING until a Section is heard", _ {
		Piece piece;
		expect(piece.left()).to_equal(std::string{"STARTING"}); // started, nothing played yet
		expect(piece.right()).to_equal(std::string{"1.1"});
		piece.stopped = true;
		const OledScreen stopped = piece.screen();
		expect(std::string{stopped.left}).to_equal(std::string{"STOPPED"});
		expect(std::string{stopped.right}).to_equal(std::string{});
		expect(stopped.tag).to_be_false();
		expect(stopped.phraseBar).to_equal(0);
	});

	it("status line: names the Section and counts its phrases, with bar.beat in the phrase and the phrase bar", _ {
		Piece piece;
		piece.runToStepLine(0);
		expect(piece.left()).to_equal(std::string{"GROOVE 1"}); // a Groove has no length: no /m
		expect(piece.right()).to_equal(std::string{"1.1"});
		expect(piece.screen().phraseBar).to_equal(0);
		piece.runToStepLine(kStepsPerBar + 4);
		expect(piece.right()).to_equal(std::string{"2.2"});
		piece.runToStepLine(piece.phraseSteps() / 2);
		expect(piece.right()).to_equal(std::string{"5.1"});
		expect(piece.screen().phraseBar).to_equal(kOledWidthPixels / 2);
		expect(piece.screen().tag).to_be_false();
		piece.runToStepLine(piece.phraseSteps() + 1);
		expect(piece.left()).to_equal(std::string{"GROOVE 2"});
		expect(piece.right()).to_equal(std::string{"1.1"});
	});

	it("status line: counts a Section that has a length as n/m, from the phrase where it is heard", _ {
		Piece piece(false); // from the Intro
		piece.runToStepLine(0);
		const int32_t phrases = (piece.generator.arc().sectionSteps() + piece.phraseSteps() - 1) / piece.phraseSteps();
		expect(piece.generator.arc().section()).to_equal(SectionKind::Intro);
		expect(piece.left()).to_equal("INTRO 1/" + std::to_string(phrases));
		// The decided side reaches the next Section 12 ticks before it is heard: the count stays the Intro's.
		const int32_t introSteps = piece.generator.arc().sectionSteps();
		piece.decideBefore(introSteps + 1);
		expect(piece.generator.arc().section()).to_equal(SectionKind::Groove);
		expect(piece.left()).to_equal("INTRO " + std::to_string(phrases) + "/" + std::to_string(phrases));
		piece.runToStepLine(introSteps);
		expect(piece.left()).to_equal(std::string{"GROOVE 1"});
	});

	it("status line: shows the next queued action as a tag in place of bar.beat", _ {
		Piece piece;
		piece.decideBefore(8);
		piece.pushAndDecide(force(SectionKind::Break));
		const OledScreen screen = piece.screen();
		expect(std::string{screen.right}).to_equal(std::string{">BR8"});
		expect(screen.tag).to_be_true();
		expect(std::string{screen.left}).to_equal(std::string{"GROOVE 1"});
	});

	it("status line: adds HELD while the Arc is held, H when the right side leaves it no room, or nothing", _ {
		Piece piece;
		piece.decideBefore(8);
		piece.pushAndDecide(hold());
		expect(piece.left()).to_equal(std::string{"GROOVE 1 HELD"});
		// EXT takes four characters: " HELD" still fits beside "1.1 EXT" exactly.
		piece.ext = true;
		expect(piece.screen().ext).to_be_true();
		expect(piece.left()).to_equal(std::string{"GROOVE 1 HELD"});
		// A tag with more queued, beside EXT: three characters are left.
		piece.pushAndDecide(force(SectionKind::Break));
		piece.pushAndDecide({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Dorian)});
		expect(piece.right()).to_equal(std::string{">BR8+"});
		expect(piece.left()).to_equal(std::string{"GROOVE 1 H"});
		// A mute lands sooner, with a longer tag: one character is left, so nothing is added.
		piece.pushAndDecide(mute(kAcid));
		expect(piece.right()).to_equal(std::string{">MUT2B+"});
		expect(piece.left()).to_equal(std::string{"GROOVE 1"});
	});

	it("QUEUE peek: lists what is queued with its countdown, over whichever page is on", _ {
		Piece piece;
		piece.peek = OledPeek::Queue;
		piece.page = GridPage::Keyboard;
		piece.decideBefore(8);
		expect(piece.line(0)).to_equal(std::string{"NOTHING QUEUED"});
		piece.pushAndDecide(force(SectionKind::Break));
		const OledScreen screen = piece.screen();
		expect(std::string{screen.lines[0]}).to_equal(std::string{"BREAK"});
		expect(std::string{screen.lineRight[0]}).to_equal(std::string{"8 BARS"});
		expect(screen.macros).to_be_false();
		piece.stopped = true;
		expect(piece.line(0)).to_equal(std::string{"NOTHING QUEUED"});
	});

	it("RECIPE peek: the genre, the tempo and swing, the key and scale", _ {
		Piece piece;
		piece.peek = OledPeek::Recipe;
		piece.recipe.tempo = 13250;
		piece.recipe.swing = -7;
		expect(piece.line(0)).to_equal(std::string{"TECHNO"});
		expect(piece.line(1)).to_equal(std::string{"132 BPM SW -7"});
		expect(piece.line(2)).to_equal(std::string{"A MINOR"});
		piece.recipe.genre = nullptr;
		expect(piece.line(0)).to_equal(std::string{"Perform"});
	});

	it("stopped: the genre and the macros' centres in points", _ {
		Piece piece;
		piece.stopped = true;
		piece.recipe.macros.set(Macro::Energy, macroPoints(31));
		piece.recipe.macros.set(Macro::Motion, macroPoints(4));
		const OledScreen screen = piece.screen();
		expect(std::string{screen.lines[0]}).to_equal(std::string{"TECHNO"});
		expect(std::string{screen.lines[1]}).to_equal(std::string{"E31 D25 C25 M4"});
		expect(screen.arc || screen.part || screen.macros).to_be_false();
	});

	it("PART title: a synth part's name and its preset's file name, and its take", _ {
		Piece piece;
		piece.page = GridPage::Clip;
		piece.title.takes = {.playing = 1, .waiting = -1, .newest = 2};
		piece.levels[2] = {.has = true, .home = -10, .live = 20};
		OledScreen screen = piece.screen();
		expect(screen.part).to_be_true();
		expect(std::string{screen.sound}).to_equal(std::string{"ACID 165 Acid"});
		expect(std::string{screen.take}).to_equal(std::string{"T2/3"});
		expect(screen.controls[2] == piece.levels[2]).to_be_true();
		piece.title.frozen = true;
		expect(piece.take()).to_equal(std::string{"T2/3*"});
		// A preset with no folder or extension is its own name.
		piece.title.preset = "Acid";
		expect(piece.sound()).to_equal(std::string{"ACID Acid"});
	});

	it("PART title: a Kit part shows its name only, and a long preset is cut to leave the take its room", _ {
		Piece piece;
		piece.page = GridPage::Clip;
		piece.title.part = kDrums;
		piece.title.preset = "KITS/909.XML";
		expect(piece.sound()).to_equal(std::string{"DRUMS"});
		piece.title.part = kAcid;
		piece.title.preset = "SYNTHS/Lead/070 A Very Long Preset Name.XML";
		expect(piece.take()).to_equal(std::string{"T1/1"});
		expect(piece.sound()).to_equal(std::string{"ACID 070 A Very "}); // 16 characters, a space, then T1/1
		piece.title.takes = {.playing = 15, .waiting = -1, .newest = 15};
		piece.title.frozen = true;
		expect(piece.take()).to_equal(std::string{"T16/16*"});
		expect(piece.sound()).to_equal(std::string{"ACID 070 A Ve"});
	});

	it("PART title: empty with no part focused yet, or one outside the roster", _ {
		Piece piece;
		piece.page = GridPage::Clip;
		piece.title.part = -1;
		OledScreen screen = piece.screen();
		expect(screen.part).to_be_true();
		expect(std::string{screen.sound}).to_equal(std::string{});
		expect(std::string{screen.take}).to_equal(std::string{});
		piece.title.part = piece.title.parts;
		expect(piece.sound()).to_equal(std::string{});
	});

	it("MACROS page: each macro's centre and where the Arc has it, - when stopped", _ {
		Piece piece;
		piece.page = GridPage::Keyboard;
		piece.recipe.macros.set(Macro::Density, macroPoints(21));
		piece.decideBefore(8);
		OledScreen screen = piece.screen();
		expect(screen.macros).to_be_true();
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			expect(screen.macroCentre[m]).to_equal(piece.recipe.macros[macro]);
			expect(screen.macroArc[m]).to_equal(piece.generator.effectiveMacro(macro));
		}
		char text[kMacrosPageTextChars + 1];
		describeMacrosPage(screen, text);
		std::string expected;
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			expected += (m == 0 ? "" : " ") + std::to_string(macroToPoints(piece.recipe.macros[macro])) + "/"
			            + std::to_string(macroToPoints(piece.generator.effectiveMacro(macro)));
		}
		expect(std::string{text}).to_equal(expected);
		expect(std::string{text}.substr(0, 9)).to_equal(std::string{"25/25 21/"});
		// Each row's label: the macro's initial and its centre in points.
		const char* labels[kNumMacros] = {"E25", "D21", "C25", "M25"};
		for (int32_t m = 0; m < kNumMacros; m++) {
			char label[kMacroLabelChars + 1];
			macroLabel(m, screen.macroCentre[m], label);
			expect(std::string{label}).to_equal(std::string{labels[m]});
		}

		piece.stopped = true;
		screen = piece.screen();
		expect(screen.macroArc[0]).to_equal(-1);
		describeMacrosPage(screen, text);
		expect(std::string{text}).to_equal(std::string{"25/- 21/- 25/- 25/-"});
	});

	it("ARC page: follows the SONG page while playing, and its text has each box and the next change", _ {
		Piece piece;
		piece.decideBefore(piece.phraseSteps() + 40);
		piece.pushAndDecide(force(SectionKind::Break));
		const OledScreen screen = piece.screen();
		expect(screen.arc).to_be_true();
		expect(std::string{screen.arcPage.box[1].tag}).to_equal(std::string{"BR"});
		expect(screen.arcPage.box[1].blink).to_be_true();
		char text[kArcPageTextChars + 1];
		describeArcPage(screen.arcPage, text);
		expect(std::string{text}.substr(0, 6)).to_equal(std::string{"GR BR "});
		expect(std::string{text}.substr(3 * ArcPage::kBoxes)).to_equal(std::string{"|NEXT BREAK 6 BARS"});
		// A peek takes the page's place.
		piece.peek = OledPeek::Recipe;
		expect(piece.screen().arc).to_be_false();
	});

	it("ARC page: its text marks a Drop with *, and the boxes after the piece's end with --", _ {
		Piece piece;
		piece.decideBefore(piece.phraseSteps() + 40);
		piece.pushAndDecide({CommandKind::End, 0, 1});
		char text[kArcPageTextChars + 1];
		describeArcPage(piece.screen().arcPage, text);
		expect(std::string{text}).to_equal(std::string{"GR OU -- -- -- -- -- -- |NEXT OUTRO 6 BARS"});

		Piece building;
		building.decideBefore(building.phraseSteps() + 40);
		building.pushAndDecide(force(SectionKind::Build));
		describeArcPage(building.screen().arcPage, text);
		expect(std::string{text}.find('*') != std::string::npos).to_be_true();
	});
});

CPPSPEC_SPEC(oled_page)
