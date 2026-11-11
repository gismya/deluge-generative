// A transition's lifecycle (spec §12.9, §12.10): the phase and its rules in perform/transition_lifecycle.h, against the
// real core and Perform driver. The host is the harness's (transition_piece.h); the firmware's is
// generative/firmware/transitions.cpp, which the delugemu scripts check as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace harness;

namespace {

using Phase = perform::TransitionPhase;
using Request = perform::TransitionRequest;
using Retired = TransitionHost::Retired;
using Events = std::vector<std::string>;

/// The playing core takes what is queued: one more step is decided.
void decideOne(TransitionPiece& t) {
	t.decideBefore(t.driver.playing()->nextStep() + 1);
}

/// Fills the steering queue with presses that undo each other.
void fillQueue(TransitionPiece& t) {
	while (t.push({CommandKind::SetHold, 0, kPress})) {}
}

/// Plays until the transition's command has landed.
void land(TransitionPiece& t) {
	t.hook.runUntil([&] { return t.out->transitionStart() >= 0 || !t.driver.running(); });
}

/// Plays until the new roster has started and the outgoing instance has drained.
void startAndDrain(TransitionPiece& t) {
	t.hook.runUntil([&] { return (t.sink.starts == 1 && t.sink.drained == 1) || !t.driver.running(); });
}

/// End is armed, and the piece plays up to the last decision of its Outro. @return the end line's step.
int32_t playToOutroEnd(TransitionPiece& t) {
	t.push({CommandKind::End, 0, 1});
	t.hook.runUntil([&] { return t.out->ended() || !t.driver.running(); });
	return t.out->nextStep();
}

} // namespace

