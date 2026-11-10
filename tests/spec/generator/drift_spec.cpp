// Drift (spec §11.5): a seeded, smoothed walk per part on Brightness, Decay, Space and Wobble, per lane on Drums and
// FX, deep and fast with Motion, outside takes and freeze; and the Sweep, a triangle locked to phrase lines that a
// genre gives a part's Brightness instead. Gestures are off here, and HOLD keeps the Groove playing.

#include "harness.h"

#include "cppspec.hpp"

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

using namespace harness;

namespace {

// Techno's roster.
constexpr uint8_t kDrums = 0;
constexpr uint8_t kBass = 1;
constexpr uint8_t kFx = 2;

constexpr int32_t kPhrases = 16;
constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;

std::vector<Event> controls(const Result& result) {
	return only(result.events, EventKind::Control);
}

const std::shared_ptr<EditedGenre> kNoGestures = technoEdited([](PartSpec& part) { part.gestures.off = kAllMoments; });

Result playDrift(int32_t motionPoints, uint32_t seed = 1, std::function<void(int32_t, SteeringQueue&)> steer = nullptr,
                 Takes takes = {}) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.genre = &kNoGestures->genre;
	run.recipe.macros.set(Macro::Motion, macroPoints(motionPoints));
	run.seed = seed;
	run.takes = takes;
	run.skipIntro = true;
	auto hold = at(0, {CommandKind::SetHold, 0, kPress});
	run.steer = [hold, steer](int32_t step, SteeringQueue& queue) {
		hold(step, queue);
		if (steer) {
			steer(step, queue);
		}
	};
	return play(run, 8 * kPhrases);
}

using Key = std::tuple<uint8_t, Lane, SoundControl>;

/// Each (part, lane, control)'s offsets as sent, in order.
std::map<Key, std::vector<int32_t>> series(const Result& result) {
	std::map<Key, std::vector<int32_t>> out;
	for (const Event& e : controls(result)) {
		out[{e.part, e.lane, e.control}].push_back(e.offset);
	}
	return out;
}

/// How often the walks turn, per walk and phrase: each change of direction between sent offsets.
double turnsPerPhrase(const Result& result) {
	int32_t turns = 0;
	int32_t walks = 0;
	for (const auto& [key, offsets] : series(result)) {
		if (std::get<0>(key) == kBass && std::get<2>(key) == SoundControl::Brightness) {
			continue; // Techno's Bass sweeps its Brightness
		}
		walks++;
		int32_t way = 0;
		for (size_t i = 1; i < offsets.size(); i++) {
			const int32_t now = offsets[i] > offsets[i - 1] ? 1 : -1;
			turns += way != 0 && now != way ? 1 : 0;
			way = now;
		}
	}
	return walks == 0 ? 0 : static_cast<double>(turns) / (walks * kPhrases);
}

int32_t deepest(const Result& result) {
	int32_t deepest = 0;
	for (const Event& e : controls(result)) {
		deepest = std::max<int32_t>(deepest, std::abs(e.offset));
	}
	return deepest;
}

/// A part's Brightness offset on each step, from the events sent.
std::vector<int32_t> brightnessBySteps(const Result& result, uint8_t part, int32_t steps) {
	std::vector<int32_t> out(steps, 0);
	int32_t offset = 0;
	size_t next = 0;
	const std::vector<Event> sent = controls(result);
	for (int32_t step = 0; step < steps; step++) {
		for (; next < sent.size() && sent[next].tick <= stepTick(step); next++) {
			const Event& e = sent[next];
			if (e.part == part && e.lane == Lane::None && e.control == SoundControl::Brightness) {
				offset = e.offset;
			}
		}
		out[step] = offset;
	}
	return out;
}

} // namespace

