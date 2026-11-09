// MIDI in Perform (spec §13.7): CCs 114–117 on the MIDI-follow channels steer the macros, 0–127 over 0–50, and
// get feedback; TRANSPOSE keeps the scale; live-value feedback sends only what changed since it was last sent.

#include "generative/perform/midi_in.h"

#include "cppspec.hpp"

using namespace deluge::generative;
using namespace deluge::generative::perform;

// clang-format off
describe midi_in("Perform's MIDI in", $ {
	it("steers Energy, Density, Complexity and Motion from CCs 114 to 117, and nothing from the CCs around them", _ {
		expect(macroForCC(114) == Macro::Energy).to_be_true();
		expect(macroForCC(115) == Macro::Density).to_be_true();
		expect(macroForCC(116) == Macro::Complexity).to_be_true();
		expect(macroForCC(117) == Macro::Motion).to_be_true();
		expect(macroForCC(113).has_value()).to_be_false();
		expect(macroForCC(118).has_value()).to_be_false();
		expect(macroForCC(1).has_value()).to_be_false();
	});

	it("names the macro's CC, for its feedback", _ {
		expect(macroCC(Macro::Energy)).to_equal(114);
		expect(macroCC(Macro::Motion)).to_equal(117);
	});

	it("scales a CC's 0–127 over the macro's 0–50", _ {
		expect(macroUnitsFromCC(0)).to_equal(0);
		expect(macroUnitsFromCC(127)).to_equal(macroPoints(50));
		expect(macroToPoints(macroUnitsFromCC(64))).to_equal(25);
		expect(macroUnitsFromCC(1)).to_equal(25); // 3200 / 127 = 25.2
	});

	it("sends a macro back as the CC value that set it", _ {
		for (int32_t value = 0; value <= 127; value++) {
			expect(macroCCValue(macroUnitsFromCC(value))).to_equal(value);
		}
		expect(macroCCValue(macroPoints(25))).to_equal(64); // 63.5 rounds up
		expect(macroCCValue(0)).to_equal(0);
		expect(macroCCValue(macroPoints(50))).to_equal(127);
	});

	it("turns a TRANSPOSE note into a key on its pitch class, keeping the scale", _ {
		const Key ahead{2, Scale::Dorian};
		expect(transposedKey(ahead, 60) == Key{0, Scale::Dorian}).to_be_true();
		expect(transposedKey(ahead, 67) == Key{7, Scale::Dorian}).to_be_true();
		expect(transposedKey(ahead, 11) == Key{11, Scale::Dorian}).to_be_true();
		expect(transposedKey(ahead, 127) == Key{7, Scale::Dorian}).to_be_true();
	});

	context("feedback filter", _ {
		it("sends every CC's first value", _ {
			FeedbackFilter filter;
			expect(filter.changed(7, 100)).to_be_true();
			expect(filter.changed(74, 0)).to_be_true();
		});

		it("drops a value already sent for that CC, and sends one that moved", _ {
			FeedbackFilter filter;
			filter.changed(74, 40);
			expect(filter.changed(74, 40)).to_be_false();
			expect(filter.changed(74, 41)).to_be_true();
			expect(filter.changed(74, 41)).to_be_false();
		});

		it("keeps each CC apart", _ {
			FeedbackFilter filter;
			filter.changed(74, 40);
			expect(filter.changed(71, 40)).to_be_true();
		});

		it("sends everything again once forgotten, as on a change of what the gold knobs point at", _ {
			FeedbackFilter filter;
			filter.changed(74, 40);
			filter.changed(114, 64);
			filter.forget();
			expect(filter.changed(74, 40)).to_be_true();
			expect(filter.changed(114, 64)).to_be_true();
		});

		it("takes the top knob position, 128", _ {
			FeedbackFilter filter;
			expect(filter.changed(10, 128)).to_be_true();
			expect(filter.changed(10, 128)).to_be_false();
		});
	});
});

CPPSPEC_SPEC(midi_in)
