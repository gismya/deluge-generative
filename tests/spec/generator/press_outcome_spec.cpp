// What a steering press did (spec §2.6, §12.9): perform/press_outcome.h, and the transition lifecycle's gate for the
// presses a pending transition refuses, against the real core and Perform driver. The presses themselves are the
// firmware's (PerformAdapter, SoundSwaps), which the delugemu scripts check as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <array>

using namespace harness;

namespace {

using perform::PressKind;
using perform::PressOutcome;
using Phase = perform::TransitionPhase;
using Request = perform::TransitionRequest;

constexpr std::array kAllKinds{PressKind::Mute,      PressKind::Solo,   PressKind::Hold,          PressKind::Force,
                               PressKind::Back,      PressKind::Freeze, PressKind::Take,          PressKind::Key,
                               PressKind::Tempo,     PressKind::Swing,  PressKind::SwingInterval, PressKind::EndArm,
                               PressKind::EndCancel, PressKind::Sound};

/// The presses spec §12.9 lists as refused while a transition is pending.
bool refusedBySpec(PressKind kind) {
	switch (kind) {
	case PressKind::Take:
	case PressKind::Key:
	case PressKind::Tempo:
	case PressKind::Swing:
	case PressKind::SwingInterval:
	case PressKind::EndArm:
	case PressKind::Sound:
		return true;
	default:
		return false;
	}
}

/// Every press kind's outcome at the gate, with the lifecycle as it is: ChangePending or Accepted (the press goes on).
bool gateMatches(TransitionPiece& t, bool pending) {
	for (const PressKind kind : kAllKinds) {
		const PressOutcome want = pending && refusedBySpec(kind) ? PressOutcome::ChangePending : PressOutcome::Accepted;
		if (t.lifecycle.gate(kind) != want) {
			return false;
		}
	}
	return true;
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
		it("refuses each press kind §12.9 lists with CHANGE PENDING, and lets the rest through", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			expect(gateMatches(t, false)).to_be_true(); // Idle

			t.lifecycle.request(TransitionStyle::Fade);
			expect(t.lifecycle.phase() == Phase::Preloading).to_be_true();
			expect(gateMatches(t, true)).to_be_true();

			t.lifecycle.load();
			expect(t.lifecycle.phase() == Phase::Queued).to_be_true();
			expect(gateMatches(t, true)).to_be_true();

			land(t);
			expect(t.lifecycle.phase() == Phase::Committed).to_be_true();
			expect(gateMatches(t, true)).to_be_true();

			t.hook.runUntil([&] { return t.lifecycle.phase() == Phase::Retiring; });
			expect(t.lifecycle.phase() == Phase::Retiring).to_be_true();
			expect(gateMatches(t, false)).to_be_true(); // the new roster plays: its edits act
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
