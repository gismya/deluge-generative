// The CLIP page's takes row and Y turn (spec §6.6, §13.5), and the seed code Y push shows (spec §6.5).

#include "generative/perform/clip_page.h"

#include "cppspec.hpp"

#include <string>

using namespace deluge::generative;
using namespace deluge::generative::perform;

namespace {

std::string row(const TakeRow& takes) {
	std::string out;
	for (int32_t t = 0; t < kMaxTakes; t++) {
		switch (takeLook(takes, t)) {
		case TakeLook::Off:
			out += '.';
			break;
		case TakeLook::Playing:
			out += 'P';
			break;
		case TakeLook::Earlier:
			out += 'e';
			break;
		case TakeLook::New:
			out += 'n';
			break;
		case TakeLook::Queued:
			out += 'q';
			break;
		}
	}
	return out;
}

std::string code(uint32_t seed) {
	char out[kSeedCodeChars + 1];
	seedCode(seed, out);
	return out;
}

} // namespace

// clang-format off
describe clip_page("The CLIP page", $ {
	it("lights the take playing, the earlier takes dim and the next new take grey", _ {
		expect(row({.playing = 0, .waiting = -1, .newest = 0})).to_equal(std::string("Pn.............."));
		expect(row({.playing = 1, .waiting = -1, .newest = 3})).to_equal(std::string("ePeen..........."));
	});

	it("blinks the waiting take, and counts it among the takes made", _ {
		expect(row({.playing = 0, .waiting = 2, .newest = 1})).to_equal(std::string("Peqn............"));
	});

	it("has no new take past the sixteenth", _ {
		expect(row({.playing = 15, .waiting = -1, .newest = 15})).to_equal(std::string("eeeeeeeeeeeeeeeP"));
	});

	it("lets a tap queue any lit take, and not past the next new one", _ {
		const TakeRow takes{.playing = 1, .waiting = -1, .newest = 3};
		expect(takeTappable(takes, 0)).to_be_true();
		expect(takeTappable(takes, 4)).to_be_true();
		expect(takeTappable(takes, 5)).to_be_false();
	});

	it("steps a Y turn from the waiting take, or the one playing", _ {
		expect(steppedTake({.playing = 2, .waiting = -1, .newest = 4}, -1)).to_equal(1);
		expect(steppedTake({.playing = 2, .waiting = -1, .newest = 4}, 1)).to_equal(3);
		expect(steppedTake({.playing = 2, .waiting = 3, .newest = 4}, 1)).to_equal(4);
		expect(steppedTake({.playing = 2, .waiting = 3, .newest = 4}, -1)).to_equal(2);
	});

	it("makes a new take when stepping forward past the newest", _ {
		expect(steppedTake({.playing = 0, .waiting = -1, .newest = 0}, 1)).to_equal(1);
		expect(steppedTake({.playing = 4, .waiting = -1, .newest = 4}, 1)).to_equal(5);
		expect(takeLook({.playing = 4, .waiting = 5, .newest = 4}, 5) == TakeLook::Queued).to_be_true();
	});

	it("goes nowhere before the first take or past the sixteenth", _ {
		expect(steppedTake({.playing = 0, .waiting = -1, .newest = 3}, -1)).to_equal(-1);
		expect(steppedTake({.playing = 15, .waiting = -1, .newest = 15}, 1)).to_equal(-1);
	});

	it("writes a seed as 7 characters QwertyUI accepts, most significant first", _ {
		expect(code(0)).to_equal(std::string("0000000"));
		expect(code(31)).to_equal(std::string("000000Z"));
		expect(code(32)).to_equal(std::string("0000010"));
		expect(code(0xFFFFFFFFU)).to_equal(std::string("3ZZZZZZ"));
		// Crockford's alphabet: no I, L, O or U, which read as 1, 1, 0 and V on the OLED.
		expect(code(18)).to_equal(std::string("000000J"));
	});
});

CPPSPEC_SPEC(clip_page)
