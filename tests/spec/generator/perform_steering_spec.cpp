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
	/// What the core plays as `part`'s Part bias on `macro` now.
	[[nodiscard]] int32_t corePlaysBias(int32_t part, Macro macro) const {
		return driver.playing()->recipe().bias.of(part, macro);
	}
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

		it("held across a transition's start step, plays on in the incoming instance; the release springs to the knob's "
		   "centre", _ {
			TransitionPiece t;
			t.runToStepLine(3, -1);
			t.steering.holdMacro(Macro::Energy, kHeld);
			t.confirm(TransitionStyle::Cut);
			const Generator* outgoing = t.driver.playing();
			t.decideBefore(6);
			expect(outgoing->recipe().macros[Macro::Energy]).to_equal(kHeld);
			expect(t.hook.runUntil([&] { return t.sink.starts > 0; })).to_be_true();
			const Generator* incoming = t.driver.playing();
			expect(incoming != outgoing).to_be_true();
			// The incoming instance carries the macros the outgoing one played: the steering sends nothing again.
			expect(t.queue.holds({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), kHeld})).to_be_false();
			t.decideBefore(2);
			expect(incoming->recipe().macros[Macro::Energy]).to_equal(kHeld);

			t.steering.setMacro(Macro::Energy, macroPoints(10)); // a knob turn while the fader holds it
			t.decideBefore(4);
			expect(t.currentRecipe.macros[Macro::Energy]).to_equal(macroPoints(10));
			expect(incoming->recipe().macros[Macro::Energy]).to_equal(kHeld);

			t.steering.springMacroBack(Macro::Energy);
			t.decideBefore(6);
			expect(incoming->recipe().macros[Macro::Energy]).to_equal(macroPoints(10));
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

	context("Part bias (spec §3.3, §13.5)", _ {
		it("set by a knob turn, is the Recipe's at once and the core's from the next step; refused past the roster", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.setBias(1, Macro::Energy, macroPoints(6));
			expect(piece.currentRecipe.bias.of(1, Macro::Energy)).to_equal(macroPoints(6));
			piece.decideMore(2);
			expect(piece.corePlaysBias(1, Macro::Energy)).to_equal(macroPoints(6));

			const PartBiases before = piece.currentRecipe.bias;
			piece.steering.setBias(partCount(*piece.currentRecipe.genre), Macro::Energy, macroPoints(6));
			piece.steering.setBias(-1, Macro::Energy, macroPoints(6));
			expect(piece.currentRecipe.bias == before).to_be_true();
		});

		it("held on a fader, plays while held, across a piece start; a knob turn moves only the Recipe's", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.holdBias(1, Macro::Density, -kHeld);
			piece.decideMore(2);
			expect(piece.corePlaysBias(1, Macro::Density)).to_equal(-kHeld);
			expect(piece.currentRecipe.bias.of(1, Macro::Density)).to_equal(0);

			piece.steering.setBias(1, Macro::Density, macroPoints(3)); // a knob turn while the fader holds it
			piece.decideMore(2);
			expect(piece.currentRecipe.bias.of(1, Macro::Density)).to_equal(macroPoints(3));
			expect(piece.corePlaysBias(1, Macro::Density)).to_equal(-kHeld);

			piece.playFromRecipe(true); // still held
			piece.decideBefore(2);
			expect(piece.corePlaysBias(1, Macro::Density)).to_equal(-kHeld);

			piece.steering.springBiasBack(1, Macro::Density); // a hold let go: back to the Recipe's
			piece.decideMore(2);
			expect(piece.corePlaysBias(1, Macro::Density)).to_equal(macroPoints(3));
		});

		it("tapped on a fader, is the Recipe's, and the next start plays it", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.holdBias(2, Macro::Motion, macroPoints(9));
			piece.steering.keepBias(2, Macro::Motion, macroPoints(9));
			piece.decideMore(2);
			expect(piece.currentRecipe.bias.of(2, Macro::Motion)).to_equal(macroPoints(9));
			expect(piece.corePlaysBias(2, Macro::Motion)).to_equal(macroPoints(9));
			piece.playFromRecipe(true);
			expect(piece.corePlaysBias(2, Macro::Motion)).to_equal(macroPoints(9));
		});

		it("held on a part's fader, leaves the Recipe level's centre to the knobs", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.holdBias(1, Macro::Energy, macroPoints(5));
			piece.steering.setMacro(Macro::Energy, macroPoints(30));
			piece.decideMore(2);
			expect(piece.corePlays(Macro::Energy)).to_equal(macroPoints(30));
		});

		it("RESET PART BIAS sets one part's biases to 0, or every part's", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.setBias(1, Macro::Energy, macroPoints(6));
			piece.steering.setBias(1, Macro::Motion, -macroPoints(6));
			piece.steering.setBias(2, Macro::Density, macroPoints(4));
			piece.steering.resetBias(1);
			piece.decideMore(2);
			expect(piece.currentRecipe.bias.any(1)).to_be_false();
			expect(piece.corePlaysBias(1, Macro::Energy)).to_equal(0);
			expect(piece.corePlaysBias(1, Macro::Motion)).to_equal(0);
			expect(piece.corePlaysBias(2, Macro::Density)).to_equal(macroPoints(4));
			piece.steering.resetBias(-1);
			piece.decideMore(2);
			expect(piece.currentRecipe.bias == PartBiases{}).to_be_true();
			expect(piece.driver.playing()->recipe().bias == PartBiases{}).to_be_true();
		});

		it("stopped, is the Recipe's at once, and the next start plays it", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			piece.steering.setBias(1, Macro::Complexity, -macroPoints(8));
			piece.playFromRecipe(true);
			expect(piece.corePlaysBias(1, Macro::Complexity)).to_equal(-macroPoints(8));
		});
	});

	context("octave shifts (spec §7.3, §13.4, §13.6)", _ {
		constexpr uint8_t kBass = 1;
		it("steps on from a press the core hasn't drained, lands on the bar line, and the Recipe keeps it", _ {
			Piece piece;
			piece.runToStepLine(kStepsPerBar + 2);
			PressOutcome outcome = PressOutcome::NothingToDo;
			expect(piece.steering.stepOctave(kBass, 1, &outcome)).to_equal(1);
			expect(outcome == PressOutcome::Accepted).to_be_true();
			expect(piece.steering.stepOctave(kBass, 1, &outcome)).to_equal(2);
			expect(piece.steering.octaveAhead(kBass)).to_equal(2);
			// Two octaves is as far as it goes: the view's HIGHEST popup asks octaveAtEdge().
			expect(piece.steering.octaveAtEdge(kBass, 1)).to_be_true();
			expect(piece.steering.octaveAtEdge(kBass, -1)).to_be_false();
			expect(piece.steering.stepOctave(kBass, 1, &outcome)).to_equal(kNoOctaveWaiting);
			expect(outcome == PressOutcome::NothingToDo).to_be_true();
			piece.decideMore(2);
			expect(piece.steering.octave(kBass)).to_equal(0);
			expect(piece.generator.octaveWaiting(kBass)).to_equal(2);
			expect(piece.steering.octaveAhead(kBass)).to_equal(2);
			piece.decideBefore(2 * kStepsPerBar + 1);
			expect(piece.steering.octave(kBass)).to_equal(2);
			expect(static_cast<int32_t>(piece.currentRecipe.octave[kBass])).to_equal(2);
		});

		it("stepped back to the shift that plays, cancels the one waiting", _ {
			Piece piece;
			piece.runToStepLine(kStepsPerBar + 2);
			PressOutcome outcome = PressOutcome::NothingToDo;
			piece.steering.stepOctave(kBass, -1, &outcome);
			piece.decideMore(2);
			expect(piece.generator.octaveWaiting(kBass)).to_equal(-1);
			expect(piece.steering.stepOctave(kBass, 1, &outcome)).to_equal(0);
			expect(piece.steering.octaveAhead(kBass)).to_equal(0);
			piece.decideBefore(2 * kStepsPerBar + 1);
			expect(piece.steering.octave(kBass)).to_equal(0);
			expect(piece.generator.octaveWaiting(kBass)).to_equal(kNoOctaveWaiting);
		});

		it("does nothing on Drums or FX", _ {
			Piece piece;
			piece.runToStepLine(4);
			const auto fx = static_cast<uint8_t>(partCount(*piece.currentRecipe.genre) - 1);
			for (uint8_t part : {uint8_t{0}, fx}) {
				PressOutcome outcome = PressOutcome::Accepted;
				expect(piece.steering.takesOctave(part)).to_be_false();
				expect(piece.steering.octavesOf(part).lowest).to_equal(0);
				expect(piece.steering.octavesOf(part).highest).to_equal(0);
				expect(piece.steering.octaveAtEdge(part, 1)).to_be_false(); // no popup on Drums or FX
				expect(piece.steering.stepOctave(part, 1, &outcome)).to_equal(kNoOctaveWaiting);
				expect(outcome == PressOutcome::NothingToDo).to_be_true();
				expect(piece.steering.pressOctave(part, 1) == PressOutcome::NothingToDo).to_be_true();
			}
			expect(piece.queue.size()).to_equal(0);
		});

		it("stopped, is the Recipe's at once, and the next start plays it", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			piece.queue.clear();
			expect(piece.steering.pressOctave(kBass, -1) == PressOutcome::Accepted).to_be_true();
			expect(static_cast<int32_t>(piece.currentRecipe.octave[kBass])).to_equal(-1);
			expect(piece.steering.octaveAhead(kBass)).to_equal(-1);
			piece.playFromRecipe(true);
			piece.decideBefore(2);
			expect(piece.generator.octave(kBass)).to_equal(-1);
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

	context("punch-ins (spec §8.9, §13.5)", _ {
		it("reach the core on the next undecided step, and only while on", _ {
			Piece piece;
			piece.runToStepLine(4);
			expect(piece.steering.setPunch(PunchIn::HatRoll, Lane::None, true)).to_be_true();
			expect(piece.generator.punchIns().on(PunchIn::HatRoll)).to_be_false(); // waiting in the queue
			piece.decideMore(1);
			expect(piece.generator.punchIns().on(PunchIn::HatRoll)).to_be_true();
			expect(piece.steering.setPunch(PunchIn::HatRoll, Lane::None, false)).to_be_true();
			piece.decideMore(1);
			expect(piece.generator.punchIns().any()).to_be_false();
			expect(piece.steering.punchIns().any()).to_be_false();
		});

		it("still on across a stop, play from the next piece's first step", _ {
			Piece piece;
			piece.runToStepLine(4);
			piece.steering.setPunch(PunchIn::LaneMute, Lane::Kick, true);
			piece.steering.setPunch(PunchIn::Pump, Lane::None, true);
			piece.decideMore(1);
			piece.playFromRecipe(true);
			piece.decideBefore(1);
			expect(piece.generator.punchIns().muted(Lane::Kick)).to_be_true();
			expect(piece.generator.punchIns().on(PunchIn::Pump)).to_be_true();
			// The pump starts on the new piece's step 0.
			expect(piece.generator.punchIns().pumped).to_be_true();
		});

		it("turned on while stopped, play from the next start", _ {
			Piece piece;
			piece.driver.stop(piece.sink);
			expect(piece.steering.setPunch(PunchIn::Flam, Lane::None, true)).to_be_true();
			piece.playFromRecipe(true);
			piece.decideBefore(1);
			expect(piece.generator.punchIns().on(PunchIn::Flam)).to_be_true();
		});

		it("stay as they were when the queue is full, or the command names none", _ {
			Piece piece;
			piece.runToStepLine(4);
			while (piece.push({CommandKind::SetFreeze, 0, kPress})) { // discrete: never coalesced, so it fills
			}
			expect(piece.steering.setPunch(PunchIn::SnareRoll, Lane::None, true)).to_be_false();
			expect(piece.steering.punchIns().any()).to_be_false();
			expect(piece.steering.setPunch(PunchIn::LaneMute, Lane::Riser, true)).to_be_false();
		});

		it("are dropped by a new entry", _ {
			Piece piece;
			piece.steering.setPunch(PunchIn::DoubleKick, Lane::None, true);
			piece.steering.enter();
			expect(piece.steering.punchIns().any()).to_be_false();
		});
	});

	context("a transition's handover (spec §12.9)", _ {
		it("makes the pending Recipe Current, with its seed and takes, and the macros the rule names", _ {
			Piece piece;
			piece.currentRecipe.macros.value[0] = macroPoints(40);
			perform::PendingRecipe pending;
			pending.recipe = acidRecipe();
			pending.recipe.tempo = 13400;
			pending.recipe.macros.value[0] = macroPoints(10);
			pending.seed = 11;
			pending.takes.take[2] = 3;
			// A genre change or Regenerate keeps the live macros
			piece.steering.handOver(pending);
			expect(piece.currentRecipe.tempo).to_equal(13400);
			expect(piece.currentRecipe.macros.value[0]).to_equal(macroPoints(40));
			expect(piece.currentSeed).to_equal(11U);
			expect(static_cast<int32_t>(piece.currentTakes.take[2])).to_equal(3);
			// A LOAD keeps the file's
			pending.macros = perform::HandoverMacros::Pending;
			piece.steering.handOver(pending);
			expect(piece.currentRecipe.macros.value[0]).to_equal(macroPoints(10));
		});

		it("forgets the takes made in the Recipe it replaces", _ {
			Piece piece;
			piece.driver.stop(piece.sink);
			piece.steering.pressTake(0, 5);
			expect(piece.steering.takeRow(0).newest).to_equal(5);
			perform::PendingRecipe pending;
			pending.recipe = acidRecipe();
			pending.takes.take[0] = 1;
			piece.steering.handOver(pending);
			expect(piece.steering.takeRow(0).newest).to_equal(1); // the pending Recipe's take, none made yet
		});
	});
});
// clang-format on
CPPSPEC_SPEC(perform_steering)
