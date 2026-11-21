// Part bias in the core (spec §3.3): a part plays at its biased value everywhere it reads a macro, notes and sound,
// its gate and the Arc plan agree with what it plays, and what the parts share (the Arc, the chord walk and its
// Extensions) stays unbiased. A bias of 0 changes nothing.

#include "harness.h"

#include "generative/core/arc_plan.h"
#include "generative/core/part_bias.h"

#include "cppspec.hpp"

#include <cstring>
#include <string_view>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBars = 32;

int32_t partNamed(const Genre& genre, std::string_view name) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (name == genre.parts[p].name) {
			return p;
		}
	}
	return -1;
}

Command setBias(int32_t part, Macro macro, int32_t units) {
	Command command{CommandKind::SetBias, static_cast<uint8_t>(part), units};
	command.length = static_cast<uint16_t>(macro);
	return command;
}

Run runOf(const Genre& genre) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.skipIntro = true;
	return run;
}

Run biased(const Genre& genre, int32_t part, Macro macro, int32_t points) {
	Run run = runOf(genre);
	run.recipe.bias.set(part, macro, macroPoints(points));
	return run;
}

std::vector<Event> ofKind(const std::vector<Event>& events, EventKind kind, int32_t part, bool samePart) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.kind == kind && (e.part == part) == samePart) {
			out.push_back(e);
		}
	}
	return out;
}

int32_t velocitySum(const std::vector<Event>& notes) {
	int32_t sum = 0;
	for (const Event& e : notes) {
		sum += e.velocity;
	}
	return sum;
}

/// Each phrase's plan for `part`, as the SONG page saw it on the phrase's first step, and whether a note of it started
/// in that phrase.
struct PhraseView {
	std::vector<bool> planned;
	std::vector<bool> heard;
};

PhraseView planAndNotes(Run run, int32_t part, int32_t phrases) {
	const int32_t phraseSteps = run.recipe.genre->phraseSteps();
	auto planned = std::make_shared<std::vector<bool>>(phrases, false);
	auto seen = std::make_shared<std::vector<bool>>(phrases, false);
	run.watch = [=](const Generator& generator) {
		const int32_t step = generator.nextStep();
		const int32_t phrase = (step - 1) / phraseSteps;
		if (step < 2 || phrase >= phrases || (*seen)[phrase] || (step - 1) % phraseSteps != 0) {
			return;
		}
		ArcPlan plan;
		generator.planArc(plan);
		(*seen)[phrase] = true;
		(*planned)[phrase] = (plan.phrase[0].sounds >> part & 1) != 0;
	};
	const Result result = play(run, phrases * phraseSteps / kStepsPerBar);
	PhraseView view{*planned, std::vector<bool>(phrases, false)};
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		const int32_t phrase = e.tick / (phraseSteps * kTicksPerStep);
		if (e.part == part && phrase < phrases) {
			view.heard[phrase] = true;
		}
	}
	return view;
}

bool sameChord(const Chord& a, const Chord& b) {
	return a.size == b.size && std::memcmp(a.steps, b.steps, sizeof(a.steps)) == 0;
}

} // namespace

