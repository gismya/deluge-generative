// The Arc through the Perform driver (spec §9.1, §9.2, §9.4): Section events, the automatic cycle, and Energy and
// Density offsets as they show in the stream.

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr int32_t kPhraseTicks = 8 * kBarTicks; // Techno's phrase is 8 bars

/// Plays long enough for two full cycles at any seed: Intro 2 + (Groove ≤ 8 + Break ≤ 2 + Build ≤ 2) × 2 phrases.
Result playLong(uint32_t seed) {
	Run run;
	run.recipe = technoRecipe();
	run.seed = seed;
	return play(run, 8 * (2 + 2 * 12));
}

/// Kick velocities and closed-hat counts per bar, to read effective Energy and Density off the stream.
struct BarProfile {
	int32_t kickVelocity = 0;
	int32_t hats = 0;
};
std::vector<BarProfile> profile(const std::vector<Event>& events, int32_t bars) {
	std::vector<BarProfile> out(bars);
	for (const Event& e : only(events, EventKind::NoteOn)) {
		int32_t bar = e.tick / kBarTicks;
		if (bar >= bars) {
			continue;
		}
		if (e.lane == Lane::Kick && e.tick % kBarTicks == 0) {
			out[bar].kickVelocity = e.velocity;
		}
		if (e.lane == Lane::ClosedHat) {
			out[bar].hats++;
		}
	}
	return out;
}

} // namespace

// clang-format off
describe arc("The Arc", $ {
	it("starts every piece with the Intro on step 0", _ {
		Result result = playLong(1);
		expect(result.events.front().kind).to_equal(EventKind::Section);
		expect(result.events.front().tick).to_equal(0);
		expect(result.events.front().section).to_equal(SectionKind::Intro);
		expect(result.events.front().occurrence).to_equal(uint16_t{0});
	});

	it("cycles Groove, Break, Build and back to Groove, each on a phrase line", _ {
		for (uint32_t seed : {1U, 2U, 3U, 77U}) {
			auto sections = only(playLong(seed).events, EventKind::Section);
			const SectionKind expected[] = {SectionKind::Intro, SectionKind::Groove, SectionKind::Break,
			                                SectionKind::Build, SectionKind::Groove, SectionKind::Break,
			                                SectionKind::Build, SectionKind::Groove};
			expect(sections.size()).to_be_greater_than(size_t{7});
			uint16_t grooves = 0;
			for (size_t i = 0; i < sections.size() && i < 8; i++) {
				expect(sections[i].section).to_equal(expected[i]);
				expect(sections[i].tick % kPhraseTicks).to_equal(0);
				if (sections[i].section == SectionKind::Groove) {
					expect(sections[i].occurrence).to_equal(grooves++);
				}
			}
		}
	});

	it("plays Techno's 2-phrase Intro, a seeded 4–8 phrase Groove, a 1–2 phrase Break and an 8 or 16 bar Build", _ {
		std::vector<int32_t> grooveLengths;
		for (uint32_t seed = 1; seed <= 12; seed++) {
			auto sections = only(playLong(seed).events, EventKind::Section);
			expect(sections[1].tick).to_equal(2 * kPhraseTicks);
			int32_t groove = (sections[2].tick - sections[1].tick) / kPhraseTicks;
			int32_t breakLength = (sections[3].tick - sections[2].tick) / kPhraseTicks;
			int32_t buildBars = (sections[4].tick - sections[3].tick) / kBarTicks;
			expect(groove).to_be_between(4, 8);
			expect(breakLength).to_be_between(1, 2);
			expect(buildBars == 8 || buildBars == 16).to_be_true();
			grooveLengths.push_back(groove);
		}
		bool varies = false;
		for (int32_t length : grooveLengths) {
			varies = varies || length != grooveLengths.front();
		}
		expect(varies).to_be_true();
	});

	it("dips Energy and Density in a Break, ramps them in a Build and lifts them on the Drop", _ {
		Result result = playLong(5);
		auto sections = only(result.events, EventKind::Section);
		int32_t grooveBar = sections[1].tick / kBarTicks + 8; // well into the first Groove, at the centre
		int32_t breakBar = sections[2].tick / kBarTicks;
		int32_t buildFirst = sections[3].tick / kBarTicks;
		int32_t dropBar = sections[4].tick / kBarTicks;
		auto bars = profile(result.events, dropBar + 9);

		expect(bars[breakBar].kickVelocity).to_be_less_than(bars[grooveBar].kickVelocity);
		expect(bars[breakBar].hats).to_be_less_than(bars[grooveBar].hats);
		// The Build starts from the Break's dip, so it doesn't jump on its first step, and climbs above the centre.
		expect(bars[buildFirst].kickVelocity).to_be_less_than(bars[grooveBar].kickVelocity + 2);
		expect(bars[dropBar - 1].kickVelocity).to_be_greater_than(bars[grooveBar].kickVelocity);
		// The Drop lifts, then decays back to the centre over one phrase.
		expect(bars[dropBar].kickVelocity).to_be_greater_than(bars[grooveBar].kickVelocity);
		expect(bars[dropBar + 8].kickVelocity).to_equal(bars[grooveBar].kickVelocity);
	});

	it("rises through the Intro from low", _ {
		Result result = playLong(1);
		auto bars = profile(result.events, 17);
		expect(bars[0].kickVelocity).to_be_less_than(bars[15].kickVelocity);
		expect(bars[0].hats).to_be_less_than(bars[16].hats + 1);
		expect(bars[15].kickVelocity).to_be_less_than(bars[16].kickVelocity + 1);
	});
});

CPPSPEC_SPEC(arc)
