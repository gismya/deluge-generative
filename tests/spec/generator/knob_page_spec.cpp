// Which F page the gold knobs show for a focused part (spec §13.5): perform/knob_page.h. The binding itself is the
// firmware's (PerformView::bindKnobs()), which perform_retire.txt and perform_homes.txt check as wiring.

#include "generative/perform/knob_page.h"

#include "cppspec.hpp"

namespace {

namespace perform = deluge::generative::perform;

constexpr uint8_t kF1 = 0;
constexpr uint8_t kF2 = 1;
constexpr uint8_t kF7 = 6;

// clang-format off
describe knob_page("Knob page", $ {
	it("keeps Thin / Drive on its part when the part's Output comes up on another page (a sound swap)", _ {
		// ACID (part 1) had Thin / Drive; its new Output's own page is F2.
		expect(perform::thinDriveAfterBind(1, 1) == 1).to_be_true();
		expect(perform::knobPage(1, 1, kF2) == kF7).to_be_true();
	});

	it("leaves a part without Thin / Drive on its Output's own page", _ {
		expect(perform::knobPage(1, -1, kF2) == kF2).to_be_true();
		expect(perform::knobPage(1, -1, kF7) == kF7).to_be_true(); // F7 alone: stutter / CUSTOM 1
	});

	it("ends Thin / Drive when focus moves to another part or to the song, which keep their own page", _ {
		expect(perform::thinDriveAfterBind(0, 1) == -1).to_be_true();
		expect(perform::thinDriveAfterBind(-1, 1) == -1).to_be_true();
		expect(perform::knobPage(0, perform::thinDriveAfterBind(0, 1), kF2) == kF2).to_be_true();
		expect(perform::knobPage(-1, perform::thinDriveAfterBind(-1, 1), kF1) == kF1).to_be_true();
	});

	it("never turns Thin / Drive on by itself", _ {
		expect(perform::thinDriveAfterBind(1, -1) == -1).to_be_true();
		expect(perform::thinDriveAfterBind(-1, -1) == -1).to_be_true();
	});
});
// clang-format on

} // namespace

CPPSPEC_SPEC(knob_page)