// clang-format off
describe part_bias("Part bias", $ {
	it("leaves the stream as it was at bias 0, set or steered", _ {
		for (const Genre* genre : allGenres()) {
			const Result plain = play(runOf(*genre), kBars);
			Run steered = runOf(*genre);
			std::vector<std::pair<int32_t, Command>> commands;
			for (int32_t p = 0; p < partCount(*genre); p++) {
				for (int32_t m = 0; m < kNumMacros; m++) {
					commands.push_back({5, setBias(p, static_cast<Macro>(m), 0)});
				}
			}
			steered.steer = steerAt(commands);
			expect(play(steered, kBars).events == plain.events).to_be_true();
		}
	});

	it("plays a part's notes at its biased Energy and leaves the other parts' notes alone", _ {
		const Genre& genre = genres::technoAcid;
		const Result plain = play(runOf(genre), kBars);
		const Result louder = play(biased(genre, 1, Macro::Energy, 20), kBars);
		expect(ofKind(louder.events, EventKind::NoteOn, 1, false) == ofKind(plain.events, EventKind::NoteOn, 1, false))
		    .to_be_true();
		const std::vector<Event> before = ofKind(plain.events, EventKind::NoteOn, 1, true);
		const std::vector<Event> after = ofKind(louder.events, EventKind::NoteOn, 1, true);
		expect(after.size()).to_equal(before.size());
		expect(velocitySum(after)).to_be_greater_than(velocitySum(before));
	});

	it("moves the biased part's sound-control macro layer and no other part's", _ {
		const Genre& genre = genres::technoAcid;
		const Result plain = play(runOf(genre), kBars);
		const Result bright = play(biased(genre, 1, Macro::Energy, 20), kBars);
		expect(ofKind(bright.events, EventKind::Control, 1, false) == ofKind(plain.events, EventKind::Control, 1, false))
		    .to_be_true();
		expect(ofKind(bright.events, EventKind::Control, 1, true) == ofKind(plain.events, EventKind::Control, 1, true))
		    .to_be_false();
	});

	it("opens a gated part's gate at its biased Density, and the Arc plan shows what it plays", _ {
		const Genre& genre = genres::technoDetroit;
		const int32_t motif = partNamed(genre, "MOTIF");
		expect(motif).to_be_greater_than(-1);
		constexpr int32_t kPhrases = 6;
		// At the default 25 the Motif (gated at 27) plays on Drops only; +10 puts it in for the whole Groove, -10
		// keeps it out of the Drop too.
		const PhraseView up = planAndNotes(biased(genre, motif, Macro::Density, 10), motif, kPhrases);
		const PhraseView down = planAndNotes(biased(genre, motif, Macro::Density, -10), motif, kPhrases);
		const PhraseView plain = planAndNotes(runOf(genre), motif, kPhrases);
		int32_t upHeard = 0;
		int32_t plainHeard = 0;
		for (int32_t phrase = 1; phrase < kPhrases; phrase++) {
			expect(static_cast<bool>(up.planned[phrase])).to_equal(static_cast<bool>(up.heard[phrase]));
			expect(static_cast<bool>(down.planned[phrase])).to_equal(static_cast<bool>(down.heard[phrase]));
			expect(static_cast<bool>(down.heard[phrase])).to_be_false();
			upHeard += up.heard[phrase] ? 1 : 0;
			plainHeard += plain.heard[phrase] ? 1 : 0;
		}
		expect(upHeard).to_equal(kPhrases - 1);
		expect(plainHeard).to_be_less_than(upHeard);
	});

	it("plans an interplay target at its own biased Density", _ {
		const Genre& genre = genres::technoAcid;
		constexpr int32_t kBias = -15;
		auto biasedRun = std::make_shared<Generator>();
		auto plainRun = std::make_shared<Generator>();
		Recipe recipe = newRecipe(genre, 0);
		plainRun->start(recipe, 1, Takes{}, true);
		recipe.bias.set(1, Macro::Density, macroPoints(kBias));
		biasedRun->start(recipe, 1, Takes{}, true);
		SteeringQueue queue;
		for (int32_t s = 0; s < 4 * kStepsPerBar; s++) {
			biasedRun->decideNextStep(queue, nullptr);
			plainRun->decideNextStep(queue, nullptr);
		}
		const RuleTarget target{1, Lane::None};
		int32_t differs = 0;
		for (int32_t density = 0; density <= kMacroFull; density += macroPoints(5)) {
			for (int32_t s = 4 * kStepsPerBar; s < 6 * kStepsPerBar; s++) {
				const bool atOwn = biasedRun->testTargetPlanned(target, s, density);
				expect(atOwn).to_equal(plainRun->testTargetPlanned(target, s, partBiased(density, macroPoints(kBias))));
				differs += atOwn != plainRun->testTargetPlanned(target, s, density) ? 1 : 0;
			}
		}
		expect(differs).to_be_greater_than(0);
	});

	it("keeps the Arc and the chord walk shared under every bias", _ {
		const Genre& genre = genres::technoDetroit;
		auto chordsOf = [&](Run run) {
			// The chord of each bar, as the last decided step plays it: driver calls fall differently as the notes do.
			auto chords = std::make_shared<std::vector<Chord>>(64);
			run.watch = [chords](const Generator& generator) {
				const int32_t step = generator.nextStep() - 1;
				if (step >= 0 && step / kStepsPerBar < 64) { // voiced on the bar line, held through the bar
					(*chords)[step / kStepsPerBar] = generator.chord();
				}
			};
			run.skipIntro = false;
			Result result = play(run, 64);
			return std::make_pair(only(result.events, EventKind::Section), *chords);
		};
		Run all = runOf(genre);
		for (int32_t p = 0; p < partCount(genre); p++) {
			for (int32_t m = 0; m < kNumMacros; m++) {
				all.recipe.bias.set(p, static_cast<Macro>(m), macroPoints(m % 2 == 0 ? 20 : -20));
			}
		}
		const auto plain = chordsOf(runOf(genre));
		const auto moved = chordsOf(all);
		expect(moved.first == plain.first).to_be_true();
		expect(moved.second.size()).to_equal(plain.second.size());
		bool same = true;
		for (size_t i = 0; i < plain.second.size() && i < moved.second.size(); i++) {
			same = same && sameChord(plain.second[i], moved.second[i]);
		}
		expect(same).to_be_true();
	});

	it("lands a SetBias in the Recipe and the log, and refuses one past the roster or the macros", _ {
		const Genre& genre = genres::technoAcid;
		Run run = runOf(genre);
		TestLog entries(256);
		SteeringLog& log = entries.log;
		run.log = &log;
		run.steer = steerAt({{4, setBias(1, Macro::Motion, -macroPoints(12))},
		                     {6, setBias(partCount(genre), Macro::Energy, macroPoints(5))},
		                     {8, setBias(0, static_cast<Macro>(kNumMacros), macroPoints(5))},
		                     {10, setBias(0, Macro::Density, 2 * kMacroFull)}});
		const Result result = play(run, 2);
		expect(result.generator->recipe().bias.of(1, Macro::Motion)).to_equal(-macroPoints(12));
		expect(result.generator->recipe().bias.of(0, Macro::Density)).to_equal(kMacroFull);
		int32_t logged = 0;
		for (int32_t i = 0; i < logSize(log); i++) {
			logged += logAt(log, i).command.kind == CommandKind::SetBias ? 1 : 0;
		}
		expect(logged).to_equal(2);
	});
});

CPPSPEC_SPEC(part_bias)
