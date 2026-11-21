// Restart AS IT IS (spec §13.2, issue #413): X+PLAY or RESTART ▸ AS IT IS while a piece plays queues a restart to the
// next bar line, where the piece starts again from bar 1 with its Intro, from the Current Recipe as the player left
// it. Nothing carries over from the first run.

#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;

/// Events with a tick before `step`'s grid line.
std::vector<Event> before(const std::vector<Event>& events, int32_t step) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.tick < stepTick(step)) {
			out.push_back(e);
		}
	}
	return out;
}

/// The events played after the sink's restart number `n` (0 for the first restart), up to the next restart.
std::vector<Event> afterRestart(const RecordingSink& sink, size_t n = 0) {
	const size_t from = sink.restartedAt.at(n);
	const size_t to = n + 1 < sink.restartedAt.size() ? sink.restartedAt[n + 1] : sink.events.size();
	return {sink.events.begin() + static_cast<std::ptrdiff_t>(from),
	        sink.events.begin() + static_cast<std::ptrdiff_t>(to)};
}

/// The events played before the first restart.
std::vector<Event> firstRun(const RecordingSink& sink) {
	return {sink.events.begin(), sink.events.begin() + static_cast<std::ptrdiff_t>(sink.restartedAt.at(0))};
}

/// The bar line the waiting restart lands on, or -1 if none waits.
int32_t restartLine(const Generator& generator) {
	QueuedAction actions[PendingList::kCapacity];
	const int32_t n = queuedByTime(generator, actions);
	for (int32_t i = 0; i < n; i++) {
		if (actions[i].command.kind == CommandKind::Restart) {
			return actions[i].landsAt;
		}
	}
	return -1;
}

/// A piece that plays `steps` steps, presses X+PLAY, and plays on until the restart's line. @return the line.
template <typename Piece>
int32_t restartAfter(Piece& piece, int32_t steps) {
	const size_t restarts = piece.sink.restartedAt.size();
	piece.decideBefore(steps);
	if (piece.steering.pressRestart() != PressOutcome::Accepted) {
		throw std::runtime_error("restartAfter: the press was not accepted");
	}
	piece.decideBefore(steps + 1);
	const int32_t line = restartLine(piece.generator);
	piece.hook.runUntil([&] { return piece.sink.restartedAt.size() > restarts; });
	return line;
}

/// Whether every note a stream started has ended by its last event: nothing left sounding.
bool everyNoteEnds(const std::vector<Event>& events) {
	std::map<std::tuple<uint8_t, Lane, uint8_t>, int32_t> sounding;
	for (const Event& e : events) {
		if (e.kind == EventKind::NoteOn && !e.muted) {
			sounding[{e.part, e.lane, e.pitch}]++;
		}
		else if (e.kind == EventKind::NoteOff) {
			auto& count = sounding[{e.part, e.lane, e.pitch}];
			count = count > 0 ? count - 1 : 0;
		}
	}
	for (const auto& [note, count] : sounding) {
		if (count > 0) {
			return false;
		}
	}
	return true;
}

} // namespace

