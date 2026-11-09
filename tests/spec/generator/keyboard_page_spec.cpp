// The KEYBOARD page (spec §13.5): the macro faders on columns 1–8, the phrase position on column 14, FILL on columns
// 14–16. A fader tap sets a new centre; a hold plays a temporary value that springs back on release, and both reach
// the core as steering, logged where they land.

#include "generative/perform/keyboard_page.h"
#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

constexpr uint32_t kHoldTime = 1000; // in the press times' units

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

/// The SetMacro the driver steers with, as PerformAdapter::holdMacro() and releaseMacro() push it.
Command macroCommand(Macro macro, int32_t units) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(macro), units};
}

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

	it("lights a fader up to its value, with the Arc's level white, on the value's level too", _ {
		expect(macroPadLook(2, 4, 6) == MacroPadLook::Lit).to_be_true();
		expect(macroPadLook(4, 4, 6) == MacroPadLook::Lit).to_be_true();
		expect(macroPadLook(5, 4, 6) == MacroPadLook::Off).to_be_true();
		expect(macroPadLook(6, 4, 6) == MacroPadLook::Arc).to_be_true();
		expect(macroPadLook(4, 4, 4) == MacroPadLook::Arc).to_be_true();
		expect(macroPadLook(4, 4, -1) == MacroPadLook::Lit).to_be_true(); // stopped: no Arc
	});

	it("shows the phrase in quarters, from the top", _ {
		expect(phraseCell(0, 128)).to_equal(0);
		expect(phraseCell(31, 128)).to_equal(0);
		expect(phraseCell(32, 128)).to_equal(1);
		expect(phraseCell(127, 128)).to_equal(3);
	});

	it("keeps a tapped value as the new centre", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Density, 3, macroPoints(36), 100);
		expect(holds.held(Macro::Density)).to_be_true();
		expect(holds.units(Macro::Density)).to_equal(macroPoints(36));
		expect(holds.release(Macro::Density, 3, 100 + kHoldTime - 1, kHoldTime) == MacroFaderHolds::Release::Keep)
		    .to_be_true();
		expect(holds.held(Macro::Density)).to_be_false();
	});

	it("springs a held value back on release", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 14, macroPoints(50), 0xFFFFFF00); // the sample timer wraps while held
		expect(holds.release(Macro::Energy, 14, 0xFFFFFF00 + kHoldTime, kHoldTime)
		       == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
	});

	it("moves a hold to a second pad on its fader, and ends it with the last pad up", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Motion, 2, macroPoints(14), 0);
		holds.press(Macro::Motion, 13, macroPoints(43), 10);
		expect(holds.units(Macro::Motion)).to_equal(macroPoints(43));
		expect(holds.release(Macro::Motion, 2, 20, kHoldTime) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.held(Macro::Motion)).to_be_true();
		expect(holds.held(Macro::Motion, 13)).to_be_true();
		// The hold dates from its first pad.
		expect(holds.release(Macro::Motion, 13, kHoldTime, kHoldTime) == MacroFaderHolds::Release::SpringBack)
		    .to_be_true();
	});

	it("ignores a release whose press it never saw, and keeps the faders apart", _ {
		MacroFaderHolds holds;
		holds.press(Macro::Energy, 0, macroPoints(0), 0);
		expect(holds.release(Macro::Energy, 1, 5, kHoldTime) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.release(Macro::Density, 0, 5, kHoldTime) == MacroFaderHolds::Release::None).to_be_true();
		expect(holds.held(Macro::Energy)).to_be_true();
		expect(holds.held(Macro::Density)).to_be_false();
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
		expect(holds.release(Macro::Energy, 14, 2 * kHoldTime, kHoldTime) == MacroFaderHolds::Release::SpringBack)
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
		expect(holds.release(Macro::Energy, 6, 1, kHoldTime) == MacroFaderHolds::Release::Keep).to_be_true();
		Result result;
		const std::vector<LogEntry> logged =
		    logSteered({{16, macroCommand(Macro::Energy, macroLevelUnits(3))}}, &result);
		expect(logged.size()).to_equal(size_t{1});
		expect(result.generator->recipe().macros[Macro::Energy]).to_equal(macroPoints(21));
	});
});

CPPSPEC_SPEC(keyboard_page)
