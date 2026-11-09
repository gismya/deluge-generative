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

	it("never re-strikes the Riser when a forced Groove cuts a Build short", _ {
		// A 16-bar Build from step 384 with its Riser held to the Drop, cut by a Groove forced onto 512.
		constexpr int32_t kBuild = kGroove0 + kPhraseSteps;
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &BuildOf<16>::genre;
		run.seed = 3;
		run.steer = steerAt({{kGroove0 + 1, force(SectionKind::Build)}, {kBuild + 9, force(SectionKind::Groove)}});
		Result result = play(run, 8 * 6);
		auto sections = only(result.events, EventKind::Section);
		const Event* build = firstFrom(sections, SectionKind::Build, kGroove0);
		const Event* groove = firstFrom(sections, SectionKind::Groove, kBuild + 1);
		expect(build != nullptr && build->tick == stepTick(kBuild)).to_be_true();
		expect(groove != nullptr && groove->tick == stepTick(kBuild + kPhraseSteps)).to_be_true();
		std::vector<int32_t> riserOns;
		int32_t riserOff = -1;
		for (const Event& e : result.events) {
			if (e.lane == Lane::Riser && e.kind == EventKind::NoteOn) {
				riserOns.push_back(e.tick);
			}
			if (e.lane == Lane::Riser && e.kind == EventKind::NoteOff && riserOff < 0) {
				riserOff = e.tick;
			}
		}
		expect(riserOns.size()).to_equal(size_t{1});
		expect(riserOff).to_equal(stepTick(kBuild + kPhraseSteps)); // it ends on the cutting line and rings out
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

	it("gives a Groove forced from a Break its phrase line's re-pick chance, picking as the Drop would", _ {
		// Two kick patterns and a re-pick at every Groove phrase line. Groove 0 plays one phrase from step 128; a
		// natural Break of two phrases and an 8-bar Build bring the Drop into Groove 1 at 640. Forced from the Break at 384, Groove 1
		// starts with no Drop: its first step is a phrase line in a Groove, so it re-picks there, keyed as the Drop's
		// pick (spec §8.1).
		static constexpr Pattern kKicks[] = {{"X...X...X...X...", "................", 0},
		                                     {"X.X.X.X.X.X.X.X.", "................", 0}};
		static constexpr LaneSpec kLanes[] = {{.lane = Lane::Kick, .pool = kKicks}};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kLanes}};
		static constexpr Scale kScales[] = {Scale::Minor};
		static constexpr Genre kGenre{.name = "repick",
		                              .facts = {.scales = kScales},
		                              .parts = kParts,
		                              .repickChance = 1000,
		                              .arc = {.introPhrases = 1, .groovePhrases = {1, 1}, .breakPhrases = {2, 2}}};
		auto kicks = [](const std::vector<Event>& events, int32_t from) {
			int32_t n = 0;
			for (const Event& e : only(events, EventKind::NoteOn)) {
				n += e.tick >= stepTick(from) && e.tick < stepTick(from + kStepsPerBar) ? 1 : 0;
			}
			return n;
		};
		bool differs = false;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			Run natural;
			natural.recipe = newRecipe(kGenre, 0);
			natural.seed = seed;
			const std::vector<Event> plain = play(natural, 8 * 6).events;
			Run forced = natural;
			forced.steer = at(300, force(SectionKind::Groove));
			const std::vector<Event> steered = play(forced, 8 * 4).events;
			// The forced Groove plays the Drop's pick, wherever Groove 0's differed from it.
			expect(kicks(steered, 3 * kPhraseSteps)).to_equal(kicks(plain, 5 * kPhraseSteps));
			differs = differs || kicks(plain, kPhraseSteps) != kicks(plain, 5 * kPhraseSteps);
		}
		expect(differs).to_be_true();
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
