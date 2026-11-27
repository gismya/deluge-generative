// Parts and lanes enter and leave on bar lines (spec §5.6, §6.7, #451). Density reaches the notes once a bar: the
// bar line latches the effective Density and each part's Density bias, so neither the Arc's ramps (Intro, Build, a
// Drop's decay, Outro) nor a live Density turn or Part bias starts or stops a hit mid-bar. A part unfrozen mid-bar
// keeps its pattern to the bar line. What may still start or stop off a bar line is not Density's: fills, the Build
// roll and the Gap, Throws and pickups (none of them a planned onset), and a pattern's own first or last
// hit, a jitter split within an importance tier among them.

#include "harness.h"
#include "stream_helpers.h"

#include "generative/core/variants.h"

#include "cppspec.hpp"

#include <array>
#include <utility>
#include <vector>

using namespace harness;

namespace {

Command density(int32_t points) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), macroPoints(points)};
}

Command densityBias(uint8_t part, int32_t points) {
	Command command{CommandKind::SetBias, part, macroPoints(points)};
	command.length = static_cast<uint16_t>(Macro::Density);
	return command;
}

/// Every part's and lane's planned onsets, as an interplay rule reads them: a pitched part whole, a Kit part by lane.
std::vector<RuleTarget> targetsOf(const Genre& genre) {
	std::vector<RuleTarget> out;
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		if (part.lanes.empty()) {
			if (!part.pool.empty()) {
				out.push_back({static_cast<uint8_t>(p), Lane::None});
			}
			continue;
		}
		for (int32_t l = 0; l < laneCount(part); l++) {
			if (!part.lanes[l].pool.empty()) {
				out.push_back({static_cast<uint8_t>(p), part.lanes[l].lane});
			}
		}
	}
	return out;
}

/// On each decided step, each target's planned onset at the Density the step read, at the Arc's Density as the bar
/// line had it, and at the Arc's Density on the step itself.
struct BarDensity : DecidedHook<BarDensity> {
	std::vector<RuleTarget> targets;
	int32_t barLive = 0;
	int32_t moved = 0;     ///< Planned onsets the step's Density gave otherwise than the bar line's: must stay 0.
	int32_t ramped = 0;    ///< Steps where the Arc's Density had moved since the bar line.
	int32_t wouldFlip = 0; ///< Planned onsets the Arc's Density on the step itself would have turned on or off.
	void decided(const Generator& g, int32_t step, int32_t density) {
		const int32_t live = g.effectiveMacro(Macro::Density);
		if (step % kStepsPerBar == 0) {
			barLive = live;
		}
		ramped += live != barLive ? 1 : 0;
		for (const RuleTarget& target : targets) {
			const bool plays = g.testTargetPlanned(target, step, density);
			moved += plays != g.testTargetPlanned(target, step, barLive) ? 1 : 0;
			wouldFlip += plays != g.testTargetPlanned(target, step, live) ? 1 : 0;
		}
	}
};

/// The pool entries of parts 0 and 1, by lane for a Kit part, on each decided step.
struct Entries : DecidedHook<Entries> {
	std::vector<std::vector<int32_t>> byStep;
	void decided(const Generator& g, int32_t step, int32_t) {
		if (static_cast<int32_t>(byStep.size()) <= step) {
			byStep.resize(step + 1);
		}
		byStep[step].clear();
		for (int32_t part : {0, 1}) {
			for (int32_t lane = 0; lane < kMaxLanesPerPart; lane++) {
				byStep[step].push_back(g.testEntry(part, lane));
			}
		}
	}
};

Run grooveOf(const Genre& genre) {
	Run run;
	run.recipe = writtenVariants(newRecipe(genre, 0));
	run.skipIntro = true;
	return run;
}

/// NoteOns alone: a Density turn still moves the sound controls on its own step.
std::vector<Event> noteOns(const Run& run, int32_t bars) {
	return only(play(run, bars).events, EventKind::NoteOn);
}

} // namespace

