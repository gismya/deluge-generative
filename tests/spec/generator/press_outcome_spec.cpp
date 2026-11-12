// What a steering press did (spec §2.6, §12.9): perform/press_outcome.h, and every steering press of the Perform
// driver's steering (perform/perform_steering.h) refused while a transition is pending, against the real core, Perform
// driver and transition lifecycle. NEW SOUND is the firmware's (SoundSwaps): only its gate is checked here, and the
// delugemu scripts check it as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <functional>
#include <vector>

using namespace harness;

namespace {

using perform::PressKind;
using perform::PressOutcome;
using Phase = perform::TransitionPhase;
using Request = perform::TransitionRequest;

/// One steering press, as the Perform view, a menu or MIDI makes it, and whether spec §12.9 refuses it while a
/// transition is pending.
struct Press {
	const char* name;
	bool refusedWhilePending;
	std::function<PressOutcome(TransitionPiece&)> press;
};

/// Every steering press, in an order where each acts when nothing is pending: End's tap after its hold.
std::vector<Press> everyPress() {
	return {
	    {"mute", false, [](TransitionPiece& t) { return t.steering.pressMute(0, false); }},
	    {"solo", false, [](TransitionPiece& t) { return t.steering.pressSolo(1, false); }},
	    {"hold", false, [](TransitionPiece& t) { return t.steering.pressHold(); }},
	    {"force", false, [](TransitionPiece& t) { return t.steering.forceSection(SectionKind::Break); }},
	    {"freeze", false, [](TransitionPiece& t) { return t.steering.pressFreeze(0); }},
	    {"take", true, [](TransitionPiece& t) { return t.steering.pressTake(0, 1); }},
	    {"key", true,
	     [](TransitionPiece& t) {
		     Key key = t.steering.keyAhead();
		     key.root = static_cast<uint8_t>((key.root + 1) % 12);
		     return t.steering.queueKeyChange(key);
	     }},
	    {"tempo", true, [](TransitionPiece& t) { return t.steering.setTempo(13000); }},
	    {"swing", true, [](TransitionPiece& t) { return t.steering.setSwing(5); }},
	    {"swing interval", true, [](TransitionPiece& t) { return t.steering.setSwingInterval(1); }},
	    {"End hold", true, [](TransitionPiece& t) { return t.steering.armEnd(); }},
	    {"End tap", false, [](TransitionPiece& t) { return t.steering.cancelEnd(); }},
	    {"BACK", false, [](TransitionPiece& t) { return t.steering.cancelNewest(); }},
	};
}

/// Presses every steering press with the lifecycle as it is, then puts the queue back as it was, so the piece plays on
/// as it would have. A press §12.9 lists gets CHANGE PENDING exactly when `pending`; the rest are never refused.
/// @return the presses that went wrong, or "".
std::string pressesMatch(TransitionPiece& t, bool pending) {
	const SteeringQueue before = t.queue;
	std::string wrong;
	for (const Press& p : everyPress()) {
		const PressOutcome outcome = p.press(t);
		const bool ok =
		    pending && p.refusedWhilePending ? outcome == PressOutcome::ChangePending : !perform::refused(outcome);
		if (!ok) {
			wrong += std::string(p.name) + " got " + std::to_string(static_cast<int>(outcome)) + "; ";
		}
	}
	// NEW SOUND's press is the firmware's: its gate.
	const PressOutcome sound = t.lifecycle.gate(PressKind::Sound);
	if (sound != (pending ? PressOutcome::ChangePending : PressOutcome::Accepted)) {
		wrong += "sound's gate; ";
	}
	t.queue = before;
	return wrong;
}

/// Plays until the transition's command has landed.
void land(TransitionPiece& t) {
	t.hook.runUntil([&] { return t.out->transitionStart() >= 0 || !t.driver.running(); });
}

/// Fills the steering queue with presses that undo each other.
void fillQueue(TransitionPiece& t) {
	while (t.push({CommandKind::SetHold, 0, kPress})) {}
}

} // namespace

