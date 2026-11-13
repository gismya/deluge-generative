// Gestures (spec §11.4): Close and Wash shade a Break and an Outro, Rise lifts a Build, Throw spikes Space on the
// last hits before a Break or a Gap, and Reset snaps them all back at a Groove's downbeat. Each switch is genre data
// per part and moment, with a depth per gesture and part. Drift is off here (drift_spec covers it).

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <memory>
#include <optional>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBeatTicks = kStepsPerBeat * kTicksPerStep;

// Techno's roster.
constexpr uint8_t kDrums = 0;
constexpr uint8_t kBass = 1;
constexpr uint8_t kFx = 2;

/// Long enough for a Groove, a Break, a Build and its Drop at seed 1.
constexpr int32_t kBars = 8 * 8 + 48;

/// Snapped Space events for (part, lane) in [from, to).
std::vector<Event> spaceSnaps(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : controls(result)) {
		if (e.part == part && e.lane == lane && e.control == SoundControl::Space && e.snap && e.tick >= from
		    && e.tick < to) {
			out.push_back(e);
		}
	}
	return out;
}

std::shared_ptr<EditedGenre> withoutDrift(const std::function<void(PartSpec&)>& edit = [](PartSpec&) {}) {
	return technoEdited([&](PartSpec& part) {
		part.drift.weight = 0;
		edit(part);
	});
}

Result playGenre(const Genre& genre, int32_t bars = kBars, bool skipIntro = true,
                 std::function<void(int32_t, SteeringQueue&)> steer = nullptr) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.genre = &genre;
	run.skipIntro = skipIntro;
	run.steer = std::move(steer);
	return play(run, bars);
}

} // namespace

