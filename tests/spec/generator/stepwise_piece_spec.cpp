// The harness's stepwise piece (spec §12.3, §21.2): what its two ways to reach a step mean, so the specs written on
// it can rely on them.

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kAcid = 1;
constexpr Command kMuteNow{CommandKind::SetMute, kAcid, kPressNow}; // lands on the next undecided step

/// Stamps each event with the driver's clock and the Hook's song tick, to show a sink is handed both.
struct StampingSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	const Hook* hook = nullptr;
	std::vector<std::pair<int32_t, int64_t>> stamps; ///< (driver clock, song tick)
	void attach(const perform::PerformDriver& playing, const Hook& calls) {
		driver = &playing;
		hook = &calls;
	}
	void play(const Event&) override { stamps.push_back({driver->clock(), hook->now()}); }
};

std::vector<Event> before(const std::vector<Event>& events, int32_t step) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.tick < stepTick(step)) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe stepwise_piece("The stepwise piece", $ {
	it("plays the stream play() plays, from the same Recipe, seed, takes and start", _ {
		for (bool skipIntro : {false, true}) {
			Run run;
			run.recipe = technoRecipe();
			run.seed = 7;
			run.takes.take[kAcid] = 2;
			run.skipIntro = skipIntro;
			StepwisePiece<> piece{{.seed = 7, .takes = run.takes, .skipIntro = skipIntro}};
			piece.runBars(8);
			// play() stops before the last line's call, so compare what played before the last bar.
			expect(before(piece.sink.events, 7 * kStepsPerBar) == before(play(run, 8).events, 7 * kStepsPerBar))
			    .to_be_true();
		}
	});

	it("calls nothing until the first run, so a push before it is there for step 0", _ {
		StepwisePiece<> piece{{.skipIntro = true, .logEntries = 4}};
		expect(piece.generator.nextStep()).to_equal(0);
		expect(piece.sink.events.empty()).to_be_true();
		piece.push(kMuteNow);
		piece.runTicks(0); // the first call alone
		expect(piece.generator.nextStep()).to_equal(1);
		expect(piece.log.size()).to_equal(1);
		expect(piece.log.at(0).step).to_equal(0);
	});

	it("decides every step before the one named: it is the next to decide, and a push now reaches it", _ {
		StepwisePiece<> piece{{.skipIntro = true, .logEntries = 4}};
		piece.decideBefore(20);
		expect(piece.generator.nextStep()).to_equal(20);
		// The clock stands on step 19's decision tick: decided, not heard.
		expect(piece.driver.clock()).to_equal(decisionTick(19));
		for (const Event& e : piece.sink.events) {
			expect(e.tick).to_be_less_than(stepTick(19));
		}
		piece.push(kMuteNow);
		piece.decideBefore(21);
		expect(piece.log.size()).to_equal(1);
		expect(piece.log.at(0).step).to_equal(20);
		// Already there: nothing moves.
		piece.decideBefore(5);
		expect(piece.generator.nextStep()).to_equal(21);
	});

	it("runs to a step's grid line: the line has played, the next step is undecided, and a push now reaches that", _ {
		StepwisePiece<> piece{{.skipIntro = true, .logEntries = 4}};
		piece.runToStepLine(16);
		expect(piece.hook.now()).to_equal(int64_t{stepTick(16)});
		expect(piece.generator.nextStep()).to_equal(17);
		bool kickOnLine = false;
		for (const Event& e : piece.sink.events) {
			kickOnLine |= e.kind == EventKind::NoteOn && e.lane == Lane::Kick && e.tick == stepTick(16);
		}
		expect(kickOnLine).to_be_true();
		piece.push(kMuteNow);
		piece.runToStepLine(18);
		expect(piece.log.size()).to_equal(1);
		expect(piece.log.at(0).step).to_equal(17);
	});

	it("runs to a line less or plus some ticks, and not back", _ {
		StepwisePiece<> piece{{.skipIntro = true}};
		piece.runToStepLine(16, -13); // the last tick a press is in time for step 16's decision
		expect(piece.hook.now()).to_equal(int64_t{decisionTick(16) - 1});
		expect(piece.generator.nextStep()).to_equal(16);
		piece.runToStepLine(16, 5);
		expect(piece.hook.now()).to_equal(int64_t{stepTick(16) + 5});
		piece.runToStepLine(10);
		expect(piece.hook.now()).to_equal(int64_t{stepTick(16) + 5});
	});

	it("reaches the same step at any song magnitude", _ {
		for (int32_t magnitude : {1, 2, 3, 4}) {
			StepwisePiece<> lined{{.skipIntro = true, .magnitude = magnitude}};
			lined.runToStepLine(16);
			expect(lined.driver.clock()).to_equal(stepTick(16));
			expect(lined.generator.nextStep()).to_equal(17);
			lined.runBars(1);
			expect(lined.driver.clock()).to_equal(stepTick(32));
			lined.runSteps(3);
			expect(lined.driver.clock()).to_equal(stepTick(35));

			StepwisePiece<> decided{{.skipIntro = true, .magnitude = magnitude}};
			decided.decideBefore(20);
			expect(decided.generator.nextStep()).to_equal(20);
			expect(decided.driver.clock()).to_equal(decisionTick(19));
		}
	});

	it("pushes a press at its song tick during a run", _ {
		StepwisePiece<> piece{{.skipIntro = true, .logEntries = 4}};
		// Pressed on step 9's decision tick, after its call: in time for step 10.
		piece.runTicks(stepTick(12), {piece.press(decisionTick(9), kMuteNow)});
		expect(piece.log.size()).to_equal(1);
		expect(piece.log.at(0).step).to_equal(10);
	});

	it("takes a pushed command into the pending list by deciding one more step", _ {
		StepwisePiece<> piece{{.skipIntro = true}};
		piece.decideBefore(5);
		piece.pushAndDecide({CommandKind::SetMute, kAcid, kPress});
		expect(piece.generator.nextStep()).to_equal(6);
		expect(piece.generator.mutePending(kAcid)).to_be_true();
	});

	it("makes a fresh piece's first call when asked for step 0's line", _ {
		StepwisePiece<> piece{{.skipIntro = true}};
		piece.push(kMuteNow);
		piece.runToStepLine(0);
		expect(piece.generator.nextStep()).to_equal(1); // step 0 is decided: a push now reaches step 1
		expect(piece.generator.partMuted(kAcid)).to_be_true();
		expect(piece.driver.clock()).to_equal(0);
	});

	it("says whether the step was reached: not once the piece has stopped", _ {
		StepwisePiece<> piece{{.skipIntro = true}};
		expect(piece.decideBefore(5)).to_be_true();
		expect(piece.decideBefore(3)).to_be_true(); // there already
		piece.driver.stop(piece.sink);
		expect(piece.decideBefore(9)).to_be_false();
		expect(piece.pushAndDecide(kMuteNow)).to_be_false();
		expect(piece.generator.nextStep()).to_equal(5);
	});

	it("starts again from the Start it was made with unless given another", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
		piece.decideBefore(5);
		piece.push(kMuteNow);
		piece.restart();
		expect(piece.queue.size()).to_equal(0); // a start drops what was queued
		expect(piece.generator.nextStep()).to_equal(0);
		piece.decideBefore(1);
		expect(piece.generator.arc().section() == SectionKind::Groove).to_be_true(); // still skipIntro
	});

	it("runs with no log unless the start asks for one", _ {
		StepwisePiece<> piece{{.skipIntro = true}};
		piece.push(kMuteNow);
		piece.decideBefore(2);
		expect(piece.generator.partMuted(kAcid)).to_be_true();
		expect(piece.log.size()).to_equal(0);
	});

	it("hands a sink that asks for them the driver and the Hook", _ {
		StepwisePiece<StampingSink> piece{{.skipIntro = true}};
		piece.runBars(1);
		expect(piece.sink.stamps.empty()).to_be_false();
		for (auto [clock, songTick] : piece.sink.stamps) {
			expect(int64_t{clock}).to_equal(songTick); // magnitude 2 from tick 0: they agree
		}
	});

	it("starts again from nothing: the pending list and the log are gone, and step 0 is next", _ {
		StepwisePiece<> piece{{.skipIntro = true, .logEntries = 4}};
		piece.decideBefore(5);
		piece.push(kMuteNow);
		piece.pushAndDecide({CommandKind::SetSolo, 0, kPress});
		expect(piece.generator.soloPending(0)).to_be_true();
		expect(piece.log.size()).to_equal(1);
		piece.restart({.skipIntro = true});
		expect(piece.driver.running()).to_be_true();
		expect(piece.generator.nextStep()).to_equal(0);
		expect(piece.generator.soloPending(0)).to_be_false();
		expect(piece.generator.partMuted(kAcid)).to_be_false();
		expect(piece.log.size()).to_equal(0);
		piece.runToStepLine(16);
		expect(piece.driver.clock()).to_equal(stepTick(16));
		expect(piece.generator.nextStep()).to_equal(17);
	});
});

CPPSPEC_SPEC(stepwise_piece)
