// The Perform driver's steering (spec §2.6, §8.7, §13.5, §12.7): what a press keeps across a piece start and a
// transition's start step, and what a press while stopped does at once. The presses are the real entry points
// (perform/perform_steering.h), on the real core and Perform driver, with the harness as the firmware.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <string>

using namespace harness;

namespace {

using perform::PressOutcome;

/// A Techno piece from its Groove, with its steering log.
struct Piece : StepwisePiece<> {
	Piece() : StepwisePiece({.seed = 3, .skipIntro = true, .logEntries = 64}) {}
	/// What the core plays for `macro` now: the SetMacro that last landed, or the Recipe's.
	[[nodiscard]] int32_t corePlays(Macro macro) const { return driver.playing()->recipe().macros[macro]; }
	/// Has the instance that plays decide `steps` more steps.
	void decideMore(int32_t steps) { decideBefore(driver.playing()->nextStep() + steps); }
};

/// The SetFill a FILL press sends: the held-fill entry, or a fresh pick with SHIFT (spec §8.7).
std::string fillPress(bool shift) {
	return "gen_fill_press value=" + std::to_string(shift ? kFillFreshPick : kFillHeldEntry);
}

/// The value of the first SetFill logged, or -1.
int32_t firstFillLogged(const SteeringLog& log) {
	for (int32_t i = 0; i < log.size(); i++) {
		if (log.at(i).command.kind == CommandKind::SetFill) {
			return log.at(i).command.value;
		}
	}
	return -1;
}

constexpr int32_t kHeld = macroPoints(40);

} // namespace

