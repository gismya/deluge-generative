// The driver-side mapping of a sound control onto params (spec §11.2): which targets each engine's controls move,
// with what range in menu units, and the value a target takes for an offset from its home.

#include "generative/perform/sound_mapping.h"

#include "cppspec.hpp"

#include <algorithm>
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
			expect(controlTargets(SoundControl::Drive, Engine::Subtractive).size()).to_equal(size_t{1});
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
		it("never moves the wavefolder: the stock folder drops the level about 4 dB as it switches in at 1 (#516)", _ {
			for (int32_t engine = 0; engine < mapping::kNumEngines; engine++) {
				for (int32_t control = 0; control < kNumSoundControls; control++) {
					for (const TargetRange& range :
					     controlTargets(static_cast<SoundControl>(control), static_cast<Engine>(engine))) {
						expect(range.target == Target::Fold).to_be_false();
					}
				}
			}
		});
		it("gives FM Drive the modulator and carrier feedback, 0..+8, and DX7 no Drive", _ {
			Found modulator = rangeOf(SoundControl::Drive, Engine::Fm, Target::ModulatorFeedback);
			expect(modulator.found && modulator.low == 0 && modulator.high == 8).to_be_true();
			Found carrier = rangeOf(SoundControl::Drive, Engine::Fm, Target::CarrierFeedback);
			expect(carrier.found && carrier.low == 0 && carrier.high == 8).to_be_true();
			expect(controlTargets(SoundControl::Drive, Engine::Fm).size()).to_equal(size_t{2});
			expect(controlTargets(SoundControl::Drive, Engine::Dx7).empty()).to_be_true();
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
		const TargetRange cables{Target::LfoCables, -50, 50}; // the table's: ×0..×2

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
			expect(targetValue(cables, -(1 << 28), 50)).to_equal(-(3 << 27));
			expect(targetValue(cables, 3 << 28, kControlFull)).to_equal(1 << 30);
		});
		it("takes Wobble's cable range from the table: ±50, in 2% steps of the cable's home", _ {
			for (Engine engine : {Engine::Subtractive, Engine::Fm, Engine::Ringmod, Engine::Dx7}) {
				Found found = rangeOf(SoundControl::Wobble, engine, Target::LfoCables);
				expect(found.found && found.low == -50 && found.high == 50).to_be_true();
			}
		});
		it("keeps a cable at ×0 one step from 0, on its home's side: the firmware deletes a cable set to 0", _ {
			expect(targetValue(cables, 1 << 28, -kControlFull)).to_equal(1);
			expect(targetValue(cables, -(1 << 28), -kControlFull)).to_equal(-1);
			expect(targetValue(cables, 50, -99)).to_equal(1);
			expect(targetValue(cables, 0, 0)).to_equal(0);
		});
	});

	context("a preset's range (spec §11.1: a palette entry's percent of the default ranges)", _ {
		const TargetRange lpf{Target::LpfFrequency, -8, 8, Needs::LpfInUse};
		const TargetRange morph{Target::LpfMorph, 0, 15};
		const TargetRange cables{Target::LfoCables, -50, 50};
		it("keeps the default at 100", _ {
			TargetRange same = scaledRange(lpf, 100);
			expect(same.target == Target::LpfFrequency && same.needs == Needs::LpfInUse).to_be_true();
			expect(same.low == -8 && same.high == 8).to_be_true();
		});
		it("scales both ends, rounding toward zero", _ {
			TargetRange wide = scaledRange(lpf, 150);
			expect(wide.low == -12 && wide.high == 12).to_be_true();
			TargetRange narrow = scaledRange(TargetRange{Target::Env1Release, -6, 6}, 33);
			expect(narrow.low == -1 && narrow.high == 1).to_be_true();
			TargetRange morphWide = scaledRange(morph, 200);
			expect(morphWide.low == 0 && morphWide.high == 30).to_be_true();
		});
		it("keeps at least 1 on each end that moves at any percent but 0", _ {
			TargetRange decay = scaledRange(TargetRange{Target::Env1Release, -6, 6}, 1);
			expect(decay.low == -1 && decay.high == 1).to_be_true();
			TargetRange rate = scaledRange(TargetRange{Target::LfoRate, -4, 4}, 24);
			expect(rate.low == -1 && rate.high == 1).to_be_true();
			TargetRange darken = scaledRange(TargetRange{Target::LpfFrequency, -10, 0}, 5);
			expect(darken.low == -1 && darken.high == 0).to_be_true();
		});
		it("moves nothing at 0", _ {
			TargetRange off = scaledRange(lpf, 0);
			expect(off.low == 0 && off.high == 0).to_be_true();
			expect(targetValue(off, menu(30), kControlFull)).to_equal(menu(30));
			expect(targetValue(scaledRange(cables, 0), 1 << 28, -kControlFull)).to_equal(1 << 28);
		});
		it("plays a scaled range's value: the LPF at 200% reaches 16 above home", _ {
			expect(targetValue(scaledRange(lpf, 200), menu(30), kControlFull)).to_equal(menu(46));
			expect(targetValue(scaledRange(lpf, 200), menu(30), -50)).to_equal(menu(22));
		});
		it("scales a cable's reach: ×0.5..×1.5 at 50%", _ {
			expect(targetValue(scaledRange(cables, 50), 1 << 28, kControlFull)).to_equal(3 << 27);
			expect(targetValue(scaledRange(cables, 50), 1 << 28, -kControlFull)).to_equal(1 << 27);
		});
		it("stops a widened cable at ×0, one step from 0, and never flips its sign", _ {
			const TargetRange wide = scaledRange(cables, 200); // ×0 at -50, ×3 at +100
			expect(targetValue(wide, 1 << 28, 50)).to_equal(1 << 29);
			expect(targetValue(wide, 1 << 28, -50)).to_equal(1);
			expect(targetValue(wide, 1 << 28, -kControlFull)).to_equal(1);
			expect(targetValue(wide, -(1 << 28), -75)).to_equal(-1);
			expect(targetValue(wide, 1 << 28, -25)).to_equal(1 << 27);
		});
		it("gives a player edit's home on a scaled range", _ {
			const TargetRange wide = scaledRange(cables, 200);
			expect(homeFor(wide, 1 << 29, 50)).to_equal(1 << 28);
			expect(homeFor(wide, 1 << 20, -kControlFull)).to_equal(1 << 20); // ×0: the new amount itself
			expect(homeFor(scaledRange(lpf, 200), menu(22), -50)).to_equal(menu(30));
		});
	});

	context("the most params one set of controls can move", _ {
		it("is FM's, from the table: modulators 2, Decay 3, feedbacks 4, Space 2, Wobble 5 plus its cables", _ {
			expect(maxParamsPerSet(0)).to_equal(16);
			expect(maxParamsPerSet(32)).to_equal(48);
		});
	});

	context("a player edit's home (spec §11.6: home = new value − current layer offset)", _ {
		const TargetRange lpf{Target::LpfFrequency, -8, 8};
		const TargetRange morph{Target::LpfMorph, 0, 15};
		const TargetRange cables{Target::LfoCables, -50, 50}; // the table's: ×0..×2

		it("is the new value at offset 0", _ { expect(homeFor(lpf, menu(33), 0)).to_equal(menu(33)); });
		it("takes off the share of the range the offset reaches", _ {
			// The LPF sits 4 below home at -50; the player turns it to 28, so home is 32.
			expect(homeFor(lpf, menu(28), -50)).to_equal(menu(32));
			expect(homeFor(morph, menu(20), 60)).to_equal(menu(11));
		});
		it("plays the new value back from the home it gives, wherever the range doesn't clip", _ {
			const TargetRange decay{Target::Env1Release, -6, 6};
			for (const int32_t offset : {-100, -73, -40, -1, 1, 25, 60, 100}) {
				for (const int32_t value : {menu(10), menu(25), menu(40) + 12345}) {
					expect(targetValue(decay, homeFor(decay, value, offset), offset)).to_equal(value);
					expect(targetValue(lpf, homeFor(lpf, value, offset), offset)).to_equal(value);
				}
			}
		});
		it("takes nothing off on the side a range doesn't reach", _ {
			expect(homeFor(morph, menu(20), -kControlFull)).to_equal(menu(20));
		});
		it("counts the whole offset where the param's own range clipped it, so the param stays where it was turned", _ {
			// Home 48 at +8 is clipped at 50; turned down to 49, home is 41 and 41 + 8 plays 49.
			expect(homeFor(lpf, menu(49), kControlFull)).to_equal(menu(41));
			expect(targetValue(lpf, homeFor(lpf, menu(49), kControlFull), kControlFull)).to_equal(menu(49));
		});
		it("clamps to the param's own range", _ {
			expect(homeFor(lpf, menu(2), kControlFull)).to_equal(INT32_MIN);
			expect(homeFor(lpf, menu(47), -kControlFull)).to_equal(INT32_MAX);
		});
		it("divides a cable's amount by its scale", _ {
			expect(homeFor(cables, 3 << 27, 50)).to_equal(1 << 28);
			expect(homeFor(cables, -(1 << 29), kControlFull)).to_equal(-(1 << 28));
		});
		it("is a cable's new amount itself at ×0, where no home plays anything else", _ {
			expect(homeFor(cables, 1 << 20, -kControlFull)).to_equal(1 << 20);
		});
	});

	context("the delay Throw's feedback (spec §11.4)", _ {
		// Space's delay share on a preset that uses its delay, and none on one whose feedback is 0 or on a kit.
		constexpr TargetRange inUse{Target::DelayFeedback, -4, 4, Needs::DelayInUse};
		constexpr TargetRange unused{Target::DelayFeedback, 0, 0};
		const int32_t homes[] = {menu(0), menu(10), menu(23), menu(30)};
		const int32_t offsets[] = {-kControlFull, -40, 0, 40, kControlFull};

		it("holds at max(home, min(home + depth, 23)) whatever Space's share would say: never above 23, never below home", _ {
			for (const int32_t home : homes) {
				for (int32_t depth = 0; depth <= 25; depth++) {
					const auto spike = static_cast<int32_t>(
					    std::max<int64_t>(home, std::min<int64_t>(home + int64_t{depth} * 85899345, menu(23))));
					for (const int32_t offset : offsets) {
						for (const TargetRange& range : {inUse, unused}) {
							const int32_t value = throwDelayValue(range, home, offset, depth, 100);
							expect(value).to_equal(spike);
							expect(value >= home).to_be_true();
							expect(value <= std::max(home, menu(23))).to_be_true();
						}
					}
				}
			}
		});
		it("adds nothing to the delay from a home at 23 or more", _ {
			expect(throwDelayValue(unused, menu(23), 0, 12, 100)).to_equal(menu(23));
			expect(throwDelayValue(inUse, menu(30), 0, 25, 100)).to_equal(menu(30));
		});
		it("lifts a home of 0 by the depth: 12 to start", _ {
			expect(throwDelayValue(unused, menu(0), 0, 12, 100)).to_equal(menu(12));
			expect(throwDelayValue(unused, menu(10), 0, 12, 100)).to_equal(menu(22));
			expect(throwDelayValue(unused, menu(15), 0, 12, 100)).to_equal(menu(23)); // the cap
		});
		it("falls back to what the other layers say as its level goes to 0, between the two all the way", _ {
			for (const int32_t home : homes) {
				for (const int32_t offset : offsets) {
					const int32_t other = targetValue(inUse, home, offset);
					expect(throwDelayValue(inUse, home, offset, 12, 0)).to_equal(other);
					const int32_t spike = throwDelayValue(inUse, home, offset, 12, 100);
					int32_t previous = spike;
					for (const int32_t percent : {75, 50, 25, 0}) {
						const int32_t value = throwDelayValue(inUse, home, offset, 12, percent);
						expect(value >= std::min(other, spike) && value <= std::max(other, spike)).to_be_true();
						expect(spike >= other ? value <= previous : value >= previous).to_be_true();
						previous = value;
					}
				}
			}
		});
		it("never goes over the cap, or over a home above it, while it falls with the Throw still in Space's offset", _ {
			// Space's offset during the fall holds the Throw's own spike (kThrowSpace × its level), so the other layers'
			// share alone would lift a used delay past 23, or past a home of 30.
			for (const int32_t home : {menu(21), menu(22), menu(30)}) {
				for (const int32_t percent : {75, 50, 25}) {
					const int32_t offset = 100 * percent / 100;
					const int32_t value = throwDelayValue(inUse, home, offset, 12, percent);
					expect(value <= std::max(home, menu(23))).to_be_true();
					expect(value >= home).to_be_true();
				}
			}
		});
		it("reads a param in menu units as the stock menu does, to the nearest", _ {
			expect(menuUnits(INT32_MAX)).to_equal(50);
			expect(menuUnits(INT32_MIN)).to_equal(0);
			for (int32_t m = 0; m <= 50; m++) {
				expect(menuUnits(menu(m))).to_equal(m);
			}
		});
		it("is the plain Space share where no Throw plays", _ {
			expect(throwDelayValue(inUse, menu(10), 40, 12, 0)).to_equal(targetValue(inUse, menu(10), 40));
			expect(throwDelayValue(unused, menu(0), kControlFull, 12, 0)).to_equal(menu(0));
		});

		context("a player edit while the Throw plays (#344)", _ {
			it("takes Space's share from its offset without the spike, so the fall lands where the player set it", _ {
				// Home 10 holding at 22, Space's offset clamped at the spike, its other layers at 0: an edit to 21 is
				// home 21, not 21 − the spiked share (17), and the fall ends at 21.
				const int32_t home = throwDelayHomeFor(inUse, menu(21), kControlFull, 0, 100);
				expect(home).to_equal(menu(21));
				expect(throwDelayValue(inUse, home, 0, 12, 0)).to_equal(menu(21));
			});
			it("lands where the player set it from any level of the Throw and any rest offset", _ {
				for (const int32_t percent : {100, 75, 50, 25}) {
					for (const int32_t rest : {-kControlFull, -40, 0, 40}) {
						const int32_t spiked = std::min(rest + percent, kControlFull);
						for (const int32_t value : {menu(4), menu(12), menu(21), menu(30)}) {
							const int32_t home = throwDelayHomeFor(inUse, value, spiked, rest, percent);
							expect(home).to_equal(homeFor(inUse, value, rest));
							expect(throwDelayValue(inUse, home, rest, 12, 0)).to_equal(value);
						}
					}
				}
			});
			it("takes it from the offset playing where no Throw plays", _ {
				expect(throwDelayHomeFor(inUse, menu(21), 40, 0, 0)).to_equal(homeFor(inUse, menu(21), 40));
				expect(throwDelayHomeFor(unused, menu(21), kControlFull, 0, 0)).to_equal(menu(21));
			});
		});
	});
});

CPPSPEC_SPEC(sound_mapping)
