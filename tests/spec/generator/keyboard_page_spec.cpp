// The KEYBOARD page (spec §13.5): the macro faders on columns 1–8, the phrase position on column 14, FILL on columns
// 14–16. A fader tap sets a new centre; a hold plays a temporary value that springs back on release, and both reach
// the core as steering, logged where they land. The page is composed from a stepped piece as the Perform view
// composes it, and described as the test port's gen_keyboard text from the same cells.

#include "generative/perform/grid_page.h"
#include "generative/perform/keyboard_page.h"
#include "harness.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

/// Plays Techno from its Groove with `commands` steered in, and returns the log.
std::vector<LogEntry> logSteered(std::vector<std::pair<int32_t, Command>> commands, Result* out = nullptr) {
	static LogEntry entries[16];
	SteeringLog log(entries, 16);
	Run run;
	run.recipe = technoRecipe();
	run.recipe.macros.set(Macro::Energy, macroPoints(25));
	run.skipIntro = true;
	run.log = &log;
	run.steer = steerAt(std::move(commands));
	Result result = play(run, 8);
	std::vector<LogEntry> logged;
	for (int32_t i = 0; i < log.size(); i++) {
		logged.push_back(log.at(i));
	}
	if (out != nullptr) {
		*out = std::move(result);
	}
	return logged;
}

/// The SetMacro the driver steers with, as PerformAdapter::holdMacro(), keepMacroCentre() and springMacroBack() push
/// it.
Command macroCommand(Macro macro, int32_t units) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(macro), units};
}

using Look = KeyboardCell::Look;
constexpr int32_t kEnergyColumn = 0;
constexpr int32_t kMotionColumn = 6;

/// A Techno piece from its Groove, played step by step, with the KEYBOARD page composed as the Perform view does and
/// its pads pressed as the view and the adapter handle them.
struct Piece : StepwisePiece<> {
	Macros centres = technoRecipe().macros;
	MacroFaderHolds holds;
	FillHolds fills;
	bool stopped = false;

	Piece() : StepwisePiece({.seed = 3, .skipIntro = true}) {}
	/// A fader pad down at `now` (ms): the macro plays the pad's value while it is held.
	void pressFader(int32_t x, int32_t row, uint32_t now) {
		const KeyboardPadAt at = resolveKeyboardPad(x, row);
		const int32_t units = macroLevelUnits(at.level);
		holds.press(at.macro, at.pad, units, now);
		push(macroCommand(at.macro, units));
	}
	/// And up: a tap keeps the value as the centre, a hold steers back to the centre.
	void releaseFader(int32_t x, int32_t row, uint32_t now) {
		const KeyboardPadAt at = resolveKeyboardPad(x, row);
		const int32_t units = holds.units(at.macro);
		switch (holds.release(at.macro, at.pad, now, kMacroTapMs)) {
		case MacroFaderHolds::Release::Keep:
			centres.set(at.macro, units);
			break;
		case MacroFaderHolds::Release::SpringBack:
			push(macroCommand(at.macro, centres[at.macro]));
			break;
		case MacroFaderHolds::Release::None:
			break;
		}
	}
	/// A FILL pad down or up, with SHIFT held or not as the first goes down.
	void fillPad(int32_t x, int32_t row, bool down, bool shift = false) {
		if (fills.set(resolveKeyboardPad(x, row).pad, down)) {
			push({CommandKind::SetFill, 0, !down ? kFillReleased : shift ? kFillFreshPick : kFillHeldEntry});
		}
	}
	KeyboardPageInput input() {
		KeyboardPageInput in;
		in.centres = centres;
		in.holds = &holds;
		in.piece = stopped ? nullptr : driver.playing();
		if (const Event* section = lastSection(); section != nullptr && !stopped) {
			in.sounding = true;
			in.section = section->section;
		}
		in.clock = driver.clock();
		in.phraseSteps = phraseSteps();
		in.fill = fills.held();
		return in;
	}
	KeyboardPage page() {
		KeyboardPage out;
		composeKeyboardPage(input(), out);
		return out;
	}
	std::string text() {
		char out[kKeyboardTextChars + 1];
		describeKeyboard(page(), out);
		return out;
	}
	/// A column's pads from the top as letters: A the Arc's level, # lit, . unlit, N the phrase quarter playing,
	/// a space where the pad is dark.
	std::string column(int32_t x) {
		const KeyboardPage composed = page();
		std::string out;
		for (const auto& row : composed.cell) {
			const KeyboardCell& cell = row[x];
			out += cell.look == Look::Off         ? ' '
			       : cell.look == Look::FaderArc  ? 'A'
			       : cell.look == Look::PhraseNow ? 'N'
			       : cell.lit                     ? '#'
			                                      : '.';
		}
		return out;
	}
};

} // namespace

