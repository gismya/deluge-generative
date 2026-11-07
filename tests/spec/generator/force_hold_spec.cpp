// Force and hold through the core's pending list (spec §2.6, §9.6), as they show in the event stream and the log.

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kGroove0 = 2 * kPhraseSteps;     // Techno's Intro is 2 phrases

Command force(SectionKind kind) {
	return {CommandKind::ForceSection, static_cast<uint8_t>(kind), 0};
}
Command hold(bool on) {
	return {CommandKind::SetHold, 0, on ? 1 : 0};
}

/// Techno with a fixed Build length.
template <uint8_t Bars>
struct BuildOf {
	static constexpr WeightedBars kBars[] = {{Bars, 1}};
	static inline const Genre genre = [] {
		Genre g = genres::techno;
		g.arc.buildBars = kBars;
		return g;
	}();
};

struct Played {
	std::vector<Event> sections;
	std::vector<LogEntry> log;
};

Played playSteered(const Genre& genre, int32_t bars, std::vector<std::pair<int32_t, Command>> commands,
                   int32_t motion = macroPoints(25)) {
	static LogEntry entries[64];
	SteeringLog log(entries, 64);
	Run run;
	run.recipe = technoRecipe();
	run.recipe.genre = &genre;
	run.recipe.macros.set(Macro::Motion, motion);
	run.seed = 3;
	run.log = &log;
	run.steer = steerAt(std::move(commands));
	Played out;
	out.sections = only(play(run, bars).events, EventKind::Section);
	for (int32_t i = 0; i < log.size(); i++) {
		out.log.push_back(log.at(i));
	}
	return out;
}

/// The first Section of `kind` starting at or after `fromStep`, or nullptr.
const Event* firstFrom(const std::vector<Event>& sections, SectionKind kind, int32_t fromStep) {
	for (const Event& e : sections) {
		if (e.section == kind && e.tick >= stepTick(fromStep)) {
			return &e;
		}
	}
	return nullptr;
}

} // namespace

