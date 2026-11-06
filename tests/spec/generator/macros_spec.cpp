// The four macros (spec §3.3), heard in the stream: Energy moves velocity, Density how many notes play, Complexity
// the pattern tier, Motion how much the Loop line changes. A macro moves its features the same way everywhere.

#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr int32_t kGrooveStart = 2 * 8 * kBarTicks; // after Techno's 2-phrase Intro
constexpr int32_t kBars = 2 * 8 + 32;               // into the Groove, which lasts at least 4 phrases

Result playWith(Macro macro, int32_t points) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.macros.set(macro, macroPoints(points));
	return play(run, kBars);
}

/// Note-ons of the first Groove.
std::vector<Event> grooveOns(const Result& result) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.tick >= kGrooveStart) {
			out.push_back(e);
		}
	}
	return out;
}

int32_t meanVelocity(const std::vector<Event>& ons) {
	int32_t sum = 0;
	for (const Event& e : ons) {
		sum += e.velocity;
	}
	return ons.empty() ? 0 : sum / static_cast<int32_t>(ons.size());
}

/// Distinct steps within the bar where the closed hat plays.
size_t hatPositions(const std::vector<Event>& ons) {
	std::set<int32_t> steps;
	for (const Event& e : ons) {
		if (e.lane == Lane::ClosedHat) {
			steps.insert((e.tick % kBarTicks) / kTicksPerStep);
		}
	}
	return steps.size();
}

/// The bass line's pitches per bar, so passes can be compared.
std::map<int32_t, std::vector<uint8_t>> bassBars(const std::vector<Event>& ons) {
	std::map<int32_t, std::vector<uint8_t>> bars;
	for (const Event& e : ons) {
		if (e.lane == Lane::None) {
			bars[e.tick / kBarTicks].push_back(e.pitch);
		}
	}
	return bars;
}

int32_t changedBars(const std::map<int32_t, std::vector<uint8_t>>& bars) {
	int32_t changes = 0;
	const std::vector<uint8_t>* previous = nullptr;
	for (const auto& [bar, pitches] : bars) {
		if (previous != nullptr && *previous != pitches) {
			changes++;
		}
		previous = &pitches;
	}
	return changes;
}

} // namespace

// clang-format off
describe macros("The macros", $ {
	it("Energy plays harder as it rises, with the same notes", _ {
		auto low = grooveOns(playWith(Macro::Energy, 5));
		auto high = grooveOns(playWith(Macro::Energy, 45));
		expect(meanVelocity(high)).to_be_greater_than(meanVelocity(low) + 10);
		expect(high.size()).to_equal(low.size());
	});

	it("Density lets more notes play as it rises, only ever adding hits", _ {
		auto sparse = grooveOns(playWith(Macro::Density, 10));
		auto busy = grooveOns(playWith(Macro::Density, 40));
		expect(busy.size()).to_be_greater_than(sparse.size() + sparse.size() / 2);
		std::set<std::pair<int32_t, Lane>> busyHits;
		for (const Event& e : busy) {
			busyHits.insert({e.tick, e.lane});
		}
		for (const Event& e : sparse) {
			expect(busyHits.count({e.tick, e.lane})).to_equal(size_t{1});
		}
	});

	it("Complexity picks busier pattern tiers", _ {
		auto plain = grooveOns(playWith(Macro::Complexity, 0));
		auto intricate = grooveOns(playWith(Macro::Complexity, 50));
		expect(hatPositions(intricate)).to_be_greater_than(hatPositions(plain));
	});

	it("Motion 0 holds the Loop line, and more Motion changes it more often", _ {
		auto still = bassBars(grooveOns(playWith(Macro::Motion, 0)));
		auto moving = bassBars(grooveOns(playWith(Macro::Motion, 50)));
		expect(changedBars(still)).to_equal(0);
		expect(changedBars(moving)).to_be_greater_than(changedBars(bassBars(grooveOns(playWith(Macro::Motion, 10)))));
	});
});

CPPSPEC_SPEC(macros)
