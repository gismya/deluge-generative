// Delay setups and the delay Throw in the event stream (spec §10.6, §11.4): a Throw on a part, kit row or kit with a
// delay setup carries its level on Space's Control events, full while it holds and falling over its beat, for the
// driver to lift delay feedback by. A set with no setup carries none. Played on Acid techno edited with delay
// setups, so it holds whatever Dub's (dub_spec) are tuned to. The feedback the driver plays from that level is
// sound_mapping_spec's; the drop-out's hold is drop_outs_spec's, the outgoing fall transitions_spec's.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <memory>
#include <vector>

using namespace harness;

namespace {

// Acid techno's roster.
constexpr uint8_t kDrums = 0;
constexpr uint8_t kBass = 1;
constexpr uint8_t kDrone = 2;

constexpr int32_t kBeatTicks = kStepsPerBeat * kTicksPerStep;
/// Long enough for a Groove, a Break, a Build and its Drop at seed 1.
constexpr int32_t kBars = 8 * 8 + 48;

constexpr DelaySetup kDottedEighth{.on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true};

Result playGenre(const Genre& genre) {
	Run run;
	run.recipe = acidRecipe();
	run.recipe.genre = &genre;
	run.skipIntro = true;
	return play(run, kBars);
}

/// Space's Control events for (part, lane) in [from, to).
std::vector<Event> spaces(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : controls(result)) {
		if (e.part == part && e.lane == lane && e.control == SoundControl::Space && e.tick >= from && e.tick < to) {
			out.push_back(e);
		}
	}
	return out;
}

/// The delay Throw's level as last sent for (part, lane) at or before `tick`: 0 if none was.
int32_t delayAt(const Result& result, uint8_t part, Lane lane, int32_t tick) {
	const Event* e = lastAt(result, part, lane, SoundControl::Space, tick);
	return e != nullptr ? e->throwDelay() : 0;
}

} // namespace