// clang-format off
describe perform_steering("The Perform driver's steering", $ {
	context("FILL (spec §8.7)", _ {
		it("held across a piece start, reaches the new piece at once, with its SHIFT", _ {
			for (bool shift : {false, true}) {
				Piece piece;
				piece.runToStepLine(4);
				piece.steering.latchFillShift(shift);
				piece.fillDown = true;
				piece.decideMore(2);
				expect(piece.generator.heldFill() >= 0).to_be_true();
				expect(piece.steering.events.back()).to_equal(fillPress(shift));

				// Stop and play again with FILL still down: the new piece plays it from its first steps.
				piece.playFromRecipe(true);
				piece.decideBefore(2);
				expect(piece.generator.heldFill() >= 0).to_be_true();
				expect(firstFillLogged(piece.log)).to_equal(shift ? kFillFreshPick : kFillHeldEntry);
				expect(piece.steering.events.size()).to_equal(size_t{2});
				expect(piece.steering.events.back()).to_equal(fillPress(shift));

				piece.fillDown = false;
				piece.decideMore(2);
				expect(piece.generator.heldFill()).to_equal(-1);
				expect(piece.steering.events.back()).to_equal(std::string("gen_fill_press value=0"));
			}
		});

		it("let go while stopped, doesn't reach the next piece, nor does its SHIFT", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			piece.steering.latchFillShift(true);
			piece.fillDown = true; // down and up again while stopped: no tick saw it
			piece.fillDown = false;
			piece.playFromRecipe(true);
			piece.decideBefore(4);
			expect(piece.generator.heldFill()).to_equal(-1);
			expect(piece.steering.events.empty()).to_be_true();
			// A plain press now plays the held-fill entry: the SHIFT latched before went with the release.
			piece.fillDown = true;
			piece.decideMore(2);
			expect(piece.steering.events.back()).to_equal(fillPress(false));
		});

		it("held across a transition's start step, reaches the incoming instance, with its SHIFT", _ {
			for (bool shift : {false, true}) {
				TransitionPiece t;
				t.runToStepLine(3, -1);
				t.steering.latchFillShift(shift);
				t.fillDown = true;
				t.confirm(TransitionStyle::Cut);
				const Generator* outgoing = t.driver.playing();
				t.decideBefore(6);
				expect(outgoing->heldFill() >= 0).to_be_true();
				expect(t.hook.runUntil([&] { return t.sink.starts > 0; })).to_be_true();
				const Generator* incoming = t.driver.playing();
				expect(incoming != outgoing).to_be_true();
				t.decideBefore(2);
				expect(incoming->heldFill() >= 0).to_be_true();
				expect(t.steering.events.size()).to_equal(size_t{2});
				expect(t.steering.events.back()).to_equal(fillPress(shift));
			}
		});
	});

	context("a KEYBOARD fader (spec §13.5)", _ {
		it("held across a piece start, plays its value in the new piece; a knob turn during it moves only the centre",
		   _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.holdMacro(Macro::Energy, kHeld);
			piece.decideMore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			piece.steering.setMacro(Macro::Energy, macroPoints(10)); // a knob turn while the fader holds it
			piece.decideMore(2);
			expect(piece.currentRecipe.macros[Macro::Energy]).to_equal(macroPoints(10));
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			piece.playFromRecipe(true); // still held
			piece.decideBefore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			piece.steering.setMacro(Macro::Energy, macroPoints(15)); // and again, in the new piece
			piece.decideMore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			piece.steering.springMacroBack(Macro::Energy); // a hold let go: back to the centre
			piece.decideMore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(macroPoints(15));
			expect(piece.currentRecipe.macros[Macro::Energy]).to_equal(macroPoints(15));
		});

		it("tapped across a piece start, keeps its value as the centre; a knob turn during the tap gives way to it", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.holdMacro(Macro::Energy, kHeld); // the pad goes down
			piece.playFromRecipe(true);                     // a piece starts during the tap
			piece.steering.setMacro(Macro::Energy, macroPoints(10));
			piece.decideBefore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			piece.steering.keepMacroCentre(Macro::Energy, kHeld); // and up within the tap time
			piece.decideMore(2);
			expect(piece.currentRecipe.macros[Macro::Energy]).to_equal(kHeld);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);

			// The next start plays it as the centre.
			piece.playFromRecipe(true);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);
		});

		it("tapped while stopped, is the next piece's centre", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			piece.steering.holdMacro(Macro::Energy, kHeld);
			piece.steering.keepMacroCentre(Macro::Energy, kHeld);
			piece.playFromRecipe(true);
			piece.decideBefore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(kHeld);
			expect(piece.steering.centresMoved.size()).to_equal(size_t{1}); // MIDI feedback for the new centre
		});
	});

	context("stopped (spec §13.2, §13.5)", _ {
		it("a take, a key and a tempo edit act at once, and the next start plays them", _ {
			StepwisePiece<TimedSink> piece({.seed = 3, .skipIntro = true});
			piece.driver.setTempo(12800, piece.sink);
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			piece.queue.clear();

			expect(piece.steering.pressTake(1, 3) == PressOutcome::Accepted).to_be_true();
			expect(static_cast<int32_t>(piece.currentTakes.take[1])).to_equal(3);
			expect(piece.steering.take(1)).to_equal(3);
			expect(piece.steering.takeRow(1).playing).to_equal(3);
			expect(piece.steering.events.back()).to_equal(std::string("gen_take part=1 take=3 stopped"));

			const Key key{5, Scale::Dorian};
			expect(piece.steering.queueKeyChange(key) == PressOutcome::Accepted).to_be_true();
			expect(piece.steering.keyAhead() == key).to_be_true();
			expect(piece.steering.keyChangePending()).to_be_false();
			expect(piece.steering.songKeys.size()).to_equal(size_t{1});
			expect(piece.steering.songKeys.back() == key).to_be_true();
			expect(piece.steering.events.back())
			    .to_equal("gen_key_set root=5 scale=" + std::to_string(static_cast<int32_t>(Scale::Dorian)) + " stopped");
			expect(piece.queue.size()).to_equal(0); // nothing waits for a line

			const size_t written = piece.sink.tempos.size();
			expect(piece.steering.setTempo(13000) == PressOutcome::Accepted).to_be_true();
			expect(piece.currentRecipe.tempo).to_equal(13000);
			expect(piece.sink.tempos.size()).to_equal(written + 1); // written at once, with no glide
			expect(piece.sink.tempos.back().second).to_equal(13000);

			piece.playFromRecipe(true);
			expect(piece.generator.take(1)).to_equal(3);
			expect(piece.generator.keyAhead() == key).to_be_true();
			expect(piece.sink.tempos.back().second).to_equal(13000);
		});

		it("the sidebar's presses have nothing to do", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			expect(piece.steering.pressMute(0, false) == PressOutcome::NothingToDo).to_be_true();
			expect(piece.steering.pressHold() == PressOutcome::NothingToDo).to_be_true();
			expect(piece.steering.forceSection(SectionKind::Break) == PressOutcome::NothingToDo).to_be_true();
			expect(piece.steering.armEnd() == PressOutcome::NothingToDo).to_be_true();
			expect(piece.steering.cancelEnd() == PressOutcome::NothingToDo).to_be_true();
		});
	});
});
// clang-format on
CPPSPEC_SPEC(perform_steering)
