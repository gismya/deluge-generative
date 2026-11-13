// Home edits as driver-side steering (spec §2.6, §11.6, §17.2): the Perform driver moves a sound control's home at
// once, and the edit reaches the core only to be logged on the next undecided step. The core passes it through.

#include "harness.h"

#include "cppspec.hpp"

using namespace harness;

namespace {

/// A home edit on `part`'s `lane` for the param the driver keys as `param`.
Command home(uint8_t part, Lane lane, uint16_t param, int32_t value) {
	return {CommandKind::SetHome, part, value, param, lane};
}

} // namespace

// clang-format off
describe home_edits("Home edits", $ {
	it("are logged on the next undecided step, one per param, keeping each param's latest home", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true, .logEntries = 8}};
		piece.runTicks(0);
		piece.push(home(1, Lane::None, 7, 100));
		piece.push(home(1, Lane::None, 9, 200)); // another param on the same part
		piece.push(home(2, Lane::Riser, 7, 300)); // an FX lane's row
		piece.push(home(1, Lane::None, 7, 150)); // coalesces with the first
		expect(piece.queue.size()).to_equal(3);
		const int32_t landing = piece.generator.nextStep();
		piece.runSteps(1);
		expect(piece.log.size()).to_equal(3);
		for (int32_t i = 0; i < piece.log.size(); i++) {
			expect(piece.log.at(i).step).to_equal(landing);
		}
		expect(piece.log.at(0).command == home(1, Lane::None, 7, 150)).to_be_true();
		expect(piece.log.at(1).command == home(1, Lane::None, 9, 200)).to_be_true();
		expect(piece.log.at(2).command == home(2, Lane::Riser, 7, 300)).to_be_true();
	});

	it("change nothing in the event stream", _ {
		Run plain;
		plain.recipe = technoRecipe();
		plain.seed = 3;
		plain.skipIntro = true;
		Run steered = plain;
		steered.steer = at(40, home(1, Lane::None, 7, 12345));
		expect(play(steered, 8).events == play(plain, 8).events).to_be_true();
	});
});

CPPSPEC_SPEC(home_edits)
