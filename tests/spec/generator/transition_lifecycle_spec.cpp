// A transition's lifecycle (spec §12.9, §12.10): the phase and its rules in perform/transition_lifecycle.h, against the
// real core and Perform driver. The adapter is the harness's (transition_piece.h); the firmware's is
// generative/firmware/transitions.cpp, which the delugemu scripts check as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <iterator>
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

/// Fills the steering queue with presses that undo each other, leaving `room` places.
void fillQueue(TransitionPiece& t, int32_t room = 0) {
	while (t.queue.room() > room && t.push({CommandKind::SetHold, 0, kPress})) {}
}

/// Decides steps until the queue has drained, the presses that filled it with it.
void drainQueue(TransitionPiece& t) {
	while (t.queue.size() > 0) {
		decideOne(t);
	}
}

/// A Drop is confirmed, its command reaches the core, and a BACK tap drops it; no pump has run since.
void droppedBeforePump(TransitionPiece& t) {
	t.confirmAt(3, TransitionStyle::BuildIntoDrop);
	decideOne(t);
	t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
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

/// The pending Recipe gets a seed and takes unlike the outgoing piece's (3, all 0) and the harness's defaults, and the
/// player turns a macro since the confirm, away from the pending Recipe's. @return the Current Recipe a commit makes:
/// the pending Recipe with the live macros, its seed and its takes, as a Start for PLAY in Groove with `groove`.
Start distinctPending(TransitionPiece& t, bool groove = false) {
	t.lifecycle.pendingRecipe.seed = 11;
	t.lifecycle.pendingRecipe.takes.take[0] = 1;
	t.lifecycle.pendingRecipe.takes.take[1] = 2;
	t.currentRecipe.macros.value[0] = macroPoints(40);
	t.lifecycle.pendingRecipe.recipe.macros.value[0] = macroPoints(10);
	Start start{.seed = t.lifecycle.pendingRecipe.seed, .takes = t.lifecycle.pendingRecipe.takes, .skipIntro = groove};
	start.recipe = t.lifecycle.pendingRecipe.recipe;
	start.recipe.macros = t.currentRecipe.macros;
	return start;
}

/// The Current Recipe is `next`'s Recipe, seed and takes: the pending Recipe's, with the live macros.
bool currentIs(const TransitionPiece& t, const Start& next) {
	const Recipe& now = t.currentRecipe;
	return now.genre == next.recipe.genre && now.tempo == next.recipe.tempo && now.swing == next.recipe.swing
	       && now.keyRoot == next.recipe.keyRoot && now.macros == next.recipe.macros && t.currentSeed == next.seed
	       && std::equal(std::begin(t.currentTakes.take), std::end(t.currentTakes.take), std::begin(next.takes.take));
}

/// The song writes a stop's commit makes (the sink's tempo and swing, the steering adapter's swing interval and key)
/// are forgotten, so a spec sees only those after it.
void forgetSongWrites(TransitionPiece& t) {
	t.sink.tempos.clear();
	t.sink.swings.clear();
	t.steering.swingIntervals.clear();
	t.steering.songKeys.clear();
}

/// The song took the pending Recipe's tempo, swing, swing interval and key, once each, since forgetSongWrites().
bool songTookPendingOnce(const TransitionPiece& t) {
	const Recipe& pending = t.lifecycle.pendingRecipe.recipe;
	return t.sink.tempos.size() == 1 && t.sink.tempos[0].second == pending.tempo && t.sink.swings.size() == 1
	       && t.sink.swings[0].second == pending.swing
	       && t.steering.swingIntervals == std::vector<int32_t>{pending.swingInterval}
	       && t.steering.songKeys.size() == 1 && t.steering.songKeys[0].root == pending.keyRoot
	       && t.steering.songKeys[0].scale == pending.scale;
}

/// What a piece started from `start` plays in its first `bars` bars.
std::vector<Event> streamOf(const Start& start, int32_t bars) {
	StepwisePiece<> piece(start);
	piece.runBars(bars);
	return piece.sink.events;
}

/// What `t` plays in its next `bars` bars.
std::vector<Event> playedOver(TransitionPiece& t, int32_t bars) {
	const size_t from = t.sink.played.size();
	t.runBars(bars);
	std::vector<Event> out;
	for (size_t i = from; i < t.sink.played.size(); i++) {
		out.push_back(t.sink.played[i].event);
	}
	return out;
}

} // namespace

