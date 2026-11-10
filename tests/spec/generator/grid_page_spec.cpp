// Perform's grid pages (spec §13.5, §13.7): SHIFT + a main pad opens the sound editor at the printed param, except on
// the pads where SHIFT already means something.

#include "generative/perform/clip_page.h"
#include "generative/perform/grid_page.h"
#include "generative/perform/keyboard_page.h"

#include "cppspec.hpp"

using namespace deluge::generative::perform;

// clang-format off
describe grid_page("SHIFT + a main pad", $ {
	it("opens the sound editor anywhere on the SONG page", _ {
		for (int32_t row = 0; row < 8; row++) {
			for (int32_t x = 0; x < 16; x++) {
				expect(shiftOpensSoundEditor(GridPage::Song, x, row)).to_be_true();
			}
		}
	});

	it("leaves the KEYBOARD page's FILL pads to a fresh pick", _ {
		for (int32_t row = kFillFirstRow; row < kFillFirstRow + kFillRows; row++) {
			for (int32_t x = kFillFirstColumn; x < kFillFirstColumn + kFillColumns; x++) {
				expect(shiftOpensSoundEditor(GridPage::Keyboard, x, row)).to_be_false();
			}
		}
	});

	it("opens the sound editor on the KEYBOARD page's faders and phrase column", _ {
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 0, 0)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 7, 7)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, kPhraseColumn, 0)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Keyboard, 8, 5)).to_be_true();
	});

	it("leaves the CLIP page's NEW SOUND to the previous sound, and SOUND EDITOR to its root menu", _ {
		expect(shiftOpensSoundEditor(GridPage::Clip, kNewSoundPad, kActionsRow)).to_be_false();
		expect(shiftOpensSoundEditor(GridPage::Clip, kSoundEditorPad, kActionsRow)).to_be_false();
	});

	it("opens the sound editor on the CLIP page's takes, FREEZE and faders", _ {
		expect(shiftOpensSoundEditor(GridPage::Clip, 0, kTakesRow)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Clip, kFreezePad, kActionsRow)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Clip, 3, kActionsRow)).to_be_true();
		expect(shiftOpensSoundEditor(GridPage::Clip, 8, 7)).to_be_true();
	});
});

CPPSPEC_SPEC(grid_page)