// clang-format off
describe drift("Drift", $ {
	const auto motion25 = std::make_shared<Result>(playDrift(25));

	it("moves Brightness, Decay, Space and Wobble, never Drive or Thin", _ {
		std::set<SoundControl> moved;
		for (const Event& e : controls(*motion25)) {
			moved.insert(e.control);
		}
		expect(moved == std::set<SoundControl>{SoundControl::Brightness, SoundControl::Decay, SoundControl::Space,
		                                        SoundControl::Wobble})
		    .to_be_true();
	});

	it("acts per part, and per lane on Drums and FX, never on the kit-wide controls", _ {
		std::set<std::pair<uint8_t, Lane>> sets;
		for (const Event& e : controls(*motion25)) {
			sets.insert({e.part, e.lane});
		}
		expect(sets.count({kBass, Lane::None})).to_equal(1U);
		expect(sets.count({kDrums, Lane::None})).to_equal(0U);
		expect(sets.count({kDrums, Lane::Kick})).to_equal(1U);
		expect(sets.count({kDrums, Lane::ClosedHat})).to_equal(1U);
		expect(sets.count({kFx, Lane::Riser})).to_equal(1U);
	});

	it("goes about half each control's range deep at Motion 50, a quarter at 25, and stops at 0", _ {
		const int32_t at50 = deepest(playDrift(50));
		expect(at50).to_be_greater_than(35);
		expect(at50).to_be_less_than(51);
		expect(deepest(*motion25)).to_be_greater_than(15);
		expect(deepest(*motion25)).to_be_less_than(26);
		expect(controls(playDrift(0)).empty()).to_be_true();
	});

	it("turns about once a phrase at Motion 25, and twice as often at 50", _ {
		const double at25 = turnsPerPhrase(*motion25);
		const double at50 = turnsPerPhrase(playDrift(50));
		expect(at25).to_be_greater_than(0.25);
		expect(at25).to_be_less_than(0.9);
		expect(at50 / at25).to_be_greater_than(1.5);
		expect(at50 / at25).to_be_less_than(2.5);
	});

	it("is seeded: the same seed drifts the same, another seed differently", _ {
		expect(controls(playDrift(25)) == controls(*motion25)).to_be_true();
		expect(controls(playDrift(25, 2)) == controls(*motion25)).to_be_false();
	});

	it("isn't covered by takes", _ {
		Takes takes;
		takes.take[kDrums] = 3;
		takes.take[kBass] = 5;
		expect(controls(playDrift(25, 1, nullptr, takes)) == controls(*motion25)).to_be_true();
	});

	it("keeps wandering under freeze", _ {
		const Result frozen = playDrift(25, 1, at(20, {CommandKind::SetFreeze, kBass, kPressNow}));
		expect(controls(frozen) == controls(*motion25)).to_be_true();
	});

	context("Techno's acid Bass Sweep", _ {
		const std::vector<int32_t> sweep = brightnessBySteps(*motion25, kBass, 4 * kPhraseSteps);

		it("is a triangle over 8 bars, locked to phrase lines", _ {
			for (int32_t step = 0; step + kPhraseSteps < static_cast<int32_t>(sweep.size()); step++) {
				expect(sweep[step + kPhraseSteps]).to_equal(sweep[step]);
			}
			for (size_t step = 1; step < sweep.size(); step++) {
				expect(std::abs(sweep[step] - sweep[step - 1])).to_be_less_than(2);
			}
			expect(*std::min_element(sweep.begin(), sweep.end())).to_equal(-25);
			expect(*std::max_element(sweep.begin(), sweep.end())).to_equal(25);
		});

		it("starts on one of four quarter positions, drawn from the seed and the part", _ {
			std::set<int32_t> starts;
			for (uint32_t seed = 1; seed <= 12; seed++) {
				const int32_t start = brightnessBySteps(playDrift(25, seed), kBass, 1)[0];
				expect(start == -25 || start == 0 || start == 25).to_be_true();
				starts.insert(start);
			}
			expect(starts.size()).to_be_greater_than(1U);
		});
	});
});

CPPSPEC_SPEC(drift)