// clang-format off
describe bar_lines("Bar lines", $ {
	it("plays each bar's planned onsets at the Density its bar line latched, through every Arc ramp", _ {
		// From an Intro through Grooves, a Build and its Drop's decay, to the Outro End brings.
		for (const Genre* genre : allGenres()) {
			int32_t ramped = 0;
			int32_t wouldFlip = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				Run run;
				run.recipe = newRecipe(*genre, seed);
				run.seed = seed;
				run.steer = at(120 * kStepsPerBar, {CommandKind::End, 0, 1});
				BarDensity hook;
				ResolvedGenre resolved;
				hook.targets = targetsOf(resolveGenre(*genre, run.recipe.variants, &resolved));
				const Result result = play(run, 260);
				expect(result.ended).to_be_true();
				expect(hook.moved).to_equal(0);
				ramped += hook.ramped;
				wouldFlip += hook.wouldFlip;
			}
			// The ramps moved Density mid-bar, across a threshold the notes would otherwise have crossed there.
			expect(ramped).to_be_greater_than(0);
			expect(wouldFlip).to_be_greater_than(0);
		}
	});

	it("lands a live Density turn on the notes at the next bar line", _ {
		for (const Genre* genre : allGenres()) {
			std::vector<std::pair<int32_t, Command>> midBar;
			std::vector<std::pair<int32_t, Command>> onLines;
			for (int32_t bar = 1; bar < 12; bar++) {
				const int32_t points = bar % 2 == 0 ? 45 : 5;
				midBar.emplace_back(bar * kStepsPerBar + 1 + bar % 15, density(points));
				onLines.emplace_back((bar + 1) * kStepsPerBar, density(points));
			}
			Run turned = grooveOf(*genre);
			turned.steer = steerAt(midBar);
			Run lined = grooveOf(*genre);
			lined.steer = steerAt(onLines);
			const std::vector<Event> notes = noteOns(turned, 12);
			expect(notes == noteOns(lined, 12)).to_be_true();
			expect(notes == noteOns(grooveOf(*genre), 12)).to_be_false(); // the turns were heard
		}
	});

	it("lands a live Density bias on a part's notes at the next bar line", _ {
		for (const Genre* genre : allGenres()) {
			std::vector<std::pair<int32_t, Command>> midBar;
			std::vector<std::pair<int32_t, Command>> onLines;
			for (int32_t bar = 1; bar < 12; bar++) {
				const int32_t points = std::array{20, 0, -20}[bar % 3]; // 0 clears the bias mid-bar
				for (uint8_t part : {uint8_t{0}, uint8_t{1}}) {
					midBar.emplace_back(bar * kStepsPerBar + 3 + bar % 12, densityBias(part, points));
					onLines.emplace_back((bar + 1) * kStepsPerBar, densityBias(part, points));
				}
			}
			Run turned = grooveOf(*genre);
			turned.steer = steerAt(midBar);
			Run lined = grooveOf(*genre);
			lined.steer = steerAt(onLines);
			const std::vector<Event> notes = noteOns(turned, 12);
			expect(notes == noteOns(lined, 12)).to_be_true();
			expect(notes == noteOns(grooveOf(*genre), 12)).to_be_false();
		}
	});

	it("keeps an unfrozen part's pattern to the bar line, then takes the pick that waited", _ {
		// Every phrase line may re-pick at Motion 50. Parts 0 and 1 freeze in bar 1 and unfreeze on step 7 of the
		// third phrase's first bar, with the re-picks of two phrase lines waiting; the bar line after it is no phrase
		// line, so only the waiting pick can change the pattern there.
		int32_t tookWaiting = 0;
		for (const Genre* genre : allGenres()) {
			Run run = grooveOf(*genre);
			run.recipe.macros.set(Macro::Motion, macroPoints(50));
			const int32_t thawBar = 2 * genre->phraseBars;
			const int32_t thaw = thawBar * kStepsPerBar + 7;
			run.steer = steerAt({{kStepsPerBar + 5, {CommandKind::SetFreeze, 0, 1}},
			                     {kStepsPerBar + 5, {CommandKind::SetFreeze, 1, 1}},
			                     {thaw, {CommandKind::SetFreeze, 0, 0}},
			                     {thaw, {CommandKind::SetFreeze, 1, 0}}});
			Entries entries;
			play(run, thawBar + 2);
			const int32_t line = (thawBar + 1) * kStepsPerBar;
			for (int32_t step = thaw; step < line; step++) {
				expect(entries.byStep[step] == entries.byStep[thaw - 1]).to_be_true();
			}
			tookWaiting += entries.byStep[line] != entries.byStep[thaw - 1] ? 1 : 0;
		}
		expect(tookWaiting).to_be_greater_than(0);
	});
});

CPPSPEC_SPEC(bar_lines)
