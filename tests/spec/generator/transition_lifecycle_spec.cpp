// A transition's lifecycle (spec §12.9, §12.10): the phase and its rules in perform/transition_lifecycle.h, against the
// real core and Perform driver. The adapter is the harness's (transition_piece.h); the firmware's is
// generative/firmware/transitions.cpp, which the delugemu scripts check as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace harness;

namespace {

using Phase = perform::TransitionPhase;
using Request = perform::TransitionRequest;
using Retired = TransitionAdapter::Retired;
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
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Started).to_be_true();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.pending()).to_be_true();
			expect(t.lifecycle.preloading()).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_false();
			expect(t.lifecycle.preloads == std::vector<uint32_t>{1}).to_be_true();
			expect(t.lifecycle.soundDrops).to_equal(1);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.lifecycle.load()).to_equal(2);
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(2);
			expect(t.driver.incoming() != nullptr).to_be_true();
			expect(t.lifecycle.events == Events{"phase=preloading", "request style=0 started", "phase=queued"})
			    .to_be_true();
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_true();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
		});

		it("drops the takes still queued at the confirm", _ {
			TransitionPiece t;
			t.decideBefore(3);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			expect(t.out->takeWaiting(0)).to_equal(2);
			t.lifecycle.request(TransitionStyle::Drop);
			decideOne(t);
			expect(t.out->takeWaiting(0)).to_be_less_than(0);
		});

		it("replaces a transition that is Preloading", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::Drop);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.lifecycle.preloads == (std::vector<uint32_t>{1, 2})).to_be_true();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
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
			expect(t.lifecycle.request(TransitionStyle::Fade) == Request::Started).to_be_true();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.lifecycle.has("cancel replaced")).to_be_true();
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_false();
			expect(t.lifecycle.load()).to_equal(3);
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			decideOne(t);
			const Command* waiting = t.out->waitingTransition();
			expect(waiting != nullptr && static_cast<TransitionStyle>(waiting->target) == TransitionStyle::Fade)
			    .to_be_true();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true(); // the first one's cancel is not a drop
		});

		it("is refused while one is Committed, and while one is Retiring", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
			expect(t.lifecycle.pending()).to_be_true();
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Refused).to_be_true();
			expect(t.lifecycle.has("request style=0 refused phase=committed")).to_be_true();
			startAndDrain(t);
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.pending()).to_be_false();
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Refused).to_be_true();
			expect(t.lifecycle.has("request style=2 refused phase=retiring")).to_be_true();
			expect(t.lifecycle.preloads.size()).to_equal(size_t{1});
			expect(t.lifecycle.soundDrops).to_equal(1);
		});

		it("is refused with QUEUE FULL when the Queued one's cancel has no room, and that one plays on", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			fillQueue(t);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::QueueFull).to_be_true();
			expect(t.lifecycle.queueFullPopups).to_equal(1);
			expect(t.lifecycle.has("cancel replaced refused")).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(2);
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.preloads.size()).to_equal(size_t{1});
			expect(t.driver.incoming() != nullptr).to_be_true();
		});

		it("shows QUEUE FULL and ends when the loaded roster's command has no room", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::Drop);
			fillQueue(t);
			t.lifecycle.load();
			expect(t.lifecycle.queueFullPopups).to_equal(1);
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.driver.incoming() == nullptr).to_be_true();
		});

		it("is ignored outside Perform, while leaving, and when its preload can't start", _ {
			TransitionPiece t;
			t.lifecycle.entered = false;
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			t.lifecycle.entered = true;
			t.lifecycle.left = true;
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			t.lifecycle.left = false;
			t.lifecycle.preloadStarts = false;
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Ignored).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.events.empty()).to_be_true();
		});

		it("changes nothing when a job ends for an older request", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::Drop);
			t.lifecycle.request(TransitionStyle::Cut);
			const Events before = t.lifecycle.events;
			t.lifecycle.failLoad(1);
			expect(t.lifecycle.load(1)).to_equal(2);
			expect(t.lifecycle.discarded == std::vector<int32_t>{2}).to_be_true(); // the caller frees what it loaded
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(0);
			expect(t.lifecycle.endedPreloads).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.lifecycle.events == before).to_be_true();
		});
	});

	context("CANCEL CHANGE", _ {
		it("cancels a transition that is Preloading, and its job's late result changes nothing", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::Drop);
			expect(t.lifecycle.cancellable()).to_be_true();
			expect(t.lifecycle.cancel()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.cancelledPreloads).to_equal(1);
			const Events cancelled{"phase=preloading", "request style=0 started", "cancel preload", "phase=idle"};
			expect(t.lifecycle.events == cancelled).to_be_true();
			t.lifecycle.failLoad(1);
			t.lifecycle.load(1);
			expect(t.lifecycle.events == cancelled).to_be_true();
			expect(t.lifecycle.discarded == std::vector<int32_t>{2}).to_be_true();
			expect(t.lifecycle.endedPreloads).to_equal(0);
			expect(t.lifecycle.cancel()).to_be_false(); // none to cancel
		});

		it("cancels a transition that is Queued: the playing roster plays on", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			expect(t.lifecycle.cancellable()).to_be_true();
			expect(t.lifecycle.cancel()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.lifecycle.has("cancel queued")).to_be_true();
			t.runBars(3 * phraseSteps / kStepsPerBar);
			expect(t.sink.starts).to_equal(0);
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(1);
		});

		it("is refused with QUEUE FULL when the cancel has no room", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			fillQueue(t);
			expect(t.lifecycle.cancel()).to_be_false();
			expect(t.lifecycle.queueFullPopups).to_equal(1);
			expect(t.lifecycle.has("cancel queued refused")).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
		});

		it("is refused once the command has landed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
			expect(t.lifecycle.events.back()).to_equal(std::string("phase=committed"));
			expect(t.lifecycle.cancellable()).to_be_false();
			expect(t.lifecycle.cancel()).to_be_false();
			expect(t.lifecycle.retired.empty()).to_be_true();
			startAndDrain(t);
			expect(t.sink.starts).to_equal(1);
			expect(t.lifecycle.cancel()).to_be_false(); // Retiring
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
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(0);
			expect(t.driver.incoming() == nullptr).to_be_true();
			expect(t.lifecycle.has("cancel dropped")).to_be_true();
			t.runBars(3 * phraseSteps / kStepsPerBar);
			expect(t.sink.starts).to_equal(0);
		});

		it("ends it when the tap reaches the same decision as the command", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			t.pump();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.has("cancel dropped")).to_be_true();
		});

		// Today's behaviour, which issue #206 will flip: the stop finds the phase still Queued, since only the pump
		// ends a dropped transition, so it commits the transition the player cancelled.
		it("commits at a stop that comes before the next pump", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			expect(t.out->transitionWaiting()).to_be_false();
			t.stop();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.currentRecipe).to_equal(2);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
			expect(t.lifecycle.has("start stopped")).to_be_true();
			t.pump();
			expect(t.lifecycle.has("cancel dropped")).to_be_false();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
		});

		it("is not followed once Perform is being left: the roster goes with Perform's song", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			t.lifecycle.left = true;
			t.pump();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.has("cancel dropped")).to_be_false();
		});

		it("leaves a transition alone whose command still waits in the core", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			for (int32_t i = 0; i < 8; i++) {
				decideOne(t);
				t.pump();
			}
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
		});
	});

	context("a stop commits in any phase", _ {
		it("commits a Preloading one when it has loaded", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::Drop);
			t.stop();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_true(); // no piece starts on the old roster meanwhile
			expect(t.lifecycle.events.back()).to_equal(std::string("commit_on_load"));
			expect(t.lifecycle.playingRoster).to_equal(1);
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_false();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.currentRecipe).to_equal(2);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
			expect(t.lifecycle.starts()).to_equal(1U);
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.driver.incoming() == nullptr).to_be_true(); // no instance was queued
		});

		it("commits a Queued one at once", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			decideOne(t);
			t.stop();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.currentRecipe).to_equal(2);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
			expect(t.lifecycle.has("start stopped")).to_be_true();
			expect(t.lifecycle.starts()).to_equal(1U);
			expect(t.sink.starts).to_equal(0); // not through the start step
		});

		it("commits a Committed one at once", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			land(t);
			t.stop();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
		});

		it("is over once the roster that played is freed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			t.stop();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			t.lifecycle.freeRetired();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
		});

		it("drains the outgoing roster of one that is Retiring, with what is left of its cap", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			expect(t.driver.outgoing() != nullptr).to_be_true();
			t.stop();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
		});
	});

	context("confirmed while stopped (spec §12.10)", _ {
		it("commits when loaded, and holds a start until then", _ {
			TransitionPiece t;
			t.stop();
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Started).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_true();
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_false();
		});

		it("takes PLAY to wait for the load, then starts the new roster", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay(true);
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.transport).to_be_false();
			t.lifecycle.load();
			expect(t.lifecycle.plays == std::vector<bool>{true}).to_be_true(); // in Groove, as it was pressed
			expect(t.lifecycle.playingRoster).to_equal(2);
			const Events all{"phase=preloading", "request style=0 started", "start_when_loaded",
			                 "start stopped",    "phase=retiring",          "start_when_loaded playing"};
			expect(t.lifecycle.events == all).to_be_true();
		});

		it("does not take PLAY when no transition holds the start", _ {
			TransitionPiece t;
			t.stop();
			expect(t.lifecycle.startWhenLoaded(false)).to_be_false();
			t.restart();
			t.lifecycle.transport = true;
			t.lifecycle.request(TransitionStyle::Drop); // playing: it doesn't commit on load
			expect(t.lifecycle.startWhenLoaded(false)).to_be_false();
		});

		it("cancels the wait when PLAY is pressed again", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.pressPlay();
			expect(t.lifecycle.events.back()).to_equal(std::string("start_when_loaded off"));
			t.lifecycle.load();
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
		});

		it("drops the waiting PLAY at a stop", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.lifecycle.transport = true; // a MIDI Start: playback runs, and the tick holds the piece
			t.stop();
			t.lifecycle.load();
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
		});

		it("drops the waiting PLAY when Perform is left", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.lifecycle.left = true;
			t.lifecycle.failLoad(); // the leave cancels the job
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.endedPreloads).to_equal(1);
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
		});

		it("forgets the waiting PLAY at reset(), and the preload still ends as the newest", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			const Events before = t.lifecycle.events;
			t.lifecycle.reset(); // Perform's song is going, with the preload still running
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_false();
			expect(t.lifecycle.pending()).to_be_false();
			t.lifecycle.failLoad(); // the leave cancelled the job: the adapter hears of its end
			expect(t.lifecycle.endedPreloads).to_equal(1);
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.events == before).to_be_true();
		});

		it("changes nothing when the preload ends well while Perform is being left", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.lifecycle.left = true;
			const Events before = t.lifecycle.events;
			t.lifecycle.load();
			expect(t.lifecycle.discarded.empty()).to_be_true(); // it goes with Perform's song, not to the free job
			expect(t.lifecycle.incomingRoster).to_equal(0);
			expect(t.lifecycle.playingRoster).to_equal(1);
			expect(t.lifecycle.plays.empty()).to_be_true();
			expect(t.lifecycle.events == before).to_be_true();
		});

		it("starts nothing itself when a MIDI Start began playback while PLAY waited", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.lifecycle.transport = true;
			t.lifecycle.load();
			expect(t.lifecycle.plays.empty()).to_be_true(); // PLAY now would stop it: the tick starts the piece
			expect(t.lifecycle.events.back()).to_equal(std::string("start_when_loaded already_playing"));
			expect(t.lifecycle.holdsStart()).to_be_false();
		});

		it("starts the roster that plays when the preload is cancelled or fails under a waiting PLAY", _ {
			TransitionPiece cancelled;
			cancelled.stop();
			cancelled.lifecycle.request(TransitionStyle::Drop);
			cancelled.pressPlay();
			expect(cancelled.lifecycle.cancel()).to_be_true();
			expect(cancelled.lifecycle.plays == std::vector<bool>{false}).to_be_true();
			expect(cancelled.lifecycle.playingRoster).to_equal(1);

			TransitionPiece failed;
			failed.stop();
			failed.lifecycle.request(TransitionStyle::Drop);
			failed.pressPlay();
			failed.lifecycle.failLoad();
			expect(failed.lifecycle.plays == std::vector<bool>{false}).to_be_true();
			expect(failed.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(failed.lifecycle.endedPreloads).to_equal(1);
		});

		it("keeps a waiting PLAY for the request that replaces the one it waited for", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::Drop);
			t.pressPlay();
			t.lifecycle.request(TransitionStyle::Cut);
			t.lifecycle.load(1);
			expect(t.lifecycle.plays.empty()).to_be_true();
			t.lifecycle.load();
			expect(t.lifecycle.plays == std::vector<bool>{false}).to_be_true();
		});
	});

	context("the start step and the outgoing roster", _ {
		it("swaps the rosters in the tick and frees nothing there", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.outgoingRoster).to_equal(1);
			expect(t.lifecycle.currentRecipe).to_equal(2);
			expect(t.lifecycle.starts()).to_equal(1U);
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.driver.playing() == &t.lifecycle.current()).to_be_true();
			// A cut starts on the line it lands on: it is never Committed.
			expect(t.lifecycle.has("phase=committed")).to_be_false();
		});

		it("counts the outgoing roster's one-phrase cap from the start step, not from the drain", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const int64_t startedAt = t.hook.lastCall();
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const int64_t drainedAt = t.hook.lastCall();
			expect(drainedAt).to_be_greater_than(startedAt);
			const uint32_t cap = t.lifecycle.phrase - static_cast<uint32_t>(drainedAt - startedAt);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
			expect(t.lifecycle.outgoingRoster).to_equal(0);
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
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true(); // still draining
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			t.pump();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true(); // drained, not freed
			t.lifecycle.freeRetired();
			t.pump();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.request(TransitionStyle::Drop) == Request::Started).to_be_true();
		});

		it("follows the incoming instance with the fixture's step calls after the start step", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut);
			expect(t.decideBefore(4 * phraseSteps)).to_be_false(); // the incoming instance took over first
			expect(t.sink.starts).to_equal(1);
			expect(t.decideBefore(5)).to_be_true();
			expect(t.lifecycle.current().nextStep()).to_equal(5);
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
				expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			}
		});

		it("never ends once the command has landed", _ {
			TransitionPiece t;
			t.push({CommandKind::End, 0, 1});
			t.confirmAt(3, TransitionStyle::Fade); // End and the transition wait for the same phrase line
			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
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
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			t.runToStepLine(endLine);
			expect(t.driver.ended()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			// The adapter's tick stops the transport, and the stop commits: here, inside the tick.
			expect(t.stopIfEnded()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			expect(t.lifecycle.currentRecipe).to_equal(2);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase}}).to_be_true();
			expect(t.lifecycle.events.back()).to_equal(std::string("phase=retiring"));
		});

		it("ends while Preloading, and commits when loaded", _ {
			TransitionPiece t;
			const int32_t endLine = playToOutroEnd(t);
			t.lifecycle.request(TransitionStyle::Drop);
			t.runToStepLine(endLine);
			expect(t.stopIfEnded()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.holdsStart()).to_be_true();
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
		});
	});
});

CPPSPEC_SPEC(transition_lifecycle)