// clang-format off
describe transition_lifecycle("Transition lifecycle", $ {
	const int32_t phraseSteps = genres::technoAcid.phraseSteps();

	context("a request", _ {
		it("preloads while the roster plays on, then queues the incoming instance and its command", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true();
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			decideOne(t);
			expect(t.out->takeWaiting(0)).to_be_less_than(0);
		});

		it("drops a take pressed since the last decision, still in the steering queue", _ {
			TransitionPiece t;
			t.decideBefore(3);
			expect(t.steering.pressTake(0, 2) == perform::PressOutcome::Accepted).to_be_true();
			const uint32_t presses = t.steering.takePresses(0);
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true();
			expect(t.steering.takePresses(0)).to_equal(presses + 1); // a press, as the CLIP page counts them
			decideOne(t);
			expect(t.out->takeWaiting(0)).to_be_less_than(0);
		});

		it("is refused with QUEUE FULL, dropping nothing, when the queue has no room for the take it drops", _ {
			TransitionPiece t;
			t.decideBefore(3);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			fillQueue(t);
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::QueueFull).to_be_true();
			expect(t.lifecycle.events == Events{"request style=0 refused queue_full"}).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.preloads.empty()).to_be_true();
			expect(t.lifecycle.soundDrops).to_equal(0);
			drainQueue(t);
			expect(t.out->takeWaiting(0)).to_equal(2); // no transition: the take lands as pressed
		});

		it("needs a place for each take, each sound and the Queued one's cancel, and with them drops all", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			decideOne(t);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			t.pushAndDecide({CommandKind::SetTake, 1, 1});
			t.lifecycle.soundsWaiting = 1;
			fillQueue(t, 3); // the cancel, two takes and a sound: one short
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::QueueFull).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.soundsWaiting).to_equal(1);
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.queue.room()).to_equal(3);
			Command filler;
			t.queue.pop(&filler); // one place more
			const uint32_t presses = t.steering.takePresses(1);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.queue.room()).to_equal(0);
			expect(t.lifecycle.soundDropsLost).to_equal(0);
			expect(t.lifecycle.soundsWaiting).to_equal(0);
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.steering.takePresses(1)).to_equal(presses + 1);
			drainQueue(t);
			expect(t.out->takeWaiting(0)).to_be_less_than(0);
			expect(t.out->takeWaiting(1)).to_be_less_than(0);
			expect(t.out->transitionWaiting()).to_be_false();
		});

		it("replaces a transition that is Preloading", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
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
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Refused).to_be_true();
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			fillQueue(t);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::QueueFull).to_be_true();
			expect(t.lifecycle.queueFullPopups).to_equal(0); // the confirm shows it, from the request's outcome
			expect(t.lifecycle.has("request style=2 refused queue_full")).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(2);
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.preloads.size()).to_equal(size_t{1});
			expect(t.driver.incoming() != nullptr).to_be_true();
		});

		it("shows QUEUE FULL and ends when the loaded roster's command has no room", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Ignored).to_be_true();
			t.lifecycle.entered = true;
			t.lifecycle.left = true;
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Ignored).to_be_true();
			t.lifecycle.left = false;
			t.lifecycle.preloadStarts = false;
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Ignored).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.events.empty()).to_be_true();
		});

		it("changes nothing when its preload can't start: the Queued one, the takes and the sounds stay", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			decideOne(t);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			t.lifecycle.soundsWaiting = 1;
			const Events before = t.lifecycle.events;
			t.lifecycle.preloadStarts = false;
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Ignored).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.events == before).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(2);
			expect(t.lifecycle.soundDrops).to_equal(1); // the first confirm's
			expect(t.lifecycle.soundsWaiting).to_equal(1);
			expect(t.queue.size()).to_equal(0);
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_true();
			expect(t.out->takeWaiting(0)).to_equal(2);
			expect(t.driver.incoming() != nullptr).to_be_true();
			// The Queued one still works: its own preload's result was taken, and a later request still replaces it.
			t.lifecycle.preloadStarts = true;
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.lifecycle.preloads == (std::vector<uint32_t>{1, 2})).to_be_true();
		});

		it("keeps the Preloading one when the replacing preload can't start", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.lifecycle.preloadStarts = false;
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Ignored).to_be_true();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.load(1)).to_equal(2); // its preload is still the newest
			expect(t.lifecycle.discarded.empty()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			decideOne(t);
			const Command* waiting = t.out->waitingTransition();
			expect(waiting != nullptr && static_cast<TransitionStyle>(waiting->target) == TransitionStyle::BuildIntoDrop)
			    .to_be_true();
		});

		it("changes nothing when its preload ends inside the submit: the Queued one, the takes and the sounds stay", _ {
			// The firmware's runner, when the job's task can't start (a full task table): it ends Failed inside the
			// submit, which still returns true.
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			decideOne(t);
			t.pushAndDecide({CommandKind::SetTake, 0, 2});
			t.lifecycle.soundsWaiting = 1;
			const Events before = t.lifecycle.events;
			const uint32_t presses = t.steering.takePresses(0);
			t.lifecycle.preloadEndsAtOnce = true;
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Ignored).to_be_true();
			expect(t.lifecycle.preloads == (std::vector<uint32_t>{1, 2})).to_be_true(); // submitted, and ended
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.preloading()).to_be_false(); // no working animation
			expect(t.lifecycle.endedPreloads).to_equal(0);
			expect(t.lifecycle.events == before).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.incomingRoster).to_equal(2);
			expect(t.lifecycle.soundDrops).to_equal(1); // the first confirm's
			expect(t.lifecycle.soundsWaiting).to_equal(1);
			expect(t.steering.takePresses(0)).to_equal(presses);
			expect(t.queue.size()).to_equal(0);
			decideOne(t);
			expect(t.out->transitionWaiting()).to_be_true();
			expect(t.out->takeWaiting(0)).to_equal(2);
			expect(t.driver.incoming() != nullptr).to_be_true();
			// The next request whose preload starts replaces the Queued one, under the same number.
			t.lifecycle.preloadEndsAtOnce = false;
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.lifecycle.preloads == (std::vector<uint32_t>{1, 2, 2})).to_be_true();
			expect(t.lifecycle.load()).to_equal(3);
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
		});

		it("preloads the genre it is given, the pending Recipe's by default", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.lifecycle.request(TransitionStyle::Cut, &genres::technoDub);
			expect(t.lifecycle.preloadGenres == (std::vector<const Genre*>{&genres::technoAcid, &genres::technoDub}))
			    .to_be_true();
		});

		it("changes nothing when a job ends for an older request", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			fillQueue(t);
			expect(t.lifecycle.cancel()).to_be_false();
			expect(t.lifecycle.queueFullPopups).to_equal(1);
			expect(t.lifecycle.has("cancel queued refused")).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
		});

		it("is refused once the command has landed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
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
			droppedBeforePump(t);
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

		// Issue #206: the stop comes before the pump has ended the transition, so the phase is still Queued. The player
		// cancelled it: the stop ends it, and the old roster and Recipe (tempo, swing, key) stay.
		it("ends the transition at a stop that comes before the next pump", _ {
			TransitionPiece t;
			droppedBeforePump(t);
			expect(t.out->transitionWaiting()).to_be_false();
			const size_t tempos = t.sink.tempos.size();
			const size_t swings = t.sink.swings.size();
			t.stop();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(1);
			expect(t.lifecycle.currentRecipe).to_equal(1);
			expect(t.lifecycle.incomingRoster).to_equal(0);
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.lifecycle.starts()).to_equal(0u);
			expect(t.sink.tempos.size()).to_equal(tempos);
			expect(t.sink.swings.size()).to_equal(swings);
			expect(t.lifecycle.has("start stopped")).to_be_false();
			expect(t.lifecycle.events.back()).to_equal(std::string("phase=idle"));
			expect(t.lifecycle.has("cancel dropped")).to_be_true();
			const Events ended = t.lifecycle.events;
			t.pump();
			expect(t.lifecycle.events == ended).to_be_true();
			expect(t.lifecycle.retired.size()).to_equal(size_t{1});
		});

		it("keeps the playing Recipe's tempo at a stop after a hard cut whose tempo glide had started", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Cut, phraseSteps * kTicksPerStep); // a phrase: the glide starts at once
			t.hook.runUntil([&] { return !t.sink.tempos.empty() || !t.driver.running(); });
			expect(t.sink.tempos.empty()).to_be_false();
			expect(t.sink.tempos.empty() ? kOutTempo : t.sink.tempos.back().second).to_be_greater_than(kOutTempo);
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			expect(t.out->transitionWaiting()).to_be_false();
			const size_t swings = t.sink.swings.size();
			t.stop();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.currentRecipe).to_equal(1);
			expect(t.sink.tempos.empty() ? 0 : t.sink.tempos.back().second).to_equal(kOutTempo);
			expect(t.sink.swings.size()).to_equal(swings);
			const size_t tempos = t.sink.tempos.size();
			t.pump();
			expect(t.sink.tempos.size()).to_equal(tempos);
		});

		it("cancels it with CANCEL CHANGE before the next pump without a place in the steering queue", _ {
			TransitionPiece t;
			droppedBeforePump(t);
			fillQueue(t);
			expect(t.lifecycle.cancel()).to_be_true();
			expect(t.lifecycle.queueFullPopups).to_equal(0);
			expect(t.lifecycle.has("cancel queued")).to_be_true();
			expect(t.lifecycle.has("cancel queued refused")).to_be_false();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			t.pump();
			expect(t.lifecycle.has("cancel dropped")).to_be_false();
		});

		it("is replaced by a request before the next pump without a place in the steering queue", _ {
			TransitionPiece t;
			droppedBeforePump(t);
			fillQueue(t);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.lifecycle.has("cancel replaced")).to_be_true();
			expect(t.lifecycle.has("request style=2 refused queue_full")).to_be_false();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			t.pump();
			expect(t.lifecycle.has("cancel dropped")).to_be_false();
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
		});

		it("is not followed once Perform is being left: the roster goes with Perform's song", _ {
			TransitionPiece t;
			droppedBeforePump(t);
			t.lifecycle.left = true;
			t.pump();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.retired.empty()).to_be_true();
			expect(t.lifecycle.has("cancel dropped")).to_be_false();
		});

		it("leaves a transition alone whose command still waits in the core", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			for (int32_t i = 0; i < 8; i++) {
				decideOne(t);
				t.pump();
			}
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
		});
	});

	context("a stop commits in any phase, except a Queued transition whose command the core dropped", _ {
		it("commits a Preloading one when it has loaded", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
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

		it("plays the pending Recipe's stream, with its seed, its takes and the live macros, at the next PLAY", _ {
			TransitionPiece t;
			const Start next = distinctPending(t);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			decideOne(t);
			t.stop();
			t.pressPlay();
			expect(t.lifecycle.plays == std::vector<bool>{false}).to_be_true();
			expect(t.driver.running()).to_be_true();
			expect(t.sink.tempos.empty() ? 0 : t.sink.tempos.back().second).to_equal(kInTempo);
			const std::vector<Event> played = playedOver(t, 4);
			expect(played.empty()).to_be_false();
			expect(played == streamOf(next, 4)).to_be_true();
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
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
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
			const int64_t startedAt = t.hook.lastCall();
			t.hook.runUntil([&] { return t.hook.lastCall() > startedAt; });
			expect(t.driver.outgoing() != nullptr).to_be_true();
			const auto elapsed = static_cast<uint32_t>(t.hook.now() - startedAt);
			expect(elapsed).to_be_greater_than(0U);
			t.stop();
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.lifecycle.phrase - elapsed}}).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
		});
	});

	context("the handover: the pending Recipe becomes the Current Recipe (spec §12.9)", _ {
		it("takes the pending seed and takes, with the live macros, at the start step", _ {
			TransitionPiece t;
			const Start next = distinctPending(t);
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			expect(currentIs(t, next)).to_be_true();
		});

		it("takes the pending seed and takes, with the live macros, at a stop's commit", _ {
			TransitionPiece t;
			const Start next = distinctPending(t);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			t.stop();
			expect(currentIs(t, next)).to_be_true();
		});

		it("writes the new Recipe's tempo, swing, swing interval and key to the song once each at a stop's commit", _ {
			TransitionPiece t;
			t.lifecycle.pendingRecipe.recipe.swingInterval = 1;
			t.lifecycle.pendingRecipe.recipe.scale = Scale::Dorian;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			decideOne(t);
			if (t.driver.running()) {
				t.driver.stop(t.sink);
			}
			forgetSongWrites(t);
			t.lifecycle.transport = false;
			t.lifecycle.playbackEnded();
			expect(t.lifecycle.has("start stopped")).to_be_true();
			expect(songTookPendingOnce(t)).to_be_true();
		});

		it("writes them once each too when the piece ended by itself with the transition Queued", _ {
			TransitionPiece t;
			t.lifecycle.pendingRecipe.recipe.swingInterval = 1;
			t.lifecycle.pendingRecipe.recipe.scale = Scale::Dorian;
			const int32_t endLine = playToOutroEnd(t);
			t.runToStepLine(endLine, -6);
			t.confirm(TransitionStyle::BuildIntoDrop);
			t.runToStepLine(endLine);
			expect(t.driver.ended()).to_be_true();
			forgetSongWrites(t);
			expect(t.stopIfEnded()).to_be_true();
			expect(t.lifecycle.has("start stopped")).to_be_true();
			expect(songTookPendingOnce(t)).to_be_true();
		});

		it("writes no swing interval or key to the song at the start step: the new instance plays them", _ {
			TransitionPiece t;
			t.lifecycle.pendingRecipe.recipe.swingInterval = 1;
			t.confirmAt(3, TransitionStyle::Cut);
			forgetSongWrites(t);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			t.runBars(1);
			expect(t.steering.swingIntervals.empty()).to_be_true();
			expect(t.steering.songKeys.empty()).to_be_true();
		});
	});

	context("confirmed while stopped (spec §12.10)", _ {
		it("commits when loaded, and holds a start until then", _ {
			TransitionPiece t;
			t.stop();
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true();
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop); // playing: it doesn't commit on load
			expect(t.lifecycle.startWhenLoaded(false)).to_be_false();
		});

		it("cancels the wait when PLAY is pressed again", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
			cancelled.lifecycle.request(TransitionStyle::BuildIntoDrop);
			cancelled.pressPlay();
			expect(cancelled.lifecycle.cancel()).to_be_true();
			expect(cancelled.lifecycle.plays == std::vector<bool>{false}).to_be_true();
			expect(cancelled.lifecycle.playingRoster).to_equal(1);

			TransitionPiece failed;
			failed.stop();
			failed.lifecycle.request(TransitionStyle::BuildIntoDrop);
			failed.pressPlay();
			failed.lifecycle.failLoad();
			expect(failed.lifecycle.plays == std::vector<bool>{false}).to_be_true();
			expect(failed.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(failed.lifecycle.endedPreloads).to_equal(1);
		});

		it("plays the pending Recipe's stream once a waiting PLAY starts it", _ {
			TransitionPiece t;
			const Start next = distinctPending(t, true);
			t.stop();
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.pressPlay(true);
			expect(t.driver.running()).to_be_false();
			t.lifecycle.load();
			expect(t.driver.running()).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(2);
			const std::vector<Event> played = playedOver(t, 4);
			expect(played.empty()).to_be_false();
			expect(played == streamOf(next, 4)).to_be_true();
		});

		it("keeps a waiting PLAY for the request that replaces the one it waited for", _ {
			TransitionPiece t;
			t.stop();
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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

		it("caps the outgoing roster at a phrase of its own genre, not the incoming one's", _ {
			TransitionPiece t;
			const uint32_t outgoing = t.lifecycle.phrase;
			t.lifecycle.pendingPhrase = 3 * outgoing;
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const int64_t startedAt = t.hook.lastCall();
			expect(t.lifecycle.phrase).to_equal(3 * outgoing); // the new genre's Recipe is Current from the start step
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const uint32_t cap = outgoing - static_cast<uint32_t>(t.hook.lastCall() - startedAt);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
		});

		it("caps the roster that played at a stop's commit at a phrase of its own genre", _ {
			TransitionPiece t;
			const uint32_t outgoing = t.lifecycle.phrase;
			t.lifecycle.pendingPhrase = 3 * outgoing;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			t.stop();
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, outgoing}}).to_be_true();
		});

		it("caps a 4-bar genre's outgoing roster at its 4-bar phrase, into an 8-bar one", _ {
			TransitionPiece t(&kFourBarPhrases, &genres::technoAcid);
			const uint32_t outgoing = t.phraseOf(kFourBarPhrases);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const int64_t startedAt = t.hook.lastCall();
			expect(t.lifecycle.phrase).to_equal(t.phraseOf(genres::technoAcid));
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const uint32_t cap = outgoing - static_cast<uint32_t>(t.hook.lastCall() - startedAt);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
		});

		it("caps an 8-bar genre's roster at its 8-bar phrase at a stop's commit, into a 1-bar one", _ {
			TransitionPiece t(&genres::technoAcid, &kOneBarPhrases);
			t.confirmAt(3, TransitionStyle::Fade);
			t.stop();
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.phraseOf(genres::technoAcid)}}).to_be_true();
			expect(t.lifecycle.phrase).to_equal(t.phraseOf(kOneBarPhrases));
		});

		it("caps at the outgoing genre's phrase when a stop commits a preload that ends after it", _ {
			TransitionPiece t(&kOneBarPhrases, &genres::technoAcid);
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.stop();
			t.lifecycle.load();
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, t.phraseOf(kOneBarPhrases)}}).to_be_true();
		});

		it("leaves a 1-bar genre's outgoing roster no cap when its Throws still fall a phrase after the start step", _ {
			// Dub's case (1-bar phrases): the Throws hold a bar from the last hits, then fall over a beat.
			TransitionPiece t(&kOneBarPhrases, &genres::technoAcid);
			t.confirmAt(3, TransitionStyle::Cut);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const int64_t startedAt = t.hook.lastCall();
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			expect(t.hook.lastCall() - startedAt).to_be_greater_than(int64_t{t.phraseOf(kOneBarPhrases)} - 1);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, 0}}).to_be_true();
		});

		it("caps a FADE THROUGH's outgoing roster from 1-bar phrases at a 1-bar phrase", _ {
			TransitionPiece t(&kOneBarPhrases, &genres::technoAcid);
			t.confirmAt(3, TransitionStyle::Fade);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const auto startedAt = static_cast<uint32_t>(t.hook.lastCall());
			expect(t.lifecycle.phrase).to_equal(t.phraseOf(genres::technoAcid));
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const uint32_t cap =
			    perform::capAfterDrain(startedAt + t.phraseOf(kOneBarPhrases), static_cast<uint32_t>(t.hook.lastCall()));
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
		});

		it("caps a FADE THROUGH's outgoing roster into 1-bar phrases at its own 8-bar phrase", _ {
			TransitionPiece t(&genres::technoAcid, &kOneBarPhrases);
			t.confirmAt(3, TransitionStyle::Fade);
			t.hook.runUntil([&] { return t.sink.starts == 1; });
			const auto startedAt = static_cast<uint32_t>(t.hook.lastCall());
			expect(t.lifecycle.phrase).to_equal(t.phraseOf(kOneBarPhrases));
			t.hook.runUntil([&] { return t.sink.drained == 1; });
			const auto drainedAt = static_cast<uint32_t>(t.hook.lastCall());
			expect(drainedAt - startedAt).to_be_less_than(t.phraseOf(genres::technoAcid));
			const uint32_t cap = t.phraseOf(genres::technoAcid) - (drainedAt - startedAt);
			expect(t.lifecycle.retired == std::vector<Retired>{{1, true, cap}}).to_be_true();
		});

		it("leaves no cap when the drain took a phrase or more", _ {
			expect(perform::capAfterDrain(1000, 0)).to_equal(1000U);
			expect(perform::capAfterDrain(1000, 400)).to_equal(600U);
			expect(perform::capAfterDrain(1000, 1000)).to_equal(0U);
			expect(perform::capAfterDrain(1000, 5000)).to_equal(0U);
			expect(perform::capAfterDrain(100, 0xFFFFFF00U)).to_equal(356U); // the sample timer wrapped meanwhile
			expect(perform::capAfterDrain(0xFFFFFF00U, 100)).to_equal(0U);
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
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true();
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

	// Each roster a transition builds retires once. The free job runs only while the runner is idle, so requests that
	// replace each other while a preload keeps it busy pile rosters up, past the transitions' share of the pool.
	context("the transitions' share of the Outputs to free (spec §12.9)", _ {
		constexpr int32_t kShare = TransitionAdapter::kTransitionShare;

		it("refuses a request with BUSY while the rosters it may retire don't fit, and accepts it once freed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::BuildIntoDrop); // Queued, with roster 2
			// Replacing the Queued one retires roster 2 at once, and this one may retire a roster: both fit.
			expect(t.lifecycle.retirementRoom()).to_equal(kShare);
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			// Roster 2 waits for the free job, which can't run while the preload does; the preload may still end for
			// an older request and retire what it built. A third would owe one more: the share is full.
			expect(t.lifecycle.retirementRoom()).to_equal(kShare - 2 * kMaxParts);
			const Events before = t.lifecycle.events;
			expect(t.lifecycle.request(TransitionStyle::Fade) == Request::Busy).to_be_true();
			expect(perform::pressOutcome(Request::Busy) == perform::PressOutcome::Busy).to_be_true();
			expect(t.lifecycle.events.size()).to_equal(before.size() + 1);
			expect(t.lifecycle.events.back()).to_equal(std::string("request style=1 refused busy"));
			// Nothing changed: the preload that runs runs on.
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(t.lifecycle.preloads == (std::vector<uint32_t>{1, 2})).to_be_true();
			// Loaded, it is Queued: a request now retires its roster and owes one, and roster 2 still waits.
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.lifecycle.request(TransitionStyle::Fade) == Request::Busy).to_be_true();
			t.lifecycle.freeRetired();
			expect(t.lifecycle.request(TransitionStyle::Fade) == Request::Started).to_be_true();
		});

		it("counts a cancelled preload still running, which retires what it built if it ends after its last boundary",
		   _ {
			   TransitionPiece t;
			   t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			   t.lifecycle.request(TransitionStyle::Cut); // roster 2 retired at once
			   expect(t.lifecycle.cancel()).to_be_true();
			   expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			   // Idle, but the cancelled job still unwinds: with roster 2, there's no room for another roster.
			   expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Busy).to_be_true();
			   t.lifecycle.load(2); // past its last boundary: its roster never played, and is retired
			   expect(t.lifecycle.discarded == std::vector<int32_t>{3}).to_be_true();
			   expect(kMaxParts * t.lifecycle.freeing).to_equal(kShare); // full, never past it
			   expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Busy).to_be_true();
			   t.lifecycle.freeRetired();
			   expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true();
		   });

		it("lets requests replace each other while the runner stays busy, as long as nothing waits to be freed", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Started).to_be_true(); // running
			expect(t.lifecycle.request(TransitionStyle::Cut) == Request::Started).to_be_true();  // next slot
			expect(t.lifecycle.request(TransitionStyle::Fade) == Request::Started).to_be_true(); // replaces it there
			t.lifecycle.load(1); // the first ends for an older request: its roster is retired
			expect(t.lifecycle.request(TransitionStyle::BuildIntoDrop) == Request::Busy).to_be_true();
			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(kMaxParts * t.lifecycle.freeing).to_be_less_than(kShare + 1);
		});
	});

	// Ticket #195, step 1: can a piece end by itself while a transition is Queued or Committed? The adapter then
	// stops the transport from inside the tick, and the stop's commit runs there.
	context("a piece that ends by itself, with End armed before the confirm", _ {
		it("never ends while the transition's command waits in the core", _ {
			for (const TransitionStyle style : {TransitionStyle::BuildIntoDrop, TransitionStyle::Fade, TransitionStyle::Cut}) {
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
			t.confirm(TransitionStyle::BuildIntoDrop);
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

		it("ends after a BACK tap dropped the command, and the stop ends the transition", _ {
			// The tap reaches the end line's decision with the command, so the core holds nothing to keep it playing.
			TransitionPiece t;
			const int32_t endLine = playToOutroEnd(t);
			t.runToStepLine(endLine - 1, -1);
			t.confirm(TransitionStyle::BuildIntoDrop);
			t.push({CommandKind::CancelNewest, 0, 0});
			t.runToStepLine(endLine);
			expect(t.driver.ended()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			// The adapter's tick stops the transport before any pump: the stop ends the transition, inside the tick.
			expect(t.stopIfEnded()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.playingRoster).to_equal(1);
			expect(t.lifecycle.currentRecipe).to_equal(1);
			expect(t.lifecycle.retired == std::vector<Retired>{{2, false, 0}}).to_be_true();
			expect(t.lifecycle.has("cancel dropped")).to_be_true();
			expect(t.lifecycle.has("start stopped")).to_be_false();
		});

		it("keeps the playing Recipe's tempo when it ends after a BACK tap dropped a hard cut whose glide had started", _ {
			const int32_t endLine = [] {
				TransitionPiece first;
				return playToOutroEnd(first);
			}();
			TransitionPiece t; // the same piece, with a hard cut confirmed in its last bar: its cut line is the end line
			t.push({CommandKind::End, 0, 1});
			t.runToStepLine(endLine - kStepsPerBar / 2, -1);
			t.confirm(TransitionStyle::Cut, phraseSteps * kTicksPerStep); // a phrase: the glide starts at once
			t.runToStepLine(endLine - 1, -1);
			expect(t.out->transitionWaiting()).to_be_true();
			expect(t.sink.tempos.empty() ? kOutTempo : t.sink.tempos.back().second).to_be_greater_than(kOutTempo);
			// The tap reaches the end line's decision with the command still waiting there and drops it: with nothing
			// to keep it going, the piece ends a line later. No pump runs before.
			t.push({CommandKind::CancelNewest, 0, 0});
			t.hook.runUntil([&] { return t.driver.ended() || !t.driver.running(); });
			expect(t.driver.ended()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(t.stopIfEnded()).to_be_true();
			expect(t.lifecycle.phase() == Phase::Idle).to_be_true();
			expect(t.lifecycle.currentRecipe).to_equal(1);
			expect(t.lifecycle.has("cancel dropped")).to_be_true();
			expect(t.sink.tempos.empty() ? 0 : t.sink.tempos.back().second).to_equal(kOutTempo);
			expect(t.sink.swings.empty()).to_be_true();
		});

		it("ends while Preloading, and commits when loaded", _ {
			TransitionPiece t;
			const int32_t endLine = playToOutroEnd(t);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
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