// clang-format off
describe keyboard_page("The KEYBOARD page", $ {
	it("lays out the faders on columns 1–8, FILL on columns 14–16 rows 5–8, and nothing else acts", _ {
		for (int32_t row = 0; row < 8; row++) {
			for (int32_t x = 0; x < 8; x++) {
				expect(keyboardPad(x, row) == KeyboardPad::Fader).to_be_true();
			}
			for (int32_t x = 8; x < 13; x++) {
				expect(keyboardPad(x, row) == KeyboardPad::None).to_be_true();
			}
			for (int32_t x = 13; x < 16; x++) {
				expect(keyboardPad(x, row) == (row >= 4 ? KeyboardPad::Fill : KeyboardPad::None)).to_be_true();
			}
		}
	});

	it("gives each macro two columns, in Energy, Density, Complexity, Motion order", _ {
		expect(faderMacro(0) == Macro::Energy).to_be_true();
		expect(faderMacro(1) == Macro::Energy).to_be_true();
		expect(faderMacro(2) == Macro::Density).to_be_true();
		expect(faderMacro(5) == Macro::Complexity).to_be_true();
		expect(faderMacro(7) == Macro::Motion).to_be_true();
	});

	it("numbers a fader's sixteen pads apart, the bottom row first", _ {
		expect(macroFaderPad(0, 0)).to_equal(0);
		expect(macroFaderPad(1, 0)).to_equal(1);
		expect(macroFaderPad(2, 0)).to_equal(0); // Density's first column
		expect(macroFaderPad(7, 7)).to_equal(15);
	});

	it("resolves a pad once for its press, its release and its hold", _ {
		const KeyboardPadAt top = resolveKeyboardPad(5, 0); // Complexity's second column, the top row
		expect(top.kind == KeyboardPad::Fader).to_be_true();
		expect(top.macro == Macro::Complexity).to_be_true();
		expect(top.level).to_equal(7);
		expect(top.pad).to_equal(15);
		const KeyboardPadAt bottom = resolveKeyboardPad(0, 7);
		expect(bottom.macro == Macro::Energy).to_be_true();
		expect(bottom.level).to_equal(0);
		expect(bottom.pad).to_equal(0);
		const KeyboardPadAt fill = resolveKeyboardPad(14, 6);
		expect(fill.kind == KeyboardPad::Fill).to_be_true();
		expect(fill.pad).to_equal(FillHolds::padHolder(fillPad(14, 6)));
		expect(fill.pad == FillHolds::kButton).to_be_false();
		expect(resolveKeyboardPad(13, 0).kind == KeyboardPad::None).to_be_true(); // the phrase position
	});

	it("numbers the twelve FILL pads apart", _ {
		expect(fillPad(13, 4)).to_equal(0);
		expect(fillPad(15, 4)).to_equal(2);
		expect(fillPad(13, 5)).to_equal(3);
		expect(fillPad(15, 7)).to_equal(11);
	});

	it("spans 0 to 50 over a fader's eight rows, the bottom row 0 and the top 50", _ {
		expect(macroLevelUnits(0)).to_equal(0);
		expect(macroLevelUnits(7)).to_equal(macroPoints(50));
		expect(macroLevelUnits(3)).to_equal(macroPoints(21));
		expect(macroLevelUnits(4)).to_equal(macroPoints(29));
		expect(macroLevel(macroPoints(25))).to_equal(4); // 3.5 rounds up
		expect(macroLevel(macroPoints(24))).to_equal(3);
		expect(macroLevel(0)).to_equal(0);
		expect(macroLevel(kMacroFull)).to_equal(7);
		for (int32_t level = 0; level < kMacroFaderLevels; level++) {
			expect(macroLevel(macroLevelUnits(level))).to_equal(level);
		}
	});

	it("composes the faders on columns 1–8 by macro, the phrase column and FILL, the rest dark", _ {
		Piece piece;
		piece.runToStepLine(4);
		const KeyboardPage page = piece.page();
		for (int32_t row = 0; row < KeyboardPage::kRows; row++) {
			for (int32_t x = 0; x < 8; x++) {
				const KeyboardCell& cell = page.cell[row][x];
				expect(cell.look == Look::Fader || cell.look == Look::FaderArc).to_be_true();
				expect(cell.macro == static_cast<Macro>(x / 2)).to_be_true();
				expect(cell.held).to_be_false();
			}
			for (int32_t x = 8; x < 13; x++) {
				expect(page.cell[row][x].look).to_equal(Look::Off);
			}
			const bool top = row < 4;
			expect(page.cell[row][13].look == (top ? row == 0 ? Look::PhraseNow : Look::Phrase : Look::Fill)).to_be_true();
			expect(page.cell[row][14].look).to_equal(top ? Look::Off : Look::Fill);
			expect(page.cell[row][15].look).to_equal(top ? Look::Off : Look::Fill);
		}
		expect(page.section).to_equal(SectionKind::Groove);
	});

	it("lights a fader up to its centre, with the Arc's level white, on the centre's level too", _ {
		Piece piece;
		piece.runToStepLine(4);
		// Techno's centres are 25 of 50, level 5 of 8. In the Groove the Arc has every macro at its centre.
		expect(piece.column(kEnergyColumn)).to_equal(std::string("...A####"));
		expect(piece.column(kEnergyColumn + 1)).to_equal(std::string("...A####")); // a fader is two columns wide
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=1 fill=0"));
	});

	it("shows the Arc's level apart from the centre's when the Arc moves the macro", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		piece.runToStepLine(piece.phraseSteps() + 4);
		// The Break takes Energy down: its white pad is below the centre's level, on a lit pad.
		expect(piece.column(kEnergyColumn)).to_equal(std::string("...##A##"));
		expect(piece.text().substr(0, 6)).to_equal(std::string("E=5/3 "));
		expect(piece.page().section).to_equal(SectionKind::Break);
	});

	it("shows a held fader's value, brighter, while it is held, and springs back when a hold is let go", _ {
		Piece piece;
		piece.runToStepLine(4);
		piece.pressFader(kMotionColumn, 0, 1000); // the top pad: 50
		expect(piece.column(kMotionColumn)).to_equal(std::string("###A####"));
		expect(piece.page().cell[0][kMotionColumn].held).to_be_true();
		expect(piece.page().cell[7][kMotionColumn + 1].held).to_be_true();
		expect(piece.page().cell[0][kEnergyColumn].held).to_be_false();
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=8h/5 phrase=1 fill=0"));
		piece.runSteps(2); // the held value landed: the Arc has Motion there
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=8h/8 phrase=1 fill=0"));
		piece.releaseFader(kMotionColumn, 0, 1000 + kMacroTapMs);
		expect(piece.page().cell[0][kMotionColumn].held).to_be_false();
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/8 phrase=1 fill=0"));
		piece.runSteps(2);
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=1 fill=0"));
	});

	it("keeps a tapped fader's value as its centre", _ {
		Piece piece;
		piece.runToStepLine(4);
		piece.pressFader(kEnergyColumn + 1, 6, 1000); // the second row from the bottom
		expect(piece.text()).to_equal(std::string("E=2h/5 D=5/5 C=5/5 M=5/5 phrase=1 fill=0"));
		piece.releaseFader(kEnergyColumn + 1, 6, 1000 + kMacroTapMs - 1);
		expect(piece.column(kEnergyColumn)).to_equal(std::string("...A..##"));
		piece.runSteps(2);
		expect(piece.column(kEnergyColumn)).to_equal(std::string("......A#"));
		expect(piece.text()).to_equal(std::string("E=2/2 D=5/5 C=5/5 M=5/5 phrase=1 fill=0"));
	});

	it("shows the phrase in quarters from the top, in the Section's colour, the ones passed lit", _ {
		Piece piece;
		expect(piece.column(kPhraseColumn)).to_equal(std::string("    ....")); // nothing heard yet
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=- fill=0"));
		piece.runToStepLine(0);
		expect(piece.column(kPhraseColumn)).to_equal(std::string("N.......")); // rows 5–8 are FILL, unlit
		piece.runToStepLine(31);
		expect(piece.column(kPhraseColumn)).to_equal(std::string("N......."));
		piece.runToStepLine(32);
		expect(piece.column(kPhraseColumn)).to_equal(std::string("#N......"));
		expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=2 fill=0"));
		piece.runToStepLine(piece.phraseSteps() - 1);
		expect(piece.column(kPhraseColumn)).to_equal(std::string("###N...."));
		piece.runToStepLine(piece.phraseSteps());
		expect(piece.column(kPhraseColumn)).to_equal(std::string("N......."));
	});

	it("lights every FILL pad while FILL is held, with or without SHIFT", _ {
		for (bool shift : {false, true}) {
			Piece piece;
			piece.runToStepLine(4);
			expect(piece.column(14)).to_equal(std::string("    ...."));
			piece.fillPad(14, 5, true, shift);
			piece.decideBefore(8);
			// SHIFT plays a fresh pick (spec §8.7); the pads show the same.
			expect(piece.driver.playing()->heldFill() >= 0).to_be_true();
			for (int32_t x = 13; x < 16; x++) {
				expect(piece.column(x).substr(4)).to_equal(std::string("####"));
			}
			expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=1 fill=1"));
			piece.fillPad(15, 7, true); // a second pad: still held when the first is let go
			piece.fillPad(14, 5, false);
			expect(piece.column(13).substr(4)).to_equal(std::string("####"));
			piece.fillPad(15, 7, false);
			expect(piece.column(13).substr(4)).to_equal(std::string("...."));
			expect(piece.text()).to_equal(std::string("E=5/5 D=5/5 C=5/5 M=5/5 phrase=1 fill=0"));
		}
	});

	it("shows no Arc level and no phrase while stopped; the faders keep their centres", _ {
		Piece piece;
		piece.runToStepLine(40);
		piece.stopped = true;
		expect(piece.column(kEnergyColumn)).to_equal(std::string("...#####"));
		expect(piece.column(kPhraseColumn)).to_equal(std::string("    ...."));
		expect(piece.text()).to_equal(std::string("E=5/- D=5/- C=5/- M=5/- phrase=- fill=0"));
	});

	it("fits its longest text", _ {
		Piece piece;
		piece.runToStepLine(4);
		for (int32_t x = 0; x < 8; x += 2) {
			piece.pressFader(x, 0, 0);
		}
		piece.fillPad(13, 4, true);
		expect(piece.text()).to_equal(std::string("E=8h/5 D=8h/5 C=8h/5 M=8h/5 phrase=1 fill=1"));
	});

	it("leaves the phrase column dark when the input has no phrase length", _ {
		Piece piece;
		piece.runToStepLine(4);
		KeyboardPageInput in = piece.input();
		in.phraseSteps = 0;
		KeyboardPage page;
		composeKeyboardPage(in, page);
		for (int32_t c = 0; c < kPhraseCells; c++) {
			expect(page.cell[c][kPhraseColumn].look).to_equal(Look::Off);
		}
	});

	it("opens the sound editor with SHIFT on the faders and the phrase column, and leaves FILL to a fresh pick", _ {
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 0, 0)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 7, 7)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, kPhraseColumn, 0)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 8, 5)).to_be_true();
		for (int32_t row = kFillFirstRow; row < kFillFirstRow + kFillRows; row++) {
			for (int32_t x = kFillFirstColumn; x < kFillFirstColumn + kFillColumns; x++) {
				expect(shiftOpensSoundEditor(GridPage::Keyboard, x, row)).to_be_false();
			}
		}
	});

	it("opens the sound editor with SHIFT on any pad of the SONG page (spec §13.7)", _ {
		for (int32_t row = 0; row < KeyboardPage::kRows; row++) {
			for (int32_t x = 0; x < KeyboardPage::kColumns; x++) {
				expect(shiftOpensSoundEditor(GridPage::Song, x, row)).to_be_true();
			}
		}
	});

	it("keeps a tapped value as the new centre", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Density, 3, macroPoints(36), 100);
		expect(holds.held(Macro::Density)).to_be_true();
		expect(holds.units(Macro::Density)).to_equal(macroPoints(36));
		expect(holds.release(Macro::Density, 3, 100 + kMacroTapMs - 1, kMacroTapMs) == MacroFaderHolds::Release::Keep)
		    .to_be_true();
		expect(holds.held(Macro::Density)).to_be_false();
	});

	it("tells a tap from a hold at 300 ms (the user's choice; stock's 100 ms default was too short)", _ {
		expect(kMacroTapMs).to_equal(300U);
		MacroFaderHolds holds;
		holds.press(Macro::Complexity, 0, macroPoints(0), 0);
		expect(holds.release(Macro::Complexity, 0, 299, kMacroTapMs) == MacroFaderHolds::Release::Keep).to_be_true();
		holds.press(Macro::Complexity, 0, macroPoints(0), 0);
		expect(holds.release(Macro::Complexity, 0, 300, kMacroTapMs) == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
	});

	it("springs a held value back on release", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 14, macroPoints(50), 0xFFFFFF00); // the sample timer wraps while held
		expect(holds.release(Macro::Energy, 14, 0xFFFFFF00 + kMacroTapMs, kMacroTapMs)
		       == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
	});

	it("moves a hold to a second pad on its fader, and ends it with the last pad up", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Motion, 2, macroPoints(14), 0);
		holds.press(Macro::Motion, 13, macroPoints(43), 10);
		expect(holds.units(Macro::Motion)).to_equal(macroPoints(43));
		expect(holds.release(Macro::Motion, 2, 20, kMacroTapMs) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.held(Macro::Motion)).to_be_true();
		expect(holds.held(Macro::Motion, 13)).to_be_true();
		// The hold dates from its first pad.
		expect(holds.release(Macro::Motion, 13, kMacroTapMs, kMacroTapMs) == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
	});

	it("ignores a release whose press it never saw, and keeps the faders apart", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 0, macroPoints(0), 0);
		expect(holds.release(Macro::Energy, 1, 5, kMacroTapMs) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.release(Macro::Density, 0, 5, kMacroTapMs) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.held(Macro::Energy)).to_be_true();
		expect(holds.held(Macro::Density)).to_be_false();
	});

	it("steers a normal tap once: the press sent its value", _ {
		MacroSteering steering;
		steering.startFrom(Macros{}); // every macro at 25
		steering.hold(Macro::Energy, macroPoints(50));
		steering.sent(Macro::Energy, macroPoints(50));
		steering.letGo(Macro::Energy);
		expect(steering.differs(Macro::Energy, macroPoints(50))).to_be_false();
	});

	it("keeps a held fader's value through a knob turn and an instance start, then springs back to the knob's centre", _ {
		// As PerformAdapter steers: a knob turn sends only while centreSteers(); a release springs back to the centre.
		MacroSteering steering;
		steering.startFrom(Macros{}); // Density at 25
		steering.hold(Macro::Density, macroPoints(50));
		steering.sent(Macro::Density, macroPoints(50));
		expect(steering.centreSteers(Macro::Density)).to_be_false(); // the knob's 10 moves only the centre
		expect(steering.centreSteers(Macro::Energy)).to_be_true();
		// A Regenerate lands mid-hold: the instance carries the held 50, so nothing is sent again.
		expect(steering.resend(Macro::Density)).to_equal(MacroSteering::kNotHeld);
		steering.letGo(Macro::Density);
		expect(steering.centreSteers(Macro::Density)).to_be_true();
		steering.sent(Macro::Density, macroPoints(10)); // the spring back sends the knob's 10
		expect(steering.differs(Macro::Density, macroPoints(10))).to_be_false();
	});

	it("lets a tap's value win over a knob turn during it, sending it once", _ {
		MacroSteering steering;
		steering.startFrom(Macros{});
		steering.hold(Macro::Energy, macroPoints(50));
		steering.sent(Macro::Energy, macroPoints(50));
		expect(steering.centreSteers(Macro::Energy)).to_be_false(); // the knob sends nothing
		steering.letGo(Macro::Energy); // the tap keeps 50 as the centre
		expect(steering.differs(Macro::Energy, macroPoints(50))).to_be_false();
	});

	it("sends a held fader again when a piece starts, so a tap across a start keeps its value", _ {
		MacroSteering steering;
		steering.startFrom(Macros{});
		steering.hold(Macro::Complexity, macroPoints(50));
		steering.sent(Macro::Complexity, macroPoints(50));
		expect(steering.resend(Macro::Complexity)).to_equal(MacroSteering::kNotHeld); // the core has it
		steering.startFrom(Macros{}); // PLAY: the queue is dropped, the piece plays 25
		expect(steering.resend(Macro::Complexity)).to_equal(macroPoints(50));
		expect(steering.resend(Macro::Energy)).to_equal(MacroSteering::kNotHeld); // not held
		steering.sent(Macro::Complexity, macroPoints(50));
		steering.letGo(Macro::Complexity); // let go as a tap
		expect(steering.differs(Macro::Complexity, macroPoints(50))).to_be_false();
	});

	it("steers a tap's value after a start when nothing sent it again", _ {
		MacroSteering steering;
		steering.hold(Macro::Motion, macroPoints(43));
		steering.sent(Macro::Motion, macroPoints(43));
		steering.startFrom(Macros{});
		steering.letGo(Macro::Motion);
		expect(steering.differs(Macro::Motion, macroPoints(43))).to_be_true();
		expect(steering.resend(Macro::Motion)).to_equal(MacroSteering::kNotHeld); // let go: nothing to send again
	});

	it("holds FILL while the button or any FILL pad is down, turning it on and off only once", _ {
		FillHolds fill;
		expect(fill.set(FillHolds::padHolder(4), true)).to_be_true();
		expect(fill.set(FillHolds::kButton, true)).to_be_false();
		expect(fill.set(FillHolds::padHolder(4), false)).to_be_false();
		expect(fill.held()).to_be_true();
		expect(fill.set(FillHolds::kButton, false)).to_be_true();
		expect(fill.held()).to_be_false();
		expect(fill.set(FillHolds::kButton, false)).to_be_false(); // a release it never saw go down
		expect(fill.set(FillHolds::padHolder(11), true)).to_be_true();
		expect(fill.held(FillHolds::padHolder(11))).to_be_true();
	});

	it("logs a held fader as steering both ways: the held value as it lands, the centre on release", _ {
		// Energy's centre is 25. Pad 14 (the top row) held from step 16 to step 48, well past the hold time.
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 14, macroLevelUnits(7), 0);
		const Command held = macroCommand(Macro::Energy, holds.units(Macro::Energy));
		expect(holds.release(Macro::Energy, 14, 2 * kMacroTapMs, kMacroTapMs) == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
		const Command back = macroCommand(Macro::Energy, macroPoints(25));
		Result result;
		const std::vector<LogEntry> logged = logSteered({{16, held}, {48, back}}, &result);
		expect(logged.size()).to_equal(size_t{2});
		expect(logged[0].step).to_equal(16);
		expect(logged[0].command == held).to_be_true();
		expect(logged[1].step).to_equal(48);
		expect(logged[1].command == back).to_be_true();
		expect(result.generator->recipe().macros[Macro::Energy]).to_equal(macroPoints(25));
	});

	it("logs a tap once: the value it set stays the centre", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 6, macroLevelUnits(3), 0);
		expect(holds.release(Macro::Energy, 6, 1, kMacroTapMs) == MacroFaderHolds::Release::Keep).to_be_true();
		Result result;
		const std::vector<LogEntry> logged =
		    logSteered({{16, macroCommand(Macro::Energy, macroLevelUnits(3))}}, &result);
		expect(logged.size()).to_equal(size_t{1});
		expect(result.generator->recipe().macros[Macro::Energy]).to_equal(macroPoints(21));
	});
});

CPPSPEC_SPEC(keyboard_page)