// clang-format off
describe force_hold("Force and hold", $ {
	it("starts a forced Section on the next phrase line and logs it there, not where it was pressed", _ {
		for (SectionKind kind : {SectionKind::Break, SectionKind::Groove}) {
			Played played = playSteered(genres::techno, 8 * 6, {{kGroove0 + 40, force(kind)}});
			const Event* forced = firstFrom(played.sections, kind, kGroove0 + 1);
			expect(forced != nullptr).to_be_true();
			expect(forced->tick).to_equal(stepTick(kGroove0 + kPhraseSteps));
			expect(played.log.size()).to_equal(size_t{1});
			expect(played.log[0].step).to_equal(kGroove0 + kPhraseSteps);
			expect(played.log[0].command.kind).to_equal(CommandKind::ForceSection);
			expect(played.log[0].command.target).to_equal(static_cast<uint8_t>(kind));
		}
	});

	it("starts a forced Build at the earliest bar line that lets it end on a phrase line", _ {
		// A 4-bar Build forced in bar 6 of an 8-bar phrase starts at bar 13 (spec §9.6).
		constexpr int32_t kPressed = kGroove0 + 5 * kStepsPerBar + 3;
		Played played = playSteered(BuildOf<4>::genre, 8 * 6, {{kPressed, force(SectionKind::Build)}});
		const Event* build = firstFrom(played.sections, SectionKind::Build, kGroove0);
		expect(build != nullptr).to_be_true();
		expect(build->tick).to_equal(stepTick(kGroove0 + 12 * kStepsPerBar));
		const Event* drop = firstFrom(played.sections, SectionKind::Groove, kGroove0 + 1);
		expect(drop->tick).to_equal(stepTick(kGroove0 + 2 * kPhraseSteps));
		expect(played.log[0].step).to_equal(kGroove0 + 12 * kStepsPerBar);
		expect(played.log[0].command.value).to_equal(4 * kStepsPerBar); // the length drawn when forced
	});

	it("lands the Drop of a forced Build of any length on a phrase line", _ {
		// Pressed in the Intro, at and inside a Groove's phrases, and inside a Break (forced first, from step 384's
		// phrase line) in its 3rd bar, before any natural Build could take the end of its phrase.
		constexpr int32_t kInBreak = kGroove0 + kPhraseSteps + 2 * kStepsPerBar + 3;
		auto check = [&](const Genre& genre, int32_t bars) {
			for (int32_t pressed : {kGroove0 + 1, kGroove0 + 3 * kStepsPerBar + 7, kGroove0 + kPhraseSteps - 3,
			                        kGroove0 / 2 + 5, kInBreak}) {
				std::vector<std::pair<int32_t, Command>> commands = {{pressed, force(SectionKind::Build)}};
				if (pressed == kInBreak) {
					commands.insert(commands.begin(), {kGroove0 + 1, force(SectionKind::Break)});
				}
				Played played = playSteered(genre, 8 * 12, commands);
				if (pressed == kInBreak) {
					const Event* playing = nullptr; // the Section playing when it was pressed
					for (const Event& e : played.sections) {
						playing = e.tick <= stepTick(pressed) ? &e : playing;
					}
					expect(playing != nullptr && playing->section == SectionKind::Break).to_be_true();
				}
				const Event* build = firstFrom(played.sections, SectionKind::Build, pressed);
				expect(build != nullptr).to_be_true();
				if (build == nullptr) {
					continue;
				}
				expect(build->tick - stepTick(pressed)).to_be_greater_than(-1);
				const Event* drop = firstFrom(played.sections, SectionKind::Groove, build->tick / kTicksPerStep + 1);
				expect(drop != nullptr).to_be_true();
				if (drop == nullptr) {
					continue;
				}
				expect(drop->tick - build->tick).to_equal(stepTick(bars * kStepsPerBar));
				expect(drop->tick % stepTick(kPhraseSteps)).to_equal(0);
			}
		};
		check(BuildOf<1>::genre, 1);
		check(BuildOf<2>::genre, 2);
		check(BuildOf<4>::genre, 4);
		check(BuildOf<8>::genre, 8);
		check(BuildOf<16>::genre, 16);
		check(BuildOf<32>::genre, 32);
	});

	it("cancels a queued force when it is pressed again, and never logs it", _ {
		Played played = playSteered(genres::techno, 8 * 4,
		                            {{kGroove0 + 10, force(SectionKind::Break)}, {kGroove0 + 20, force(SectionKind::Break)}});
		expect(firstFrom(played.sections, SectionKind::Break, kGroove0) == nullptr).to_be_true();
		expect(played.log.empty()).to_be_true();
	});

	it("replaces a queued force of another kind", _ {
		Played played = playSteered(genres::techno, 8 * 5,
		                            {{kGroove0 + 10, force(SectionKind::Break)}, {kGroove0 + 20, force(SectionKind::Build)}});
		const Event* build = firstFrom(played.sections, SectionKind::Build, kGroove0);
		expect(build != nullptr).to_be_true();
		expect(build->tick).to_equal(stepTick(kGroove0 + kPhraseSteps));
		expect(firstFrom(played.sections, SectionKind::Break, kGroove0) == nullptr).to_be_true();
		expect(played.log.size()).to_equal(size_t{1});
	});

	it("holds the current Section until released, then lets the Arc move on at the next line", _ {
		// At Motion 50 Techno's first Groove is 2 or 3 phrases. Held from its first bar to its 6th phrase, it plays
		// on, and ends on the line after the release.
		constexpr int32_t kRelease = kGroove0 + 5 * kPhraseSteps + 9;
		Played played = playSteered(genres::techno, 8 * 12, {{kGroove0 + 3, hold(true)}, {kRelease, hold(false)}},
		                            macroPoints(50));
		const Event* next = firstFrom(played.sections, SectionKind::Break, kGroove0);
		expect(next != nullptr).to_be_true();
		expect(next->tick).to_equal(stepTick(kGroove0 + 6 * kPhraseSteps));
		expect(played.log.size()).to_equal(size_t{2});
		expect(played.log[0].step).to_equal(kGroove0 + 3); // a hold lands on the next undecided step
	});

	it("plays a forced Section during a hold, then holds wherever the Arc lands", _ {
		// Held in the Groove, a forced Break plays and is held in turn: no Build follows while the hold lasts.
		Played played = playSteered(genres::techno, 8 * 14,
		                            {{kGroove0 + 3, hold(true)}, {kGroove0 + 20, force(SectionKind::Break)}});
		const Event* breakStart = firstFrom(played.sections, SectionKind::Break, kGroove0);
		expect(breakStart != nullptr).to_be_true();
		expect(breakStart->tick).to_equal(stepTick(kGroove0 + kPhraseSteps));
		expect(firstFrom(played.sections, SectionKind::Build, kGroove0) == nullptr).to_be_true();
	});

	it("still lands a held Build on its Drop, and holds the Groove it lands on", _ {
		Played played = playSteered(BuildOf<8>::genre, 8 * 14,
		                            {{kGroove0 + 20, force(SectionKind::Build)}, {kGroove0 + kPhraseSteps + 5, hold(true)}});
		const Event* drop = firstFrom(played.sections, SectionKind::Groove, kGroove0 + 1);
		expect(drop != nullptr).to_be_true();
		expect(drop->tick).to_equal(stepTick(kGroove0 + 2 * kPhraseSteps));
		expect(drop->occurrence).to_equal(uint16_t{1});
		expect(firstFrom(played.sections, SectionKind::Break, kGroove0) == nullptr).to_be_true();
	});

	it("starts every piece fresh: a stop cancels whatever is pending", _ {
		// A stop cancels the pending list: every start is fresh (spec §2.6, §13.2).
		Generator generator;
		SteeringQueue queue;
		generator.start(technoRecipe(), 1, {});
		generator.decideNextStep(queue, nullptr); // past step 0, a phrase line where they would land at once
		queue.push(force(SectionKind::Break));
		queue.push({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Dorian)});
		queue.push({CommandKind::SetTake, 0, 3});
		for (int32_t i = 0; i < 5; i++) {
			generator.decideNextStep(queue, nullptr);
		}
		expect(generator.keyChangePending()).to_be_true();
		expect(generator.sectionForced()).to_be_true();
		generator.start(technoRecipe(), 1, {});
		expect(generator.keyChangePending()).to_be_false();
		expect(generator.sectionForced()).to_be_false();
		for (int32_t step = 0; step <= kGroove0; step++) {
			generator.decideNextStep(queue, nullptr);
		}
		expect(generator.arc().section()).to_equal(SectionKind::Groove);
		expect(generator.recipe().keyRoot).to_equal(uint8_t{9});
	});
});

CPPSPEC_SPEC(force_hold)