// clang-format off
describe restart("Restart", $ {
	it("queues to the next bar line and shows in QUEUE as RESTART", _ {
		StepwisePiece<> piece{{.seed = 3}};
		piece.decideBefore(21);
		expect(piece.steering.pressRestart() == PressOutcome::Accepted).to_be_true();
		expect(piece.steering.restartAhead()).to_be_true();
		piece.decideBefore(22);
		expect(piece.generator.restartWaiting()).to_be_true();
		expect(restartLine(piece.generator)).to_equal(2 * kStepsPerBar);
		char tag[16];
		QueuedAction actions[PendingList::kCapacity];
		statusTag(std::span(actions, queuedByTime(piece.generator, actions)), piece.driver.clock(), tag);
		expect(std::string{tag}).to_equal(std::string{">RST3B"}); // 11 steps to bar 2's line
		char label[24];
		queueLabel({CommandKind::Restart, 0, 1}, genres::technoAcid, label);
		expect(std::string{label}).to_equal(std::string{"RESTART"});
	});

	it("starts the piece again on that bar line, from bar 1 with its Intro", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
		const uint32_t pieces = piece.steering.pieces();
		const int32_t line = restartAfter(piece, 3 * kStepsPerBar + 5);
		expect(line).to_equal(4 * kStepsPerBar);
		// The new piece's step 0 sits on the old one's line: its ticks count from there.
		expect(piece.driver.clock()).to_be_less_than(kTicksPerStep);
		piece.decideBefore(2);
		expect(piece.generator.arc().section() == SectionKind::Intro).to_be_true();
		const std::vector<Event> after = afterRestart(piece.sink);
		expect(after.empty()).to_be_false();
		expect(after.front().tick).to_be_greater_than(stepTick(0) - kTicksPerStep);
		expect(piece.steering.pieces()).to_equal(pieces + 1); // a new piece: the menus read their values again
	});

	it("ends every note of the first run on the line", _ {
		StepwisePiece<> piece{{.seed = 5}};
		const int32_t line = restartAfter(piece, 2 * 64 + 9);
		const std::vector<Event> first = firstRun(piece.sink);
		expect(everyNoteEnds(first)).to_be_true();
		for (const Event& e : first) {
			expect(e.tick).to_be_less_than(stepTick(line) + 1);
		}
	});

	it("replays the first run's stream from bar 1 when nothing was steered", _ {
		for (uint32_t seed : {1U, 3U, 7U}) {
			StepwisePiece<> piece{{.seed = seed}};
			// Long enough for Groove lengths, fills, drop-outs and the Arc's counters to have moved on.
			const int32_t line = restartAfter(piece, 7 * 64 + 13);
			piece.decideBefore(line);
			piece.runToStepLine(line);
			expect(before(afterRestart(piece.sink), line) == before(firstRun(piece.sink), line)).to_be_true();
		}
	});

	it("carries nothing over: a second restart replays the same stream again", _ {
		StepwisePiece<> piece{{.seed = 9}};
		const int32_t line = restartAfter(piece, 3 * 64 + 2);
		const int32_t again = restartAfter(piece, line - 3);
		expect(again).to_equal(line);
		expect(before(afterRestart(piece.sink, 0), line) == before(firstRun(piece.sink), line)).to_be_true();
		piece.decideBefore(line);
		piece.runToStepLine(line);
		expect(before(afterRestart(piece.sink, 1), line) == before(firstRun(piece.sink), line)).to_be_true();
	});

	it("clears mutes, solos, a force, the hold, a waiting End and freezes", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
		piece.decideBefore(17);
		piece.steering.pressMute(kAcid, true);
		piece.steering.pressSolo(kDrums, true);
		piece.steering.pressHold();
		piece.steering.pressFreeze(kDrums);
		piece.decideBefore(19);
		piece.steering.forceSection(SectionKind::Break);
		piece.steering.armEnd();
		piece.decideBefore(20);
		expect(piece.generator.arc().held()).to_be_true();
		restartAfter(piece, 21);
		piece.decideBefore(1);
		expect(piece.generator.arc().held()).to_be_false();
		expect(piece.generator.partFrozen(kDrums)).to_be_false();
		expect(piece.generator.partMuted(kAcid)).to_be_false();
		expect(piece.generator.partSoloed(kDrums)).to_be_false();
		QueuedAction actions[PendingList::kCapacity];
		expect(queuedByTime(piece.generator, actions)).to_equal(0);
	});

	it("after steering, plays as a fresh start from the steered Recipe would", _ {
		StepwisePiece<> piece{{.seed = 4}};
		piece.decideBefore(40);
		piece.steering.setMacro(Macro::Energy, piece.currentRecipe.macros[Macro::Energy] + 10);
		piece.steering.pressTake(kAcid, 1);
		piece.steering.pressOctave(kAcid, 1);
		piece.steering.setBias(kAcid, Macro::Density, -12); // kept, as a Regenerate keeps it (keepsPartBias())
		piece.decideBefore(2 * 64 + 1); // the take lands on the phrase line, the shift on its bar line
		expect(piece.currentTakes.take[kAcid]).to_equal(1);
		const int32_t line = restartAfter(piece, 2 * 64 + 20);
		piece.decideBefore(line);
		piece.runToStepLine(line);

		StepwisePiece<> steered{{.recipe = piece.currentRecipe, .seed = piece.currentSeed, .takes = piece.currentTakes}};
		steered.decideBefore(line);
		steered.runToStepLine(line);
		expect(piece.generator.recipe().bias.of(kAcid, Macro::Density)).to_equal(-12);
		expect(before(afterRestart(piece.sink), line) == before(steered.sink.events, line)).to_be_true();
	});

	it("restarts on the line from a late call, the new piece on time from there", _ {
		StepwisePiece<> onTime{{.seed = 6}};
		const int32_t line = restartAfter(onTime, 2 * kStepsPerBar + 3);
		onTime.decideBefore(kStepsPerBar);
		onTime.runToStepLine(kStepsPerBar);

		StepwisePiece<> late{{.seed = 6}};
		late.decideBefore(2 * kStepsPerBar + 3);
		late.steering.pressRestart();
		late.runToStepLine(line, -1);
		late.hook.callAt(late.hook.now() + 5); // 5 song ticks past the line
		expect(late.sink.restartedAt.size()).to_equal(1U);
		late.decideBefore(kStepsPerBar);
		late.runToStepLine(kStepsPerBar);
		expect(before(afterRestart(late.sink), kStepsPerBar) == before(afterRestart(onTime.sink), kStepsPerBar))
		    .to_be_true();
	});

	it("keeps the master's bar lines under another clock magnitude", _ {
		StepwisePiece<> piece{{.seed = 2, .magnitude = 3}};
		const int32_t line = restartAfter(piece, kStepsPerBar + 7);
		expect(line).to_equal(2 * kStepsPerBar);
		// The master is on the line: a bar of song ticks per bar of steps, from the first call.
		expect(piece.hook.now() % songTicksFor(kStepsPerBar * kTicksPerStep, 3)).to_equal(0);
		expect(piece.driver.clock()).to_equal(0);
	});

	it("is cancelled by BACK while it waits, and by a second press", _ {
		StepwisePiece<> piece{{.seed = 3}};
		piece.decideBefore(21);
		piece.steering.pressRestart();
		piece.decideBefore(22);
		piece.steering.cancelNewest();
		piece.decideBefore(23);
		expect(piece.generator.restartWaiting()).to_be_false();
		expect(piece.steering.restartAhead()).to_be_false();
		piece.steering.pressRestart();
		expect(piece.steering.pressRestart() == PressOutcome::Accepted).to_be_true(); // the second cancels it
		expect(piece.steering.restartAhead()).to_be_false();
		piece.decideBefore(3 * kStepsPerBar + 1);
		expect(piece.sink.restartedAt.empty()).to_be_true();
	});

	it("is refused while a transition is pending, and does nothing while stopped", _ {
		StepwisePiece<> piece{{.seed = 3}};
		bool pending = true;
		piece.steering.pending = [&] { return pending; };
		piece.decideBefore(5);
		expect(piece.steering.pressRestart() == PressOutcome::ChangePending).to_be_true();
		expect(piece.steering.restartAhead()).to_be_false();
		pending = false;
		piece.driver.stop(piece.sink);
		expect(piece.steering.pressRestart() == PressOutcome::NothingToDo).to_be_true();
	});
});

CPPSPEC_SPEC(restart)