// clang-format off
describe delay_throws("Delay setups and the delay Throw", $ {
	// The Bass with a setup, no drift. Its Wash stays on, so the Break's macro layer, Wash and the Throw hold Space at
	// its clamp while the Throw falls: the delay level must move on with no change in Space.
	const auto bassDelay = technoEdited([](PartSpec& part) {
		part.drift.weight = 0;
		if (part.role == Role::Bass) {
			part.delay = kDottedEighth;
		}
	});
	const auto withDelay = std::make_shared<Result>(playGenre(bassDelay->genre));
	const int32_t breakTick = sectionStart(*withDelay, SectionKind::Break);

	it("plays a Break the Bass throws into", _ {
		expect(breakTick).to_be_greater_than(0);
	});

	it("lifts the delay in full on the spike, snapped with Space, and holds it while the part is silent", _ {
		const std::vector<Event> window = spaces(*withDelay, kBass, Lane::None, breakTick - kBeatTicks, breakTick);
		int32_t spikes = 0;
		for (const Event& e : window) {
			if (e.snap) {
				spikes++;
				expect(e.throwDelay()).to_equal(100);
			}
		}
		expect(spikes).to_be_greater_than(0);
		expect(delayAt(*withDelay, kBass, Lane::None, breakTick + kBarTicks / 2)).to_equal(100);
	});

	it("falls over a beat after the hold, step by step, while Space stays at its clamp", _ {
		std::vector<int32_t> falling;
		std::vector<int32_t> offsets;
		for (const Event& e : spaces(*withDelay, kBass, Lane::None, breakTick, breakTick + 2 * kBarTicks)) {
			expect(e.snap).to_be_false();
			falling.push_back(e.throwDelay());
			offsets.push_back(e.offset);
		}
		expect(falling == std::vector<int32_t>{75, 50, 25, 0}).to_be_true();
		// Macro +40, Wash about +50 and the falling Throw sum past the clamp until the Throw is gone.
		expect(offsets.size()).to_equal(size_t{4});
		expect(offsets[0] == kControlFull && offsets[1] == kControlFull && offsets[2] == kControlFull).to_be_true();
		expect(offsets[3]).to_be_less_than(kControlFull);
	});

	it("carries nothing on a part with no delay setup, which throws reverb only", _ {
		bool droneSpace = false;
		for (const Event& e : controls(*withDelay)) {
			if (e.part != kBass) {
				expect(e.throwDelay()).to_equal(0);
			}
			droneSpace = droneSpace || (e.part == kDrone && e.control == SoundControl::Space);
		}
		expect(droneSpace).to_be_true();
		// And the rest of the stream is the same as with no setup at all.
		const auto noDelay = technoEdited([](PartSpec& part) { part.drift.weight = 0; });
		const Result plain = playGenre(noDelay->genre);
		std::vector<Event> with = withDelay->events;
		for (Event& e : with) {
			if (e.kind == EventKind::Control) {
				e.velocity = 0;
			}
		}
		// Without the level, Space sends no event while it stays at its clamp: compare the other events.
		std::erase_if(with, [](const Event& e) { return e.kind == EventKind::Control && e.part == kBass; });
		std::vector<Event> without = plain.events;
		std::erase_if(without, [](const Event& e) { return e.kind == EventKind::Control && e.part == kBass; });
		expect(with == without).to_be_true();
		for (const Event& e : plain.events) {
			expect(e.kind != EventKind::Control || e.throwDelay() == 0).to_be_true();
		}
	});

	it("moves only Space: no other control carries a level", _ {
		for (const Event& e : controls(*withDelay)) {
			if (e.control != SoundControl::Space) {
				expect(e.throwDelay()).to_equal(0);
			}
		}
	});

	context("on Drums", _ {
		// The Build's snare roll plays into the Gap, so the kit and the Snare's row each spike there.
		const int32_t buildTick = sectionStart(*withDelay, SectionKind::Build, breakTick);
		const int32_t dropTick = sectionStart(*withDelay, SectionKind::Groove, buildTick);
		const int32_t gapTick = dropTick - kBeatTicks; // Techno's Gap is a beat

		it("moves the kit's delay with a kit-wide Throw, and no row's", _ {
			const auto kitDelay = technoEdited([](PartSpec& part) {
				part.drift.weight = 0;
				if (part.role == Role::Drums) {
					part.delay = kDottedEighth; // the kit's, at kit level
				}
			});
			const Result result = playGenre(kitDelay->genre);
			bool lifted = false;
			for (const Event& e : spaces(result, kDrums, Lane::None, gapTick - kBeatTicks, gapTick)) {
				lifted = lifted || (e.snap && e.throwDelay() == 100);
			}
			expect(lifted).to_be_true();
			for (const Event& e : controls(result)) {
				if (e.part == kDrums && e.lane != Lane::None) {
					expect(e.throwDelay()).to_equal(0);
				}
			}
		});

		it("moves a row's delay with a Throw aimed at its lane, and not the kit's", _ {
			const auto rowDelay = technoEdited([](PartSpec& part) {
				part.drift.weight = 0;
				if (part.role == Role::Drums) {
					part.delay = kDottedEighth; // the kit has one too: an aimed Throw leaves it alone
					part.gestures.aim[static_cast<int32_t>(Gesture::Throw)] = Lane::Snare;
					auto lanes = std::make_shared<std::vector<LaneSpec>>(part.lanes.begin(), part.lanes.end());
					for (LaneSpec& lane : *lanes) {
						lane.delay.on = lane.lane == Lane::Snare || lane.lane == Lane::ClosedHat;
					}
					static std::vector<std::shared_ptr<std::vector<LaneSpec>>> kept;
					kept.push_back(lanes);
					part.lanes = *lanes;
				}
			});
			const Result result = playGenre(rowDelay->genre);
			bool lifted = false;
			for (const Event& e : spaces(result, kDrums, Lane::Snare, gapTick - kBeatTicks, gapTick)) {
				lifted = lifted || (e.snap && e.throwDelay() == 100);
			}
			expect(lifted).to_be_true();
			for (const Event& e : controls(result)) {
				if (e.part == kDrums && e.lane != Lane::Snare) {
					expect(e.throwDelay()).to_equal(0); // the kit, and the hat whose row isn't aimed at
				}
			}
		});
	});

	context("its data", _ {
		it("reads a set's setup from its lane's row, or the part's own for the part or the kit", _ {
			static constexpr LaneSpec kLanes[] = {{.lane = Lane::Kick, .delay = kDottedEighth}, {.lane = Lane::Snare}};
			static constexpr PartSpec kKit{.role = Role::Drums, .lanes = kLanes};
			expect(delaySetupOf(kKit, Lane::Kick) == kDottedEighth).to_be_true();
			expect(delaySetupOf(kKit, Lane::Snare).on).to_be_false();
			expect(delaySetupOf(kKit, Lane::None).on).to_be_false();
			expect(delaySetupOf(kKit, Lane::Tom).on).to_be_false(); // a lane the part doesn't list
			static constexpr PartSpec kBassPart{.role = Role::Bass, .delay = kDottedEighth};
			expect(delaySetupOf(kBassPart, Lane::None) == kDottedEighth).to_be_true();
		});

		it("starts a part's Throw depth at 12 menu units", _ {
			expect(PartSpec{}.throwDelayDepth).to_equal(uint8_t{12});
		});

		it("gives a delay setup only to Dub: Dub techno's Stab keeps its preset's own echo", _ {
			for (const Genre* genre : allGenres()) {
				// dub_spec checks Dub's setups.
				expect(genreHasDelaySetup(*genre)).to_equal(genre == &genres::dub);
			}
		});
	});
});

CPPSPEC_SPEC(delay_throws)
