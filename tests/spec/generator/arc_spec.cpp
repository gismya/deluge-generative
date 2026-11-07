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

/// The Section events of `bars` bars of `genre` from Start, with Techno's other facts.
std::vector<Event> sectionsOf(const Genre& genre, uint32_t seed, int32_t bars,
                              const std::function<void(int32_t, SteeringQueue&)>& steer = {}) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.genre = &genre;
	run.seed = seed;
	run.steer = steer;
	return only(play(run, bars).events, EventKind::Section);
}

/// How many phrases Techno's first Groove plays at `motion`, or -1 if it hasn't ended after 40 phrases.
int32_t firstGroovePhrases(uint32_t seed, int32_t motion) {
	Run run;
	run.recipe = technoRecipe();
	run.recipe.macros.set(Macro::Motion, motion);
	run.seed = seed;
	auto sections = only(play(run, 8 * (2 + 40)).events, EventKind::Section);
	if (sections.size() < 3) {
		return -1;
	}
	return (sections[2].tick - sections[1].tick) / kPhraseTicks;
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

	it("starts a Build from exactly where the Break's offsets ended", _ {
		// A Break that keeps falling, so a Build starting one step's worth too far would show.
		static constexpr Genre kFalling{
		    .name = "falling",
		    .arc = {.groovePhrases = {1, 1},
		            .offsets = {{}, {}, {0, macroPoints(-20), 0, macroPoints(-30)}, {0, macroPoints(10), 0, macroPoints(10)}, {}}},
		};
		// A centre with room for every offset, so they play unscaled (spec §9.4).
		Macros centre;
		centre.set(Macro::Energy, macroPoints(30));
		centre.set(Macro::Density, macroPoints(30));
		Arc arc;
		arc.start(kFalling);
		int32_t lastEnergy = 0;
		int32_t lastDensity = 0;
		for (int32_t step = 0; step < 100 * kStepsPerBar; step++) {
			bool starts = arc.enterStep(kFalling, 1, step, centre);
			if (starts && arc.section() == SectionKind::Build) {
				expect(lastEnergy).to_equal(macroPoints(-20));
				expect(lastDensity).to_equal(macroPoints(-30));
				expect(arc.offset(kFalling, Macro::Energy, centre[Macro::Energy])).to_equal(lastEnergy);
				expect(arc.offset(kFalling, Macro::Density, centre[Macro::Density])).to_equal(lastDensity);
				return;
			}
			lastEnergy = arc.offset(kFalling, Macro::Energy, centre[Macro::Energy]);
			lastDensity = arc.offset(kFalling, Macro::Density, centre[Macro::Density]);
		}
		expect(false).to_be_true(); // no Build came
	});

	it("uses the Energy offset for each end of a Density offset a genre leaves unset, and for the Drop lift", _ {
		static constexpr Genre kHalfSet{
		    .name = "halfset",
		    .arc = {.groovePhrases = {1, 1},
		            .dropLiftEnergy = macroPoints(6),
		            .offsets = {{}, {}, {macroPoints(-4), macroPoints(-8), macroPoints(-20), kSameAsEnergy}, {}, {}}},
		};
		Macros centre;
		centre.set(Macro::Density, macroPoints(30));
		Arc arc;
		arc.start(kHalfSet);
		int32_t first = 0;
		int32_t last = 0;
		int32_t dropLift = 0;
		bool inBreak = false;
		for (int32_t step = 0; step < 100 * kStepsPerBar; step++) {
			if (arc.enterStep(kHalfSet, 1, step, centre)) {
				inBreak = arc.section() == SectionKind::Break;
				if (inBreak) {
					first = arc.offset(kHalfSet, Macro::Density, centre[Macro::Density]);
				}
				if (arc.section() == SectionKind::Groove && arc.occurrence() == 1) {
					dropLift = arc.offset(kHalfSet, Macro::Density, centre[Macro::Density]);
					break;
				}
			}
			if (inBreak) {
				last = arc.offset(kHalfSet, Macro::Density, centre[Macro::Density]);
			}
		}
		expect(first).to_equal(macroPoints(-20));
		expect(last).to_equal(macroPoints(-8));
		expect(dropLift).to_equal(macroPoints(6));
	});

	it("starts a Build shorter than a phrase in the phrase before the Drop, so the Drop lands on a phrase line", _ {
		// A 1-phrase Break followed by a 4-bar Build in an 8-bar phrase plays 4 bars of Break (spec §9.3).
		for (uint8_t bars : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
			const WeightedBars build[] = {{bars, 1}};
			Genre genre = genres::techno;
			genre.arc.introPhrases = 1;
			genre.arc.groovePhrases = {1, 1};
			genre.arc.breakPhrases = {1, 1};
			genre.arc.buildBars = build;
			auto sections = sectionsOf(genre, 1, 40);
			expect(sections.size()).to_be_greater_than(size_t{4});
			expect(sections[2].section).to_equal(SectionKind::Break);
			expect(sections[2].tick).to_equal(2 * kPhraseTicks);
			expect(sections[3].section).to_equal(SectionKind::Build);
			expect(sections[3].tick).to_equal(3 * kPhraseTicks - bars * kBarTicks);
			expect(sections[4].section).to_equal(SectionKind::Groove);
			expect(sections[4].tick).to_equal(3 * kPhraseTicks);
		}
	});

	it("paces the Groove by Motion: b ÷ 3 phrases at Motion 50, about 3 × b just above 0, and never ending at 0", _ {
		// Techno's b is 4–8 phrases at Motion 25 (spec §9.3).
		for (uint32_t seed = 1; seed <= 12; seed++) {
			int32_t at25 = firstGroovePhrases(seed, macroPoints(25));
			int32_t at50 = firstGroovePhrases(seed, macroPoints(50));
			int32_t at1 = firstGroovePhrases(seed, macroPoints(1));
			expect(at25).to_be_between(4, 8);
			expect(at50).to_equal((at25 + 2) / 3); // the first line where phrases × 3 ≥ b
			expect(at1).to_be_between(3 * at25 - 3, 3 * at25);
			expect(firstGroovePhrases(seed, 0)).to_equal(-1);
		}
	});

	it("reads the live Motion at every phrase line, so turning it up ends a long Groove at the next line", _ {
		// At Motion 1 the first Groove runs for 12 or more phrases. Turned to 50 mid-phrase in its 6th, where 6
		// phrases × 3 is past any b, it ends at the end of that phrase.
		for (uint32_t seed = 1; seed <= 4; seed++) {
			constexpr int32_t kPhraseSteps = 8 * kStepsPerBar;
			constexpr int32_t kTurn = (2 + 5) * kPhraseSteps + 37; // Intro 2 phrases, then into the Groove's 6th
			Run run;
			run.recipe = technoRecipe();
			run.recipe.macros.set(Macro::Motion, macroPoints(1));
			run.seed = seed;
			run.steer = [](int32_t nextStep, SteeringQueue& queue) {
				if (nextStep == kTurn) {
					queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Motion), macroPoints(50)});
				}
			};
			auto sections = only(play(run, 8 * 12).events, EventKind::Section);
			expect(sections[2].section).to_equal(SectionKind::Break);
			expect(sections[2].tick).to_equal(stepTick((2 + 6) * kPhraseSteps));
		}
	});

	it("plays offsets unchanged where the centre leaves room, and scales each side to fit where it doesn't", _ {
		// Techno: the Build tops out at Energy +12, the Break dips Density to -20 (spec §9.4 headroom).
		Genre genre = genres::techno;
		genre.arc.groovePhrases = {1, 1};
		genre.arc.breakPhrases = {1, 1};
		auto offsets = [&](int32_t energy, int32_t density) {
			Macros centre;
			centre.set(Macro::Energy, macroPoints(energy));
			centre.set(Macro::Density, macroPoints(density));
			Arc arc;
			arc.start(genre);
			struct Seen {
				int32_t breakDensity = 0;
				int32_t buildTopEnergy = 0;
			} seen;
			for (int32_t step = 0; step < 6 * 8 * kStepsPerBar; step++) {
				arc.enterStep(genre, 1, step, centre);
				if (arc.section() == SectionKind::Break && arc.sectionStartsHere()) {
					seen.breakDensity = arc.offset(genre, Macro::Density, centre[Macro::Density]);
				}
				if (arc.section() == SectionKind::Build && arc.stepsToDrop() == 1) {
					seen.buildTopEnergy = arc.offset(genre, Macro::Energy, centre[Macro::Energy]);
					break;
				}
			}
			return seen;
		};
		auto roomy = offsets(25, 25);
		expect(roomy.breakDensity).to_equal(macroPoints(-20));
		expect(roomy.buildTopEnergy).to_equal(macroPoints(12));
		// Centre 45 has 5 points above it: the Build's top plays 12 × 5 / 12 and reaches exactly 50.
		auto high = offsets(45, 5);
		expect(high.buildTopEnergy).to_equal(macroPoints(5));
		// Centre 5 has 5 points below it: the -20 dip plays -20 × 5 / 20 and reaches exactly 0.
		expect(high.breakDensity).to_equal(macroPoints(-5));
	});

	it("raises Density from the Intro floor to the centre through the Intro, and keeps a centre below it", _ {
		// The lowest gate in the roster is the floor: a lane opens only with its part, so its gate is the higher.
		static constexpr Pattern kOn1[] = {{"X...............", "................", 0}};
		static constexpr LaneSpec kLanes[] = {{.lane = Lane::Kick, .pool = kOn1, .densityGate = macroPoints(4)},
		                                      {.lane = Lane::Rim, .pool = kOn1, .densityGate = macroPoints(12)}};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kLanes, .densityGate = macroPoints(8)}};
		static constexpr Genre kGated{.name = "gated", .parts = kParts, .arc = {.introPhrases = 1}};
		auto introDensity = [](int32_t centrePoints) {
			Macros centre;
			centre.set(Macro::Density, macroPoints(centrePoints));
			Arc arc;
			arc.start(kGated);
			std::vector<int32_t> out;
			for (int32_t step = 0; step < 8 * kStepsPerBar; step++) {
				arc.enterStep(kGated, 1, step, centre);
				out.push_back(centre[Macro::Density] + arc.offset(kGated, Macro::Density, centre[Macro::Density]));
			}
			return out;
		};
		auto rising = introDensity(30);
		expect(rising.front()).to_equal(macroPoints(8));
		expect(rising.back()).to_equal(macroPoints(30));
		for (size_t i = 1; i < rising.size(); i++) {
			expect(rising[i]).to_be_greater_than(rising[i - 1] - 1);
		}
		for (int32_t density : introDensity(6)) {
			expect(density).to_equal(macroPoints(6));
		}
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