// clang-format off
describe gestures("Gestures", $ {
	const auto plain = withoutDrift();
	const auto shared = std::make_shared<Result>(playGenre(plain->genre));
	const int32_t breakTick = sectionStart(*shared, SectionKind::Break);
	const int32_t buildTick = sectionStart(*shared, SectionKind::Build, breakTick);
	const int32_t dropTick = sectionStart(*shared, SectionKind::Groove, buildTick);
	/// In the Break, after any Throw from its last bar has held and decayed.
	const int32_t afterThrow = breakTick + 2 * kBarTicks;

	it("plays a Groove, a Break, a Build and its Drop", _ {
		expect(breakTick).to_be_greater_than(0);
		expect(buildTick).to_be_greater_than(breakTick);
		expect(dropTick).to_be_greater_than(buildTick);
	});

	context("Close and Wash entering a Break", _ {
		it("ease Brightness down and Space up over the Break's first bar, then hold", _ {
			const Result& result = *shared;
			// The macro layer alone gives the Break -40 and +40 (sound_controls_spec); Close and Wash add 50 each.
			const int32_t first = offsetAt(result, kBass, Lane::None, SoundControl::Brightness, breakTick);
			expect(first).to_be_less_than(-40);
			expect(first).to_be_greater_than(-50);
			int32_t previous = first;
			for (int32_t step = 1; step < kStepsPerBar; step++) {
				const int32_t now =
				    offsetAt(result, kBass, Lane::None, SoundControl::Brightness, breakTick + step * kTicksPerStep);
				expect(now).to_be_less_than(previous + 1);
				previous = now;
			}
			const int32_t barOn = breakTick + kBarTicks - kTicksPerStep;
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, barOn)).to_equal(-90);
			// The Bass's Throw holds Space at the top for a bar from its last hit before the Break, then decays.
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, afterThrow)).to_equal(90);
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Space, barOn)).to_equal(90);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, buildTick - kTicksPerStep))
			    .to_equal(-90);
		});

		it("act on the Drums' kit-wide controls and on each FX lane's own", _ {
			const Result& result = *shared;
			const int32_t t = breakTick + kBarTicks;
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Brightness, t)).to_equal(-90);
			expect(offsetAt(result, kDrums, Lane::Kick, SoundControl::Brightness, t)).to_equal(0);
			expect(offsetAt(result, kFx, Lane::Impact, SoundControl::Space, t)).to_equal(90);
		});

		it("hold through the Build until the Drop's Reset snaps every control back", _ {
			const Result& result = *shared;
			int32_t atDrop = 0;
			for (const Event& e : controls(result)) {
				if (e.tick == dropTick) {
					atDrop++;
					expect(e.snap).to_be_true();
				}
			}
			expect(atDrop).to_be_greater_than(0);
			// The Drop lifts Energy, so the macro layer isn't at home: compare with no gestures at all.
			const auto none = withoutDrift([](PartSpec& part) { part.gestures.off = kAllMoments; });
			const Result macroOnly = playGenre(none->genre);
			for (SoundControl control : {SoundControl::Brightness, SoundControl::Space, SoundControl::Thin}) {
				for (uint8_t part : {kBass, kDrums}) {
					expect(offsetAt(result, part, Lane::None, control, dropTick))
					    .to_equal(offsetAt(macroOnly, part, Lane::None, control, dropTick));
				}
			}
		});
	});

	context("Rise across a Build", _ {
		it("raises Thin with an ease-in, peaking on the Build's last step", _ {
			const Result& result = *shared;
			const int32_t last = dropTick - kTicksPerStep;
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Thin, last)).to_equal(100);
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Thin, last)).to_equal(100);
			const int32_t middle = offsetAt(result, kBass, Lane::None, SoundControl::Thin, (buildTick + dropTick) / 2);
			expect(middle).to_be_greater_than(0);
			expect(middle).to_be_less_than(35); // an ease-in: a quarter of the way at the halfway point
			int32_t previous = 0;
			for (const Event& e : controls(result)) {
				if (e.part == kBass && e.control == SoundControl::Thin && e.tick >= buildTick && e.tick < dropTick) {
					expect(e.offset).to_be_greater_than(previous - 1);
					previous = e.offset;
				}
			}
		});

		it("raises Brightness by 40 at its peak, and a Riser's Space by 50", _ {
			const auto noRise = withoutDrift([](PartSpec& part) { part.gestures.off |= momentBit(GestureMoment::RiseBuild); });
			const Result without = playGenre(noRise->genre);
			const Result& with = *shared;
			const int32_t last = dropTick - kTicksPerStep;
			expect(offsetAt(with, kBass, Lane::None, SoundControl::Brightness, last)
			       - offsetAt(without, kBass, Lane::None, SoundControl::Brightness, last))
			    .to_equal(40);
			expect(offsetAt(with, kFx, Lane::Riser, SoundControl::Space, last)
			       - offsetAt(without, kFx, Lane::Riser, SoundControl::Space, last))
			    .to_equal(50);
			expect(offsetAt(with, kFx, Lane::Impact, SoundControl::Space, last)
			       - offsetAt(without, kFx, Lane::Impact, SoundControl::Space, last))
			    .to_equal(0);
		});
	});

	context("in Techno", _ {
		Run run;
		run.recipe = technoRecipe();
		const auto techno = std::make_shared<Result>(play(run, 8 * 2 + kBars));
		const int32_t grooveTick = sectionStart(*techno, SectionKind::Groove);
		const int32_t technoBuild = sectionStart(*techno, SectionKind::Build);
		const int32_t technoDrop = sectionStart(*techno, SectionKind::Groove, technoBuild);

		it("keeps the Intro unfiltered while its Builds Rise (spec §11.4)", _ {
			const Result& result = *techno;
			expect(grooveTick).to_be_greater_than(0);
			expect(technoDrop).to_be_greater_than(technoBuild);
			for (const Event& e : controls(result)) {
				if (e.tick < grooveTick) {
					expect(e.control != SoundControl::Thin || e.offset == 0).to_be_true();
				}
			}
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Thin, technoDrop - kTicksPerStep)).to_equal(100);
		});
	});

	context("a genre's switches and depths", _ {
		it("switch a gesture off for one part and moment only", _ {
			const auto genre = withoutDrift([](PartSpec& part) {
				if (part.role == Role::Bass) {
					part.gestures.off = momentBit(GestureMoment::CloseBreak);
				}
			});
			const Result result = playGenre(genre->genre);
			const int32_t t = afterThrow;
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, t)).to_equal(-40);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, t)).to_equal(90);
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Brightness, t)).to_equal(-90);
		});

		it("scale each gesture ×0..×2 per part", _ {
			const auto genre = withoutDrift([](PartSpec& part) {
				if (part.role == Role::Bass) {
					part.gestures.depth[static_cast<int32_t>(Gesture::Wash)] = 500;
					part.gestures.depth[static_cast<int32_t>(Gesture::Close)] = 0;
				}
			});
			const Result result = playGenre(genre->genre);
			const int32_t t = afterThrow;
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, t)).to_equal(65);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Brightness, t)).to_equal(-40);
		});

		it("aim a Drums gesture at one lane instead of the kit-wide controls", _ {
			const auto genre = withoutDrift([](PartSpec& part) {
				if (part.role == Role::Drums) {
					part.gestures.aim[static_cast<int32_t>(Gesture::Rise)] = Lane::Snare;
				}
			});
			const Result result = playGenre(genre->genre);
			const int32_t last = dropTick - kTicksPerStep;
			expect(offsetAt(result, kDrums, Lane::Snare, SoundControl::Thin, last)).to_equal(100);
			expect(offsetAt(result, kDrums, Lane::None, SoundControl::Thin, last)).to_equal(0);
			expect(offsetAt(result, kDrums, Lane::Kick, SoundControl::Thin, last)).to_equal(0);
		});
	});

	context("Close and Wash across an Outro", _ {
		it("shade it over its first bar, by their depth", _ {
			const auto on = withoutDrift();
			const auto off = withoutDrift([](PartSpec& part) {
				part.gestures.off |= momentBit(GestureMoment::CloseOutro) | momentBit(GestureMoment::WashOutro);
			});
			const Command end{CommandKind::End, 0, 1};
			const Result with = playGenre(on->genre, 24, true, at(40, end));
			const Result without = playGenre(off->genre, 24, true, at(40, end));
			const int32_t outro = sectionStart(with, SectionKind::Outro);
			expect(outro).to_be_greater_than(0);
			const int32_t t = outro + kBarTicks - kTicksPerStep;
			expect(offsetAt(with, kBass, Lane::None, SoundControl::Brightness, t)
			       - offsetAt(without, kBass, Lane::None, SoundControl::Brightness, t))
			    .to_equal(-50);
			expect(offsetAt(with, kBass, Lane::None, SoundControl::Space, t)
			       - offsetAt(without, kBass, Lane::None, SoundControl::Space, t))
			    .to_equal(50);
		});
	});

	context("Reset after an Intro that Rises", _ {
		it("snaps Thin back to home on the first Groove's downbeat", _ {
			const auto genre = withoutDrift([](PartSpec& part) { part.gestures.off = 0; });
			const Result result = playGenre(genre->genre, 8 * 3, false);
			const int32_t groove = sectionStart(result, SectionKind::Groove);
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Thin, groove - kTicksPerStep)).to_equal(100);
			const Event* reset = lastAt(result, kBass, Lane::None, SoundControl::Thin, groove);
			expect(reset != nullptr && reset->tick == groove && reset->snap && reset->offset == 0).to_be_true();
		});
	});

	context("Throw", _ {
		// Wash off on the Bass so its Space shows the Throw alone, at half depth so the clamp stays out of the way.
		const auto halfThrow = withoutDrift([](PartSpec& part) {
			part.gestures.off |= momentBit(GestureMoment::WashBreak);
			part.gestures.depth[static_cast<int32_t>(Gesture::Throw)] = 500;
		});
		const auto thrown = std::make_shared<Result>(playGenre(halfThrow->genre));
		const std::vector<Event> spikes = spaceSnaps(*thrown, kBass, Lane::None, breakTick - kBeatTicks, breakTick);

		it("spikes Space on a part's hits in the last beat before a Break that silences it, snapped", _ {
			expect(spikes.empty()).to_be_false();
			for (const Event& e : spikes) {
				expect(e.offset).to_equal(50);
			}
		});

		it("holds while the part is silent, up to the throw hold, then decays over a beat", _ {
			const Result& result = *thrown;
			// The Break's macro layer gives Space +40, the Throw 50 more. Later hits in the window re-spike without an
			// event, so the hold runs at least a bar from the first spike and ends within a bar of the Break line.
			expect(offsetAt(result, kBass, Lane::None, SoundControl::Space, spikes.back().tick + kBarTicks)).to_equal(90);
			std::vector<int32_t> falling;
			for (const Event& e : controls(result)) {
				if (e.part == kBass && e.control == SoundControl::Space && e.tick > breakTick && e.tick < afterThrow) {
					expect(e.snap).to_be_false();
					falling.push_back(e.offset);
				}
			}
			expect(falling == std::vector<int32_t>{77, 65, 52, 40}).to_be_true();
		});

		it("leaves parts the Break keeps alone", _ {
			// Techno's Break keeps the kick, rim and closed hat, so the kit isn't silent; FX lanes always play.
			expect(spaceSnaps(*thrown, kDrums, Lane::None, breakTick - kBeatTicks, breakTick).empty()).to_be_true();
			for (const Event& e : controls(*thrown)) {
				if (e.part == kFx && e.control == SoundControl::Space && e.tick < breakTick) {
					expect(e.snap).to_be_false();
				}
			}
		});

		it("spikes every part that hits in the last beat before the Gap", _ {
			const int32_t gapTick = dropTick - kBeatTicks; // Techno's Gap is a beat
			const std::vector<Event> kit = spaceSnaps(*shared, kDrums, Lane::None, gapTick - kBeatTicks, gapTick);
			expect(kit.empty()).to_be_false();
			for (const Event& e : kit) {
				expect(e.offset).to_be_greater_than(49);
			}
			const auto noGap = withoutDrift([](PartSpec& part) { part.gestures.off |= momentBit(GestureMoment::ThrowGap); });
			const Result without = playGenre(noGap->genre);
			expect(spaceSnaps(without, kDrums, Lane::None, gapTick - kBeatTicks, gapTick).empty()).to_be_true();
		});

		it("brings a part that plays again before its hold ends back home, snapped", _ {
			// HOLD lands just after the first spike, so the Groove plays on and the Bass plays again.
			const int32_t step = spikes.front().tick / kTicksPerStep + 1;
			const Result held = playGenre(halfThrow->genre, kBars, true, at(step, {CommandKind::SetHold, 0, kPress}));
			std::optional<Event> back;
			for (const Event& e : controls(held)) {
				if (!back && e.part == kBass && e.control == SoundControl::Space && e.tick > spikes.front().tick
				    && e.offset == 0) {
					back = e;
				}
			}
			expect(back.has_value()).to_be_true();
			expect(back->snap).to_be_true();
			expect(back->tick).to_be_less_than(spikes.front().tick + kBarTicks);
		});
	});
});

CPPSPEC_SPEC(gestures)