// clang-format off
describe press_outcome("Press outcome", $ {
	context("while a transition is pending (spec §12.9)", _ {
		it("refuses each press §12.9 lists with CHANGE PENDING, and lets the rest through", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(pressesMatch(t, false)).to_equal(std::string{}); // Idle

			t.lifecycle.request(TransitionStyle::Fade);
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(pressesMatch(t, true)).to_equal(std::string{});

			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(pressesMatch(t, true)).to_equal(std::string{});

			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
			expect(pressesMatch(t, true)).to_equal(std::string{});

			t.hook.runUntil([&] { return t.lifecycle.phase() == Phase::Retiring; });
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(pressesMatch(t, false)).to_equal(std::string{}); // the new roster plays: its edits act
		});

		it("refuses a take step while pending: no take is pressed", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			const perform::TakeRow row = t.steering.takeRow(0);
			PressOutcome outcome = PressOutcome::Accepted;
			expect(t.steering.stepTake(0, row, 1, &outcome)).to_equal(-1);
			expect(outcome == PressOutcome::ChangePending).to_be_true();
		});

		it("refuses a transition request with CHANGE PENDING while one is Committed or Retiring", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Fade);
			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Drop)) == PressOutcome::ChangePending)
			    .to_be_true();
			t.hook.runUntil([&] { return t.lifecycle.phase() == Phase::Retiring; });
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Drop)) == PressOutcome::ChangePending)
			    .to_be_true();
		});

		it("replaces one Preloading or Queued: the request is accepted", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Cut)) == PressOutcome::Accepted)
			    .to_be_true();
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Drop)) == PressOutcome::Accepted)
			    .to_be_true();
			t.lifecycle.load();
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Cut)) == PressOutcome::Accepted)
			    .to_be_true();
		});

		it("refuses the request with QUEUE FULL when the Queued one's cancel has no room, and leaves the popup to "
		   "the caller", _ {
			TransitionPiece t;
			t.confirmAt(3, TransitionStyle::Drop);
			fillQueue(t);
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Cut)) == PressOutcome::QueueFull)
			    .to_be_true();
			expect(t.lifecycle.queueFullPopups).to_equal(0);
		});

		it("has nothing to do outside Perform", _ {
			TransitionPiece t;
			t.lifecycle.entered = false;
			expect(perform::pressOutcome(t.lifecycle.request(TransitionStyle::Cut)) == PressOutcome::NothingToDo)
			    .to_be_true();
		});
	});

	context("with a full queue", _ {
		it("refuses a discrete press with QUEUE FULL, and a take step presses no take", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			fillQueue(t);
			expect(t.steering.pressMute(0, false) == PressOutcome::QueueFull).to_be_true();
			expect(t.steering.armEnd() == PressOutcome::QueueFull).to_be_true();
			PressOutcome outcome = PressOutcome::Accepted;
			expect(t.steering.stepTake(0, t.steering.takeRow(0), 1, &outcome)).to_equal(-1);
			expect(outcome == PressOutcome::QueueFull).to_be_true();
		});

		it("has nothing to do for an End tap with no End armed or queued, rather than refuse it", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(t.steering.cancelEnd() == PressOutcome::NothingToDo).to_be_true();
			expect(t.steering.armEnd() == PressOutcome::Accepted).to_be_true();
			fillQueue(t);
			expect(t.steering.cancelEnd() == PressOutcome::QueueFull).to_be_true(); // End waits in the queue
			t.queue.clear();
			expect(t.steering.cancelEnd() == PressOutcome::NothingToDo).to_be_true();
			expect(t.steering.armEnd() == PressOutcome::Accepted).to_be_true();
			t.decideBefore(t.driver.playing()->nextStep() + 1); // the core has End waiting for its line
			expect(t.driver.playing()->endQueued()).to_be_true();
			fillQueue(t);
			expect(t.steering.cancelEnd() == PressOutcome::QueueFull).to_be_true();
		});
	});

	context("the outcome", _ {
		it("is refused for QUEUE FULL, CHANGE PENDING and BUSY only", _ {
			expect(perform::refused(PressOutcome::Accepted)).to_be_false();
			expect(perform::refused(PressOutcome::NothingToDo)).to_be_false();
			expect(perform::refused(PressOutcome::QueueFull)).to_be_true();
			expect(perform::refused(PressOutcome::ChangePending)).to_be_true();
			expect(perform::refused(PressOutcome::Busy)).to_be_true();
		});

		it("of a discrete command's push is QUEUE FULL when the queue had no room", _ {
			expect(perform::queued(true) == PressOutcome::Accepted).to_be_true();
			expect(perform::queued(false) == PressOutcome::QueueFull).to_be_true();
		});
	});
});
// clang-format on
CPPSPEC_SPEC(press_outcome)
