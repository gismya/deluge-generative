// Takes from the CLIP page (spec §6.6, §13.4, §13.5): a take waits for the next phrase line in its part's pending
// slot. Pressing it again, or pressing the take that plays, cancels it, and a cancelled take is never logged.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kAcid = 1;

Command take(uint8_t part, int32_t value) {
	return {CommandKind::SetTake, part, value};
}

} // namespace

// clang-format off
describe takes("Takes", $ {
	const int32_t phrase = genres::techno.phraseSteps();
	const int32_t phraseBars = phrase / kStepsPerBar;

	it("shows the take waiting for its line, then the one playing", _ {
		Result waiting = playSteered(phraseBars - 1, {{3, take(kAcid, 2)}});
		expect(waiting.generator->take(kAcid)).to_equal(0);
		expect(waiting.generator->takeWaiting(kAcid)).to_equal(2);
		Result landed = playSteered(phraseBars + 1, {{3, take(kAcid, 2)}});
		expect(landed.generator->take(kAcid)).to_equal(2);
		expect(landed.generator->takeWaiting(kAcid)).to_equal(-1);
	});

	it("cancels a waiting take when it is pressed again, and logs nothing", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Result result = playSteered(phraseBars + 1, {{3, take(kAcid, 2)}, {5, take(kAcid, 2)}}, &log);
		expect(result.generator->take(kAcid)).to_equal(0);
		expect(result.generator->takeWaiting(kAcid)).to_equal(-1);
		expect(log.size()).to_equal(0);
	});

	it("cancels a waiting take when the take that plays is pressed", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Result result = playSteered(phraseBars + 1, {{3, take(kAcid, 2)}, {5, take(kAcid, 0)}}, &log);
		expect(result.generator->take(kAcid)).to_equal(0);
		expect(result.generator->takeWaiting(kAcid)).to_equal(-1);
		expect(log.size()).to_equal(0);
	});

	it("replaces a waiting take with another", _ {
		Result result = playSteered(phraseBars + 1, {{3, take(kAcid, 2)}, {5, take(kAcid, 3)}});
		expect(result.generator->take(kAcid)).to_equal(3);
	});

	it("starts each piece on the takes it was given", _ {
		Run run;
		run.recipe = technoRecipe();
		run.takes.take[kAcid] = 4;
		Result result = play(run, 1);
		expect(result.generator->take(kAcid)).to_equal(4);
	});
});

CPPSPEC_SPEC(takes)
