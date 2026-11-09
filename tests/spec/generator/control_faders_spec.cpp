// The CLIP page's sound-control faders (spec §13.5): rows 3–8 hold Brightness, Thin, Decay, Space, Drive and Wobble
// across the 16 pads. Tapping a pad sets home; the pads are lit up to home, and the white pad is the live value.

#include "generative/perform/control_faders.h"

#include "cppspec.hpp"

using namespace deluge::generative;
using namespace deluge::generative::perform;

// clang-format off
describe control_faders("The CLIP page's control faders", $ {
	it("hold the controls in the spec's order from row 3 down", _ {
		expect(faderControl(2) == SoundControl::Brightness).to_be_true();
		expect(faderControl(3) == SoundControl::Thin).to_be_true();
		expect(faderControl(4) == SoundControl::Decay).to_be_true();
		expect(faderControl(5) == SoundControl::Space).to_be_true();
		expect(faderControl(6) == SoundControl::Drive).to_be_true();
		expect(faderControl(7) == SoundControl::Wobble).to_be_true();
	});

	it("span a param's whole knob range, pad 1 the bottom and pad 16 the top", _ {
		expect(faderKnobPos(0)).to_equal(-64);
		expect(faderKnobPos(15)).to_equal(64);
		expect(faderPad(-64)).to_equal(0);
		expect(faderPad(64)).to_equal(15);
		expect(faderPad(0)).to_equal(8); // a knob's middle rounds up
		expect(faderPad(-60)).to_equal(0);
		expect(faderPad(60)).to_equal(15);
	});

	it("show a tapped pad back on that pad", _ {
		for (int32_t pad = 0; pad < kFaderPads; pad++) {
			expect(faderPad(faderKnobPos(pad))).to_equal(pad);
		}
	});

	it("light the pads up to home, the live value's pad white, on home's pad too (spec §13.5)", _ {
		expect(faderLook(3, 5, 5) == FaderLook::Home).to_be_true();
		expect(faderLook(5, 5, 5) == FaderLook::Live).to_be_true();
		expect(faderLook(6, 5, 5) == FaderLook::Off).to_be_true();
		expect(faderLook(9, 5, 9) == FaderLook::Live).to_be_true();
		expect(faderLook(2, 5, 2) == FaderLook::Live).to_be_true();
		expect(faderLook(5, 5, 2) == FaderLook::Home).to_be_true();
	});
});

CPPSPEC_SPEC(control_faders)
