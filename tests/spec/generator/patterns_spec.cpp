// Patterns (spec §8.1, §6.6): Complexity tiers, the weighted keyed pick, Density thinning and when patterns change,
// read off the event stream of a small test genre whose pool entries each have one anchor on their own step, so a
// bar's hits name the entry playing.

#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr int32_t kPhraseBars = 2;

// Kick: band 1–3. Tier 1 has two entries, weighted 3:1; tier 2 is empty; tier 3 has one.
constexpr Pattern kKick[] = {
    {"........X.......", "................", 1, 3},
    {"............X...", "................", 1, 1},
    {"..X.............", "................", 3},
};
// Rim: band 0–1, one entry per tier, so a bar's hit names the tier.
constexpr Pattern kRim[] = {
    {"X...............", "................", 0},
    {"......X.........", "................", 1},
};
// Closed hat: one entry, no anchors, all the same importance, so the jitter alone decides which steps play.
constexpr Pattern kHat[] = {
    {"5555555555555555", "................", 0},
};
constexpr LaneSpec kLanes[] = {
    {.lane = Lane::Kick, .pool = kKick, .band = {1, 3}},
    {.lane = Lane::Rim, .pool = kRim, .band = {0, 1}},
    {.lane = Lane::ClosedHat, .pool = kHat, .band = {0, 0}},
};
constexpr PartSpec kParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kLanes}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{kPhraseBars, 1}};

/// Short phrases and fixed Section lengths at Motion 25, with no Arc offsets, so only the steering moves the macros
/// after the Intro (whose Density rises from the Intro floor, spec §9.4).
constexpr Genre kGenre{
    .name = "patterns",
    .facts = {.scales = kScales},
    .phraseBars = kPhraseBars,
    .parts = kParts,
    // A straight line, so the hat's importance 5 plays about half its steps at Density 25.
    .densityCurve = {{1000, 800, 600, 400, 200, 0}},
    .arc =
        {
            .introPhrases = 1,
            .groovePhrases = {4, 4},
            .breakPhrases = {1, 1},
            .buildBars = kBuildBars,
            .dropLiftEnergy = 0,
            .dropLiftDensity = 0,
            .offsets = {},
        },
};
/// Intro 1 + (Groove 4 + Break 1 + Build 1) phrases a cycle, at Motion 25.
constexpr int32_t kCyclePhrases = 6;
/// The first Groove's first bar.
constexpr int32_t kGrooveBar = kPhraseBars;

/// kGenre with no re-picks, so only Drops change the picks while the Arc cycles.
constexpr Genre kNoRepicks = [] {
	Genre genre = kGenre;
	genre.repickChance = 0;
	return genre;
}();

Run runAt(uint32_t seed, int32_t complexity, int32_t motion = 0, const Genre& genre = kGenre) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, macroPoints(motion));
	run.seed = seed;
	return run;
}

/// For each bar, the steps within the bar where `lane` hit.
std::vector<std::set<int32_t>> bars(const Result& result, Lane lane, int32_t count) {
	std::vector<std::set<int32_t>> out(count);
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == lane && e.tick / kBarTicks < count) {
			out[e.tick / kBarTicks].insert((e.tick % kBarTicks) / kTicksPerStep);
		}
	}
	return out;
}

/// The Section kind playing in each bar.
std::vector<SectionKind> sections(const Result& result, int32_t count) {
	std::vector<SectionKind> out(count, SectionKind::Intro);
	for (const Event& e : only(result.events, EventKind::Section)) {
		for (int32_t bar = e.tick / kBarTicks; bar < count; bar++) {
			out[bar] = e.section;
		}
	}
	return out;
}

/// Bars where a Drop lands: the first bar of every Groove but the first.
std::set<int32_t> dropBars(const Result& result) {
	std::set<int32_t> out;
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.section == SectionKind::Groove && e.occurrence > 0) {
			out.insert(e.tick / kBarTicks);
		}
	}
	return out;
}

/// Bars after the first Groove's first whose closed-hat shape differs from the bar before. The hat has one entry, so
/// a change is a new pick. Intro bars change anyway, as Density rises.
std::set<int32_t> hatChanges(const Result& result, int32_t count) {
	auto hat = bars(result, Lane::ClosedHat, count);
	std::set<int32_t> out;
	for (int32_t bar = kGrooveBar + 1; bar < count; bar++) {
		if (hat[bar] != hat[bar - 1]) {
			out.insert(bar);
		}
	}
	return out;
}

/// The step of the bar's one anchor hit, or -1.
int32_t anchorStep(const std::set<int32_t>& bar) {
	return bar.size() == 1 ? *bar.begin() : -1;
}

} // namespace

