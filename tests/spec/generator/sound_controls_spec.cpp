// The six sound controls in the event stream (spec §11.1, §11.3, §11.7): the macro layer reads effective Energy, so a
// Section shades the sound (alone here: gestures_spec and drift_spec cover the other layers); a control's target is
// worked out once per step and sent only when it changes, ramped, with the piece's first step snapped.

#include "harness.h"

#include "cppspec.hpp"

#include <memory>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

std::vector<Event> controls(const Result& result) {
	return only(result.events, EventKind::Control);
}

/// The last offset sent for (part, lane, control) at or before `tick`, or 0 (home) if none was.
int32_t offsetAt(const Result& result, uint8_t part, Lane lane, SoundControl control, int32_t tick) {
	int32_t offset = 0;
	for (const Event& e : controls(result)) {
		if (e.tick <= tick && e.part == part && e.lane == lane && e.control == control) {
			offset = e.offset;
		}
	}
	return offset;
}

/// The first tick a Section of `kind` starts, or -1.
int32_t sectionStart(const Result& result, SectionKind kind) {
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.section == kind) {
			return e.tick;
		}
	}
	return -1;
}

/// Techno with no gesture and no drift: these specs look at the macro layer alone.
const std::shared_ptr<EditedGenre> kMacroOnly = technoEdited(macroLayerOnly);
Recipe macroRecipe() {
	Recipe recipe = technoRecipe();
	recipe.genre = &kMacroOnly->genre;
	return recipe;
}

// Techno's roster.
constexpr uint8_t kDrums = 0;
constexpr uint8_t kBass = 1;
constexpr uint8_t kFx = 2;

} // namespace

// clang-format off
describe sound_controls("The sound controls", $ {
	context("at the Recipe's default Energy in a Groove", _ {
		it("leaves every control at home: no Control event", _ {
			Run run;
			run.recipe = macroRecipe();
			run.skipIntro = true;
			Result result = play(run, 16);
			expect(controls(result).empty()).to_be_true();
		});
	});

	context("in a Break, 10 Energy points below the centre", _ {
		Run run;
		run.recipe = macroRecipe();
		run.skipIntro = true;
		const auto shared = std::make_shared<Result>(play(run, 8 * 8 + 32));
		const int32_t breakTick = sectionStart(*shared, SectionKind::Break);

		it("is reached", _ { expect(breakTick).to_be_greater_than(0); });

		it("darkens, lengthens, dries the drive and wets the space by the sign table (spec §11.3)", _ {
			const Result& result = *shared;
			// Energy −10 points against the default 25: 40% of each control's range (Generator-wide depth).
			const int32_t t = breakTick + kBarTicks;
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, t)).to_equal(-40);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Decay, t)).to_equal(40);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Drive, t)).to_equal(-40);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, t)).to_equal(40);
		});

		it("leaves Wobble and Thin alone: their Energy entries are 0", _ {
			const Result& result = *shared;
			for (const Event& e : controls(result)) {
				expect(e.control != SoundControl::Wobble && e.control != SoundControl::Thin).to_be_true();
			}
		});

		it("acts on the Drums' kit-wide controls and on each FX lane's own (spec §11.4)", _ {
			const Result& result = *shared;
			const int32_t t = breakTick + kBarTicks;
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Brightness, t)).to_equal(-40);
			expect(offsetAt(result, kFx, Lane::Riser, SoundControl::Brightness, t)).to_equal(-40);
			expect(offsetAt(result, kFx, Lane::Impact, SoundControl::Space, t)).to_equal(40);
			for (const Event& e : controls(result)) {
				expect(e.part == kFx ? e.lane != Lane::None : e.lane == Lane::None).to_be_true();
			}
		});

		it("ramps: no Control event is a snap after the first step", _ {
			const Result& result = *shared;
			for (const Event& e : controls(result)) {
				expect(e.snap).to_be_false();
			}
		});

		it("sends a control only when its target changes", _ {
			const Result& result = *shared;
			// A Break holds its offset, so each control is sent on the Break's first step and not again.
			int32_t sent = 0;
			for (const Event& e : controls(result)) {
				if (e.part == kBass && e.control == SoundControl::Brightness && e.tick > breakTick
				    && e.tick < breakTick + 8 * kBarTicks) {
					sent++;
				}
			}
			expect(sent).to_equal(0);
		});
	});

	context("in the Intro", _ {
		Run run;
		run.recipe = macroRecipe();
		const auto shared = std::make_shared<Result>(play(run, 16));

		it("snaps the first step's offsets, so the first notes sound shaded", _ {
			const Result& result = *shared;
			std::vector<Event> first;
			for (const Event& e : controls(result)) {
				if (e.tick == 0) {
					first.push_back(e);
				}
			}
			expect(first.empty()).to_be_false();
			for (const Event& e : first) {
				expect(e.snap).to_be_true();
			}
			// Energy starts 15 points below the centre (the Intro's offset).
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, 0)).to_equal(-60);
		});

		it("ramps a step at a time as Energy rises, each step's Control event on its grid line", _ {
			const Result& result = *shared;
			int32_t previous = -60;
			int32_t changes = 0;
			for (const Event& e : controls(result)) {
				if (e.part == kBass && e.control == SoundControl::Brightness && e.tick > 0) {
					expect(e.tick % kTicksPerStep).to_equal(0);
					expect(e.offset).to_be_greater_than(previous);
					previous = e.offset;
					changes++;
				}
			}
			expect(changes).to_be_greater_than(10);
		});

		it("plays each step's Control events before its notes", _ {
			const Result& result = *shared;
			// No Control event plays after a note-on of its own tick.
			int32_t lastNoteTick = -1;
			for (const Event& e : result.events) {
				if (e.kind == EventKind::NoteOn) {
					lastNoteTick = e.tick;
				}
				if (e.kind == EventKind::Control) {
					expect(e.tick != lastNoteTick).to_be_true();
				}
			}
		});

		it("drops nothing from the event buffer", _ { expect(shared->droppedEvents).to_equal(0U); });
	});

	context("at the top of the Energy range", _ {
		it("clamps each offset to the control's range, ±100", _ {
			Run run;
			run.recipe = macroRecipe();
			run.recipe.macros.set(Macro::Energy, kMacroFull);
			run.skipIntro = true;
			Result result = play(run, 4);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, 0)).to_equal(100);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, 0)).to_equal(-100);
		});
	});

	context("a genre's weights", _ {
		it("scale each sign table entry ×0..×2 and never flip it", _ {
			Genre genre = kMacroOnly->genre;
			genre.soundWeight[0][static_cast<int32_t>(SoundControl::Brightness)] = 2000;
			genre.soundWeight[0][static_cast<int32_t>(SoundControl::Space)] = 0;
			Run run;
			run.recipe = macroRecipe();
			run.recipe.genre = &genre;
			run.recipe.macros.set(Macro::Energy, macroPoints(30));
			run.skipIntro = true;
			Result result = play(run, 4);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, 0)).to_equal(40);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Decay, 0)).to_equal(-20);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, 0)).to_equal(0);
		});
	});

	context("an Energy turn in a Groove", _ {
		it("moves the controls from the next undecided step", _ {
			Run run;
			run.recipe = macroRecipe();
			run.skipIntro = true;
			run.steer = at(32, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(30)});
			Result result = play(run, 4);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, stepTick(31))).to_equal(0);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, stepTick(32))).to_equal(20);
		});
	});
});

CPPSPEC_SPEC(sound_controls)
