// The driver-side mapping of a sound control onto params (spec §11.2): which targets each engine's controls move,
// with what range in menu units, and the value a target takes for an offset from its home.

#include "generative/perform/sound_mapping.h"

#include "cppspec.hpp"

#include <cstdint>

using namespace deluge::generative;
using namespace deluge::generative::perform;

namespace {

/// A standard param's value at a menu value (0–50), as the stock menus write it.
int32_t menu(int32_t value) {
	return value >= 50 ? INT32_MAX : static_cast<int32_t>(int64_t{value} * 85899345 - 2147483648LL);
}

/// The range a control gives `target` on `engine`, if it moves it.
struct Found {
	int32_t low = 0;
	int32_t high = 0;
	bool found = false;
};
Found rangeOf(SoundControl control, Engine engine, Target target) {
	for (const TargetRange& range : controlTargets(control, engine)) {
		if (range.target == target) {
			return {range.low, range.high, true};
		}
	}
	return {};
}

} // namespace

// clang-format off
describe sound_mapping("The sound-control mapping", $ {
	context("on a subtractive synth", _ {
		it("moves the LPF ±8 and the wave index ±5 with Brightness", _ {
			Found lpf = rangeOf(SoundControl::Brightness, Engine::Subtractive, Target::LpfFrequency);
			expect(lpf.found && lpf.low == -8 && lpf.high == 8).to_be_true();
			Found wave = rangeOf(SoundControl::Brightness, Engine::Subtractive, Target::WaveIndex);
			expect(wave.found && wave.low == -5 && wave.high == 5).to_be_true();
		});
		it("only raises Drive's ladder morph, 0..+15", _ {
			Found morph = rangeOf(SoundControl::Drive, Engine::Subtractive, Target::LpfMorph);
			expect(morph.found && morph.low == 0 && morph.high == 15).to_be_true();
		});
		it("moves delay feedback ±4 only where the preset already uses the delay", _ {
			bool found = false;
			for (const TargetRange& range : controlTargets(SoundControl::Space, Engine::Subtractive)) {
				if (range.target == Target::DelayFeedback) {
					found = true;
					expect(range.needs == Needs::DelayInUse).to_be_true();
				}
			}
			expect(found).to_be_true();
		});
	});

	context("per engine", _ {
		it("gives FM Brightness to the modulators in use, and FM no Thin", _ {
			Found mod = rangeOf(SoundControl::Brightness, Engine::Fm, Target::Modulator1Level);
			expect(mod.found && mod.low == -6 && mod.high == 6).to_be_true();
			expect(rangeOf(SoundControl::Brightness, Engine::Fm, Target::LpfFrequency).found).to_be_false();
			expect(controlTargets(SoundControl::Thin, Engine::Fm).empty()).to_be_true();
		});
		it("only cuts on DX7 and kit rows (spec §11.2's engine notes)", _ {
			Found dx = rangeOf(SoundControl::Brightness, Engine::Dx7, Target::LpfFrequency);
			expect(dx.found && dx.low == -10 && dx.high == 0).to_be_true();
			for (Engine engine : {Engine::Dx7, Engine::KitRow}) {
				for (const TargetRange& range : controlTargets(SoundControl::Decay, engine)) {
					expect(range.high).to_equal(int8_t{0});
				}
			}
			Found row = rangeOf(SoundControl::Brightness, Engine::KitRow, Target::LpfFrequency);
			expect(row.found && row.low == -15 && row.high == 0).to_be_true();
		});
		it("darkens and thins a whole Kit through its affect-entire filter", _ {
			Found lpf = rangeOf(SoundControl::Brightness, Engine::KitLevel, Target::LpfFrequency);
			expect(lpf.found && lpf.low == -20 && lpf.high == 0).to_be_true();
			Found hpf = rangeOf(SoundControl::Thin, Engine::KitLevel, Target::HpfFrequency);
			expect(hpf.found && hpf.low == 0 && hpf.high == 25).to_be_true();
		});
	});

	context("a target's value", _ {
		const TargetRange lpf{Target::LpfFrequency, -8, 8};
		const TargetRange morph{Target::LpfMorph, 0, 15};
		const TargetRange cables{Target::LfoCables, 0, 0};

		it("is its home at offset 0", _ { expect(targetValue(lpf, menu(30), 0)).to_equal(menu(30)); });
		it("reaches the end of the range at ±kControlFull", _ {
			expect(targetValue(lpf, menu(30), kControlFull)).to_equal(menu(38));
			expect(targetValue(lpf, menu(30), -kControlFull)).to_equal(menu(22));
		});
		it("moves in proportion between", _ { expect(targetValue(lpf, menu(30), -50)).to_equal(menu(26)); });
		it("stays at home on the side a range doesn't reach", _ {
			expect(targetValue(morph, menu(10), -kControlFull)).to_equal(menu(10));
			expect(targetValue(morph, menu(10), 60)).to_equal(menu(19));
		});
		it("clamps to the param's own range", _ {
			expect(targetValue(lpf, menu(48), kControlFull)).to_equal(INT32_MAX);
			expect(targetValue(lpf, menu(3), -kControlFull)).to_equal(INT32_MIN);
		});
		it("scales a cable amount ×0..×2, within a cable's ±2^30", _ {
			expect(targetValue(cables, 1 << 28, kControlFull)).to_equal(1 << 29);
			expect(targetValue(cables, 1 << 28, -kControlFull)).to_equal(0);
			expect(targetValue(cables, -(1 << 28), 50)).to_equal(-(3 << 27));
			expect(targetValue(cables, 3 << 28, kControlFull)).to_equal(1 << 30);
		});
	});
});

CPPSPEC_SPEC(sound_mapping)