// clang-format off
describe transition_lifecycle("Transition lifecycle", $ {
	const int32_t phraseSteps = genres::techno.phraseSteps();

	context("a request", _ {
		it("preloads while the roster plays on, then queues the incoming instance and its command", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(t.change.request(TransitionStyle::Drop) == Request::Started).to_be_true();
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			expect(t.change.pending()).to_be_true();
			expect(t.change.preloading()).to_be_true();
			expect(t.change.holdsStart()).to_be_false();
			expect(t.change.preloads == std::vector<uint32_t>{1}).to_be_true();
			expect(t.change.soundDrops).to_equal(1);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.change.load()).to_equal(2);
			expect(t.change.phase() == Phase::Queued).to_be_true();
			expect(t.change.incomingRoster).to_equal(2);
			expect(t.driver.incoming() != nullptr).to_be_true();
			expect(t.change.events == Events{"phase=preloading", "request style=0 started", "phase=queued"})
			    .to_be_true();
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_true();
			t.pump();
			expect(t.change.phase() == Phase::Queued).to_be_true();
		});

		it("drops the takes still queued at the confirm", _ {
			TransitionPiece t;
			t.decideBefore(3);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			expect(t.out->takeWaiting(0)).to_equal(2);
			t.change.request(TransitionStyle::Drop);
			decideOne(t);
			expect(t.out->takeWaiting(0)).to_be_less_than(0);
		});

		it("replaces a transition that is Preloading", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.change.request(TransitionStyle::Drop);
			expect(t.change.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.change.preloads == (std::vector<uint32_t>{1, 2})).to_be_true();
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			t.change.load();
			expect(t.change.phase() == Phase::Queued).to_be_true();
			decideOne(t);
			const Command* waiting = t.out->waitingTransition();
			expect(waiting != nullptr).to_be_true();
			expect(waiting != nullptr && static_cast<TransitionStyle>(waiting->target) == TransitionStyle::Cut)
			    .to_be_true();
		});

		it("replaces a transition that is Queued: its command is cancelled and its roster released", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_true();
			expect(t.change.request(TransitionStyle::Fade) == Request::Started).to_be_true();
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			expect(t.change.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.change.incomingRoster).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.change.has("cancel replaced")).to_be_true();
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_false();
			expect(t.change.load()).to_equal(3);
			expect(t.change.phase() == Phase::Queued).to_be_true();
			decideOne(t);
			const Command* waiting = t.out->waitingTransition();
			expect(waiting != nullptr && static_cast<TransitionStyle>(waiting->target) == TransitionStyle::Fade)
			    .to_be_true();
			t.pump();
			expect(t.change.phase() == Phase::Queued).to_be_true(); // the first one's cancel is not a drop
		});

		it("is refused while one is Committed, and while one is Retiring", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			land(t);
			expect(t.change.phase() == Phase::Committed).to_be_true();
			expect(t.change.pending()).to_be_true();
			expect(t.change.request(TransitionStyle::Drop) == Request::Refused).to_be_true();
			expect(t.change.has("request style=0 refused phase=committed")).to_be_true();
			startAndDrain(t);
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.pending()).to_be_false();
			expect(t.change.request(TransitionStyle::Cut) == Request::Refused).to_be_true();
			expect(t.change.has("request style=2 refused phase=retiring")).to_be_true();
			expect(t.change.preloads.size()).to_equal(size_t{1});
			expect(t.change.soundDrops).to_equal(1);
		});

		it("is refused with QUEUE FULL when the Queued one's cancel has no room, and that one plays on", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			fillQueue(t);
			expect(t.change.request(TransitionStyle::Cut) == Request::QueueFull).to_be_true();
			expect(t.change.queueFullPopups).to_equal(1);
			expect(t.change.has("cancel replaced refused")).to_be_true();
			expect(t.change.phase() == Phase::Queued).to_be_true();
			expect(t.change.incomingRoster).to_equal(2);
			expect(t.change.retired.empty()).to_be_true();
			expect(t.change.preloads.size()).to_equal(size_t{1});
			expect(t.driver.incoming() != nullptr).to_be_true();
		});

		it("shows QUEUE FULL and ends when the loaded roster's command has no room", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.change.request(TransitionStyle::Drop);
			fillQueue(t);
			t.change.load();
			expect(t.change.queueFullPopups).to_equal(1);
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.driver.incoming() == nullptr).to_be_true();
		});

		it("is ignored outside Perform, while leaving, and when its preload can't start", _ {
			TransitionPiece t;
			t.change.entered = false;
			expect(t.change.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			t.change.entered = true;
			t.change.left = true;
			expect(t.change.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			t.change.left = false;
			t.change.preloadStarts = false;
			expect(t.change.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.events.empty()).to_be_true();
		});

		it("changes nothing when a job ends for an older request", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.change.request(TransitionStyle::Drop);
			t.change.request(TransitionStyle::Cut);
			const Events before = t.change.events;
			t.change.failLoad(1);
			expect(t.change.load(1)).to_equal(2);
			expect(t.change.discarded == std::vector<int32_t>{2}).to_be_true(); // the caller frees what it loaded
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			expect(t.change.incomingRoster).to_equal(0);
			expect(t.change.endedPreloads).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.change.events == before).to_be_true();
		});
	});

	context("CANCEL CHANGE", _ {
		it("cancels a transition that is Preloading, and its job's late result changes nothing", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.change.request(TransitionStyle::Drop);
			expect(t.change.cancellable()).to_be_true();
			expect(t.change.cancel()).to_be_true();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.cancelledPreloads).to_equal(1);
			const Events cancelled{"phase=preloading", "request style=0 started", "cancel preload", "phase=idle"};
			expect(t.change.events == cancelled).to_be_true();
			t.change.failLoad(1);
			t.change.load(1);
			expect(t.change.events == cancelled).to_be_true();
			expect(t.change.discarded == std::vector<int32_t>{2}).to_be_true();
			expect(t.change.endedPreloads).to_equal(0);
			expect(t.change.cancel()).to_be_false(); // none to cancel
		});

		it("cancels a transition that is Queued: the playing roster plays on", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			expect(t.change.cancellable()).to_be_true();
			expect(t.change.cancel()).to_be_true();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.change.has("cancel queued")).to_be_true();
			t.runBars(3 * phraseSteps / kStepsPerBar);
			expect(t.sink.starts).to_equal(0);
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.playingRoster).to_equal(1);
		});

		it("is refused with QUEUE FULL when the cancel has no room", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			fillQueue(t);
			expect(t.change.cancel()).to_be_false();
			expect(t.change.queueFullPopups).to_equal(1);
			expect(t.change.has("cancel queued refused")).to_be_true();
			expect(t.change.phase() == Phase::Queued).to_be_true();
			expect(t.change.retired.empty()).to_be_true();
		});

		it("is refused once the command has landed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			land(t);
			expect(t.change.phase() == Phase::Committed).to_be_true();
			expect(t.change.events.back()).to_equal(std::string("phase=committed"));
			expect(t.change.cancellable()).to_be_false();
			expect(t.change.cancel()).to_be_false();
			expect(t.change.retired.empty()).to_be_true();
			startAndDrain(t);
			expect(t.sink.starts).to_equal(1);
			expect(t.change.cancel()).to_be_false(); // Retiring
		});
	});

	context("a BACK tap that drops the queued command (spec §13.5)", _ {
		it("ends the transition at the next pump and releases the incoming roster", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			expect(t.out->transitionWaiting()).to_be_false();
			// The tick only flags it: the roster is released in task context.
			expect(t.change.phase() == Phase::Queued).to_be_true();
			expect(t.change.retired.empty()).to_be_true();
			t.pump();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.change.incomingRoster).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.change.has("cancel dropped")).to_be_true();
			t.runBars(3 * phraseSteps / kStepsPerBar);
			expect(t.sink.starts).to_equal(0);
		});

		it("ends it when the tap reaches the same decision as the command", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			t.pump();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.has("cancel dropped")).to_be_true();
		});

		it("leaves a transition alone whose command still waits in the core", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			for (int32_t i = 0; i < 8; i++) {
				decideOne(t);
				t.pump();
			}
			expect(t.change.phase() == Phase::Queued).to_be_true();
		});
	});

	context("a stop commits in any phase", _ {
		it("commits a Preloading one when it has loaded", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.change.request(TransitionStyle::Drop);
			t.stop();
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			expect(t.change.holdsStart()).to_be_true(); // no piece starts on the old roster meanwhile
			expect(t.change.events.back()).to_equal(std::string("commit_on_load"));
			expect(t.change.playingRoster).to_equal(1);
			t.change.load();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.holdsStart()).to_be_false();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.currentRecipe).to_equal(2);
			expect(t.change.retired == std::vector<Retired>{{1, true, t.change.phrase}}).to_be_true();
			expect(t.change.starts()).to_equal(1U);
			expect(t.change.plays.empty()).to_be_true();
			expect(t.driver.incoming() == nullptr).to_be_true(); // no instance was queued
		});

		it("commits a Queued one at once", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			t.stop();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.currentRecipe).to_equal(2);
			expect(t.change.retired == std::vector<Retired>{{1, true, t.change.phrase}}).to_be_true();
			expect(t.change.has("start stopped")).to_be_true();
			expect(t.change.starts()).to_equal(1U);
			expect(t.sink.starts).to_equal(0); // not through the start step
		});

		it("commits a Committed one at once", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			land(t);
			t.stop();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.retired == std::vector<Retired>{{1, true, t.change.phrase}}).to_be_true();
		});

		it("is over once the roster that played is freed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			t.stop();
			t.pump();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			t.change.freeRetired();
			t.pump();
			expect(t.change.phase() == Phase::Idle).to_be_true();
		});

		it("drains the outgoing roster of one that is Retiring, with what is left of its cap", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			expect(t.driver.outgoing() != nullptr).to_be_true();
			t.stop();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.retired == std::vector<Retired>{{1, true, t.change.phrase}}).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
		});
	});

	context("confirmed while stopped (spec §12.10)", _ {
		it("commits when loaded, and holds a start until then", _ {
			TransitionPiece t;
			t.stop();
			expect(t.change.request(TransitionStyle::Drop) == Request::Started).to_be_true();
			expect(t.change.holdsStart()).to_be_true();
			t.change.load();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.plays.empty()).to_be_true();
			expect(t.change.holdsStart()).to_be_false();
		});

		it("takes PLAY to wait for the load, then starts the new roster", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay(true);
			expect(t.change.plays.empty()).to_be_true();
			expect(t.change.transport).to_be_false();
			t.change.load();
			expect(t.change.plays == std::vector<bool>{true}).to_be_true(); // in Groove, as it was pressed
			expect(t.change.playingRoster).to_equal(2);
			const Events all{"phase=preloading", "request style=0 started", "start_when_loaded",
			                 "start stopped",    "phase=retiring",          "start_when_loaded playing"};
			expect(t.change.events == all).to_be_true();
		});

		it("does not take PLAY when no transition holds the start", _ {
			TransitionPiece t;
			t.stop();
			expect(t.change.startWhenLoaded(false)).to_be_false();
			t.restart();
			t.change.transport = true;
			t.change.request(TransitionStyle::Drop); // playing: it doesn't commit on load
			expect(t.change.startWhenLoaded(false)).to_be_false();
		});

		it("cancels the wait when PLAY is pressed again", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay();
			t.pressPlay();
			expect(t.change.events.back()).to_equal(std::string("start_when_loaded off"));
			t.change.load();
			expect(t.change.plays.empty()).to_be_true();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
		});

		it("drops the waiting PLAY at a stop", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay();
			t.change.transport = true; // a MIDI Start: playback runs, and the tick holds the piece
			t.stop();
			t.change.load();
			expect(t.change.plays.empty()).to_be_true();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
		});

		it("drops the waiting PLAY when Perform is left", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay();
			t.change.left = true;
			t.change.failLoad(); // the leave cancels the job
			expect(t.change.plays.empty()).to_be_true();
			expect(t.change.endedPreloads).to_equal(1);
			expect(t.change.phase() == Phase::Idle).to_be_true();
		});

		it("starts nothing itself when a MIDI Start began playback while PLAY waited", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay();
			t.change.transport = true;
			t.change.load();
			expect(t.change.plays.empty()).to_be_true(); // PLAY now would stop it: the tick starts the piece
			expect(t.change.events.back()).to_equal(std::string("start_when_loaded already_playing"));
			expect(t.change.holdsStart()).to_be_false();
		});

		it("starts the roster that plays when the preload is cancelled or fails under a waiting PLAY", _ {
			TransitionPiece cancelled;
			cancelled.stop();
			cancelled.change.request(TransitionStyle::Drop);
			cancelled.pressPlay();
			expect(cancelled.change.cancel()).to_be_true();
			expect(cancelled.change.plays == std::vector<bool>{false}).to_be_true();
			expect(cancelled.change.playingRoster).to_equal(1);

			TransitionPiece failed;
			failed.stop();
			failed.change.request(TransitionStyle::Drop);
			failed.pressPlay();
			failed.change.failLoad();
			expect(failed.change.plays == std::vector<bool>{false}).to_be_true();
			expect(failed.change.phase() == Phase::Idle).to_be_true();
			expect(failed.change.endedPreloads).to_equal(1);
		});

		it("keeps a waiting PLAY for the request that replaces the one it waited for", _ {
			TransitionPiece t;
			t.stop();
			t.change.request(TransitionStyle::Drop);
			t.pressPlay();
			t.change.request(TransitionStyle::Cut);
			t.change.load(1);
			expect(t.change.plays.empty()).to_be_true();
			t.change.load();
			expect(t.change.plays == std::vector<bool>{false}).to_be_true();
		});
	});

	context("the start step and the outgoing roster", _ {
		it("swaps the rosters in the tick and frees nothing there", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.outgoingRoster).to_equal(1);
			expect(t.change.currentRecipe).to_equal(2);
			expect(t.change.starts()).to_equal(1U);
			expect(t.change.retired.empty()).to_be_true();
			expect(t.driver.playing() == &t.change.current()).to_be_true();
			// A cut starts on the line it lands on: it is never Committed.
			expect(t.change.has("phase=committed")).to_be_false();
		});

		it("counts the outgoing roster's one-phrase cap from the start step, not from the drain", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const int64_t startedAt = t.hook.lastCall();
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const int64_t drainedAt = t.hook.lastCall();
			expect(drainedAt).to_be_greater_than(startedAt);
			const uint32_t cap = t.change.phrase - static_cast<uint32_t>(drainedAt - startedAt);
			expect(t.change.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
			expect(t.change.outgoingRoster).to_equal(0);
		});

		it("leaves no cap when the drain took a phrase or more", _ {
			expect(perform::capAfterDrain(1000, 0)).to_equal(1000U);
			expect(perform::capAfterDrain(1000, 400)).to_equal(600U);
			expect(perform::capAfterDrain(1000, 1000)).to_equal(0U);
			expect(perform::capAfterDrain(1000, 5000)).to_equal(0U);
		});

		it("stays Retiring until the outgoing instance has drained and its roster is freed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			t.pump();
			expect(t.change.phase() == Phase::Retiring).to_be_true(); // still draining
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			t.pump();
			expect(t.change.phase() == Phase::Retiring).to_be_true(); // drained, not freed
			t.change.freeRetired();
			t.pump();
			expect(t.change.phase() == Phase::Idle).to_be_true();
			expect(t.change.request(TransitionStyle::Drop) == Request::Started).to_be_true();
		});

		it("follows the incoming instance with the fixture's step calls after the start step", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			expect(t.decideBefore(4 * phraseSteps)).to_be_false(); // the incoming instance took over first
			expect(t.sink.starts).to_equal(1);
			expect(t.decideBefore(5)).to_be_true();
			expect(t.change.current().nextStep()).to_equal(5);
		});
	});

	// Ticket #195, step 1: can a piece end by itself while a transition is Queued or Committed? The adapter then
	// stops the transport from inside the tick, and the stop's commit runs there.
	context("a piece that ends by itself, with End armed before the confirm", _ {
		it("never ends while the transition's command waits in the core", _ {
			for (const TransitionStyle style : {TransitionStyle::Drop, TransitionStyle::Fade, TransitionStyle::Cut}) {
				TransitionPiece t;
				const int32_t endLine = playToOutroEnd(t);
				// In time for the end line's decision, which takes the queue's steering before it ends the piece.
				t.runToStepLine(endLine - 1, -1);
				t.confirm(style);
				t.runToStepLine(endLine + 1);
				expect(t.driver.ended()).to_be_false();
				expect(t.driver.running()).to_be_true();
				startAndDrain(t);
				expect(t.sink.starts).to_equal(1);
				expect(t.driver.ended()).to_be_false();
				expect(t.change.phase() == Phase::Retiring).to_be_true();
			}
		});

		it("never ends once the command has landed", _ {
			TransitionPiece t;
			t.push({CommandKind::End, 0, 1});
			t.confirmAt(3, TransitionStyle::Fade); // End and the transition wait for the same phrase line
			land(t);
			expect(t.change.phase() == Phase::Committed).to_be_true();
			startAndDrain(t);
			expect(t.sink.starts).to_equal(1);
			expect(t.driver.ended()).to_be_false();
		});

		it("ends while Queued when the command is still in the queue after the end line took its steering", _ {
			// The one way in: the preload ends in the 12 ticks between the end line's decision tick and the line.
			TransitionPiece t;
			const int32_t endLine = playToOutroEnd(t);
			t.runToStepLine(endLine, -6);
			t.confirm(TransitionStyle::Drop);
			expect(t.change.phase() == Phase::Queued).to_be_true();
			t.runToStepLine(endLine);
			expect(t.driver.ended()).to_be_true();
			expect(t.change.phase() == Phase::Queued).to_be_true();
			// The adapter's tick stops the transport, and the stop commits: here, inside the tick.
			expect(t.stopIfEnded()).to_be_true();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
			expect(t.change.currentRecipe).to_equal(2);
			expect(t.change.retired == std::vector<Retired>{{1, true, t.change.phrase}}).to_be_true();
			expect(t.change.events.back()).to_equal(std::string("phase=retiring"));
		});

		it("ends while Preloading, and commits when loaded", _ {
			TransitionPiece t;
			const int32_t endLine = playToOutroEnd(t);
			t.change.request(TransitionStyle::Drop);
			t.runToStepLine(endLine);
			expect(t.stopIfEnded()).to_be_true();
			expect(t.change.phase() == Phase::Preloading).to_be_true();
			expect(t.change.holdsStart()).to_be_true();
			t.change.load();
			expect(t.change.phase() == Phase::Retiring).to_be_true();
			expect(t.change.playingRoster).to_equal(2);
		});
	});
});

CPPSPEC_SPEC(transition_lifecycle)