// clang-format off
describe patterns("Patterns", $ {
	it("play the band's lowest tier at Complexity 0 and its highest at 50", _ {
		for (uint32_t seed = 1; seed <= 20; seed++) {
			for (const auto& bar : bars(play(runAt(seed, 0), 8), Lane::Kick, 8)) {
				expect(anchorStep(bar) == 8 || anchorStep(bar) == 12).to_be_true();
			}
			for (const auto& bar : bars(play(runAt(seed, 50), 8), Lane::Kick, 8)) {
				expect(anchorStep(bar)).to_equal(2);
			}
		}
	});

	it("fall back from an empty tier to the nearest lower one", _ {
		// Complexity 25 maps band 1–3 to tier 2 exactly, with no remainder.
		for (uint32_t seed = 1; seed <= 20; seed++) {
			for (const auto& bar : bars(play(runAt(seed, 25), 8), Lane::Kick, 8)) {
				expect(anchorStep(bar) == 8 || anchorStep(bar) == 12).to_be_true();
			}
		}
	});

	it("pick within a tier by weight, keyed on the seed", _ {
		int32_t heavy = 0;
		int32_t light = 0;
		for (uint32_t seed = 1; seed <= 200; seed++) {
			int32_t step = anchorStep(bars(play(runAt(seed, 0), 1), Lane::Kick, 1)[0]);
			heavy += step == 8 ? 1 : 0;
			light += step == 12 ? 1 : 0;
		}
		expect(heavy + light).to_equal(200);
		expect(light).to_be_greater_than(20);
		expect(heavy).to_be_greater_than(2 * light);
	});

	it("take the next tier up by a keyed chance of the remainder, so a slow turn moves lanes one at a time", _ {
		// Rim's band is 0–1: Complexity is the chance of tier 1 (its anchor on step 6). Each seed's draw holds, so as
		// Complexity rises a seed only ever moves up, and about half the seeds are up at 25.
		int32_t upAt25 = 0;
		for (uint32_t seed = 1; seed <= 100; seed++) {
			bool up = false;
			for (int32_t complexity = 0; complexity <= 50; complexity += 5) {
				bool upHere = anchorStep(bars(play(runAt(seed, complexity), 1), Lane::Rim, 1)[0]) == 6;
				expect(!up || upHere).to_be_true();
				up = upHere;
				upAt25 += complexity == 25 && up ? 1 : 0;
			}
			expect(up).to_be_true();
		}
		expect(upAt25).to_be_between(30, 70);
	});

	it("hear a Complexity turn at the pattern's next wrap", _ {
		Run run = runAt(1, 0);
		run.steer = [](int32_t nextStep, SteeringQueue& queue) {
			if (nextStep == 3 * kStepsPerBar + 5) {
				queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Complexity), macroPoints(50)});
			}
		};
		auto kick = bars(play(run, 5), Lane::Kick, 5);
		expect(anchorStep(kick[3]) == 8 || anchorStep(kick[3]) == 12).to_be_true();
		expect(anchorStep(kick[4])).to_equal(2);
	});

	it("replay the same music when Complexity turns up and back down", _ {
		// Techno, whose hats and bass have tiered pools and whose bass rolls a line at each pick. Motion 0 holds the
		// line, so after the excursion every note matches a run that never moved.
		constexpr int32_t kBars = 2 * 8 + 16; // into the first Groove, which lasts at least 4 phrases
		constexpr int32_t kUpBar = 20;
		constexpr int32_t kDownBar = 24;
		for (uint32_t seed = 1; seed <= 5; seed++) {
			Run still;
			still.recipe = technoRecipe();
			still.recipe.macros.set(Macro::Motion, 0);
			still.seed = seed;
			Run turned = still;
			turned.steer = [](int32_t nextStep, SteeringQueue& queue) {
				auto complexity = static_cast<uint8_t>(Macro::Complexity);
				if (nextStep == kUpBar * kStepsPerBar + 3) {
					queue.push({CommandKind::SetMacro, complexity, macroPoints(50)});
				}
				if (nextStep == kDownBar * kStepsPerBar + 3) {
					queue.push({CommandKind::SetMacro, complexity, macroPoints(25)});
				}
			};
			auto expected = only(play(still, kBars).events, EventKind::NoteOn);
			auto actual = only(play(turned, kBars).events, EventKind::NoteOn);
			// From the wrap after the turn back down (2-bar patterns wrap on even bars).
			constexpr int32_t kFrom = (kDownBar + 2) * kBarTicks;
			std::vector<Event> expectedAfter;
			std::vector<Event> actualAfter;
			for (const Event& e : expected) {
				if (e.tick >= kFrom) {
					expectedAfter.push_back(e);
				}
			}
			for (const Event& e : actual) {
				if (e.tick >= kFrom) {
					actualAfter.push_back(e);
				}
			}
			expect(actualAfter == expectedAfter).to_be_true();
			expect(expected == actual).to_be_false(); // the excursion itself was heard
		}
	});

	it("keep the Intro's pick through the first Groove, Breaks and Builds, and pick afresh at each Drop", _ {
		constexpr int32_t kBars = 3 * kCyclePhrases * kPhraseBars;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Result result = play(runAt(seed, 25, 25, kNoRepicks), kBars);
			std::set<int32_t> drops = dropBars(result);
			expect(drops.size()).to_be_greater_than(size_t{1});
			expect(hatChanges(result, kBars) == drops).to_be_true();
			// The Intro and the first Groove share a pick: the same kick entry.
			auto kick = bars(result, Lane::Kick, kGrooveBar + 1);
			expect(anchorStep(kick[0])).to_equal(anchorStep(kick[kGrooveBar]));
		}
	});

	it("re-pick at phrase lines in a Groove, more often with more Motion, never at Motion 0", _ {
		constexpr int32_t kBars = 3 * kCyclePhrases * kPhraseBars;
		// Motion also paces the Arc (spec §9.3), so compare re-picks per phrase line where one may land.
		std::map<int32_t, int32_t> repicks; // by Motion
		std::map<int32_t, int32_t> lines;
		for (int32_t motion : {0, 10, 50}) {
			for (uint32_t seed = 1; seed <= 30; seed++) {
				Result result = play(runAt(seed, 25, motion), kBars);
				std::set<int32_t> drops = dropBars(result);
				std::vector<SectionKind> kinds = sections(result, kBars);
				for (int32_t bar = kGrooveBar + kPhraseBars; bar < kBars; bar += kPhraseBars) {
					bool inGroove = kinds[bar] == SectionKind::Groove && kinds[bar - kPhraseBars] == SectionKind::Groove;
					lines[motion] += inGroove && drops.count(bar) == 0 ? 1 : 0;
				}
				for (int32_t bar : hatChanges(result, kBars)) {
					if (drops.count(bar) != 0) {
						continue;
					}
					// A re-pick lands only on a phrase line inside a Groove, never on its first.
					expect(bar % kPhraseBars).to_equal(0);
					expect(kinds[bar]).to_equal(SectionKind::Groove);
					expect(kinds[bar - kPhraseBars]).to_equal(SectionKind::Groove);
					repicks[motion]++;
				}
			}
		}
		expect(repicks[0]).to_equal(0);
		expect(repicks[10]).to_be_greater_than(0);
		// Per line, Motion 50 re-picks more than twice as often as Motion 10.
		expect(repicks[50] * lines[10]).to_be_greater_than(2 * repicks[10] * lines[50]);
	});

	it("re-pick at the same phrase lines whatever the take, with each take's own pick", _ {
		constexpr int32_t kBars = 3 * kCyclePhrases * kPhraseBars;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run first = runAt(seed, 25, 50);
			Run other = first;
			other.takes.take[0] = 5;
			Result a = play(first, kBars);
			Result b = play(other, kBars);
			expect(hatChanges(a, kBars) == hatChanges(b, kBars)).to_be_true();
			expect(bars(a, Lane::ClosedHat, kBars) == bars(b, Lane::ClosedHat, kBars)).to_be_false();
		}
	});

	it("start a new take at the next phrase line, and replay the earlier take exactly on stepping back", _ {
		// Techno's Drums at Motion 25, so lanes re-pick at phrase lines while the takes change, and at Density 40,
		// so the jitter thins many steps.
		constexpr int32_t kPhrase = 8 * kStepsPerBar;
		constexpr int32_t kBars = 2 * 8 + 32;
		constexpr int32_t kNewTake = 2 * kPhrase + 40;  // lands on the phrase line at 3 phrases
		constexpr int32_t kStepBack = 4 * kPhrase + 7; // lands at 5 phrases
		for (uint32_t seed = 1; seed <= 5; seed++) {
			Run plain;
			plain.recipe = technoRecipe();
			plain.recipe.macros.set(Macro::Density, macroPoints(40));
			plain.seed = seed;
			Run stepped = plain;
			stepped.steer = [](int32_t nextStep, SteeringQueue& queue) {
				if (nextStep == kNewTake) {
					queue.push({CommandKind::SetTake, 0, 1});
				}
				if (nextStep == kStepBack) {
					queue.push({CommandKind::SetTake, 0, 0});
				}
			};
			LogEntry entries[8];
			SteeringLog log(entries, 8);
			stepped.log = &log;

			auto drums = [](const Result& result, int32_t fromStep, int32_t toStep) {
				std::vector<Event> out;
				for (const Event& e : only(result.events, EventKind::NoteOn)) {
					if (e.part == 0 && e.tick >= stepTick(fromStep) && e.tick < stepTick(toStep)) {
						out.push_back(e);
					}
				}
				return out;
			};
			Result a = play(plain, kBars);
			Result b = play(stepped, kBars);
			constexpr int32_t kEnd = kBars * kStepsPerBar;
			expect(drums(b, 0, 3 * kPhrase) == drums(a, 0, 3 * kPhrase)).to_be_true();
			expect(drums(b, 3 * kPhrase, 5 * kPhrase) == drums(a, 3 * kPhrase, 5 * kPhrase)).to_be_false();
			expect(drums(b, 5 * kPhrase, kEnd) == drums(a, 5 * kPhrase, kEnd)).to_be_true();

			// Logged where each take landed, not where it was pushed.
			expect(log.size()).to_equal(2);
			expect(log.at(0).step).to_equal(3 * kPhrase);
			expect(log.at(1).step).to_equal(5 * kPhrase);
		}
	});

	it("play exactly the anchors at Density 0, in every genre", _ {
		for (const Genre* genre : allGenres()) {
			// The Intro and the shortest first Groove: the Arc's Build offset would lift Density off 0.
			int32_t bars = genre->phraseBars * (genre->arc.introPhrases + genre->arc.groovePhrases.low);
			for (uint32_t seed = 1; seed <= 5; seed++) {
				Run run;
				run.recipe = newRecipe(*genre, seed);
				run.recipe.macros.set(Macro::Density, 0);
				run.seed = seed;
				auto ons = only(play(run, bars).events, EventKind::NoteOn);

				// Every part and lane, bar by bar: the steps it hit are exactly one pool entry's anchors.
				for (int32_t p = 0; p < static_cast<int32_t>(genre->parts.size()); p++) {
					const PartSpec& part = genre->parts[p];
					std::vector<std::pair<Lane, std::span<const Pattern>>> sources;
					if (!part.pool.empty()) {
						sources.emplace_back(Lane::None, part.pool);
					}
					for (const LaneSpec& lane : part.lanes) {
						sources.emplace_back(lane.lane, lane.pool);
					}
					for (const auto& [lane, pool] : sources) {
						std::set<int32_t> hits;
						for (const Event& e : ons) {
							if (e.part == p && e.lane == lane) {
								hits.insert(e.tick / kTicksPerStep);
							}
						}
						if (pool.empty()) {
							expect(hits.empty()).to_be_true(); // an FX lane: only section rules play it (spec §5.4)
							continue;
						}
						for (int32_t bar = 0; bar < bars; bar++) {
							bool matches = false;
							for (const Pattern& pattern : pool) {
								bool same = true;
								for (int32_t step = bar * kStepsPerBar; step < (bar + 1) * kStepsPerBar; step++) {
									bool anchor = pattern.importance[step % pattern.length] == kAnchor;
									same = same && anchor == (hits.count(step) != 0);
								}
								matches = matches || same;
							}
							expect(matches).to_be_true();
						}
					}
				}

				// Techno: the kick on every beat and nothing else.
				if (genre == &genres::techno) {
					expect(ons.size()).to_equal(static_cast<size_t>(bars * kStepsPerBar / kStepsPerBeat));
					for (const Event& e : ons) {
						expect(e.lane).to_equal(Lane::Kick);
						expect(e.tick % (kStepsPerBeat * kTicksPerStep)).to_equal(0);
					}
				}
			}
		}
		// The test genre's hat has no anchors, so it is silent.
		auto hat = bars(play(runAt(1, 25, 25), 8), Lane::ClosedHat, 8);
		Run silent = runAt(1, 25, 25);
		silent.recipe.macros.set(Macro::Density, 0);
		expect(bars(play(silent, 8), Lane::ClosedHat, 8) == std::vector<std::set<int32_t>>(8)).to_be_true();
		expect(hat == std::vector<std::set<int32_t>>(8)).to_be_false();
	});

	it("thin by the genre's Density curve when it sets one", _ {
		// A curve that keeps the hat's importance 5 out until Density 50.
		static constexpr Genre kSparse = [] {
			Genre genre = kGenre;
			genre.densityCurve = {{1000, 1000, 1000, 1000, 1000, 0}};
			return genre;
		}();
		Run run = runAt(1, 25, 25);
		run.recipe.genre = &kSparse;
		run.skipIntro = true; // the Recipe's Density from the first bar
		run.recipe.macros.set(Macro::Density, macroPoints(40));
		expect(bars(play(run, 4), Lane::ClosedHat, 4) == std::vector<std::set<int32_t>>(4)).to_be_true();
		run.recipe.macros.set(Macro::Density, macroPoints(50));
		for (const auto& bar : bars(play(run, 4), Lane::ClosedHat, 4)) {
			expect(bar.size()).to_equal(size_t{kStepsPerBar});
		}
	});
});

CPPSPEC_SPEC(patterns)
