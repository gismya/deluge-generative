// The CLIP page (spec §6.6, §13.5): the last part focused's takes on row 1, FREEZE, NEW SOUND and SOUND EDITOR on
// row 2 and its six sound controls as faders on rows 3–8. Composed from a stepped piece as the Perform view composes
// it, and described as the test port's gen_clip and gen_faders texts from the same cells. Also the Y turn through
// the takes, and the seed code Y push shows (spec §6.5).

#include "generative/perform/clip_page.h"
#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <string>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

using Look = ClipCell::Look;
constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kAcid = 1;
constexpr int32_t kBrightnessRow = 2; // spec row 3
constexpr int32_t kWobbleRow = 7;     // spec row 8

std::string clipText(const ClipPage& page) {
	char out[kClipTextChars + 1];
	describeClip(page, out);
	return out;
}
std::string fadersText(const ClipPage& page) {
	char out[kClipTextChars + 1];
	describeFaders(page, out);
	return out;
}
ClipPage compose(const ClipPageInput& in) {
	ClipPage out;
	composeClipPage(in, out);
	return out;
}

/// The takes row of a page showing `takes`, as gen_clip's letters.
std::string takes(const TakeRow& row) {
	ClipPageInput in;
	in.part = 0;
	in.parts = 3;
	in.takes = row;
	return clipText(compose(in)).substr(std::string("part=0 takes=").size(), kMaxTakes);
}

/// A fader row's pads as letters: L the live value's pad, h a pad lit up to home, . one above it.
std::string fader(const ClipPage& page, int32_t row) {
	std::string out;
	for (const ClipCell& cell : page.cell[row]) {
		out += cell.look == Look::FaderLive ? 'L' : cell.look == Look::Off ? ' ' : cell.lit ? 'h' : '.';
	}
	return out;
}

/// A page showing part 0 with every control's home and live value at `home` and `live`.
ClipPageInput withLevels(int32_t home, int32_t live) {
	ClipPageInput in;
	in.part = 0;
	in.parts = 3;
	for (ControlLevel& level : in.levels) {
		level = {.has = true, .home = static_cast<int8_t>(home), .live = static_cast<int8_t>(live)};
	}
	return in;
}

std::string code(uint32_t seed) {
	char out[kSeedCodeChars + 1];
	seedCode(seed, out);
	return out;
}

/// A Techno piece played step by step, with the CLIP page composed for `part` as the Perform view does. The view
/// reads the sound swap and the controls from the firmware; here they are the spec's to set.
struct Piece : StepwisePiece<> {
	int32_t part = kAcid;
	bool loading = false;
	ControlLevel levels[kNumSoundControls];
	/// The highest take each part has played, as the Current Recipe keeps it.
	int32_t newest[kMaxParts] = {};

	explicit Piece(const Takes& startTakes = {}) : StepwisePiece({.seed = 3, .takes = startTakes, .skipIntro = true}) {
		for (int32_t p = 0; p < kMaxParts; p++) {
			newest[p] = startTakes.take[p];
		}
	}
	ClipPageInput input() {
		ClipPageInput in;
		in.part = part;
		in.parts = partCount(genres::techno);
		if (part >= 0 && part < in.parts) {
			newest[part] = std::max(newest[part], generator.take(part));
			in.takes = {
			    .playing = generator.take(part), .waiting = generator.takeWaiting(part), .newest = newest[part]};
			in.frozen = generator.partFrozen(part);
			in.sound = loading                             ? SoundSwap::Loading
			           : generator.soundWaiting(part) >= 0 ? SoundSwap::Waiting
			                                               : SoundSwap::None;
		}
		std::copy_n(levels, kNumSoundControls, in.levels);
		return in;
	}
	ClipPage page() { return compose(input()); }
};

} // namespace

// clang-format off
describe clip_page("The CLIP page", $ {
	it("is dark before any part is focused, and for a part past the roster", _ {
		Piece piece;
		piece.decideBefore(4);
		for (int32_t part : {-1, 3}) {
			piece.part = part;
			const ClipPage page = piece.page();
			for (const auto& row : page.cell) {
				for (const ClipCell& cell : row) {
					expect(cell.look).to_equal(Look::Off);
				}
			}
		}
		piece.part = -1;
		expect(clipText(piece.page())).to_equal(std::string("part=-1 takes=................ freeze=0 sound=-"));
		expect(fadersText(piece.page())).to_equal(std::string("part=-1 at_home - - - - - -"));
	});

	it("lays out row 2: FREEZE, NEW SOUND and SOUND EDITOR from the left, the rest dark", _ {
		Piece piece;
		piece.decideBefore(4);
		const ClipPage page = piece.page();
		expect(page.cell[kActionsRow][kFreezePad].look).to_equal(Look::Freeze);
		expect(page.cell[kActionsRow][kNewSoundPad].look).to_equal(Look::NewSound);
		expect(page.cell[kActionsRow][kSoundEditorPad].look).to_equal(Look::SoundEditor);
		for (int32_t x = 3; x < ClipPage::kColumns; x++) {
			expect(page.cell[kActionsRow][x].look).to_equal(Look::Off);
		}
	});

	it("lights the take playing and shows the next new take grey", _ {
		Piece piece;
		piece.decideBefore(4);
		const ClipPage page = piece.page();
		expect(page.cell[kTakesRow][0].look).to_equal(Look::TakePlaying);
		expect(page.cell[kTakesRow][1].look).to_equal(Look::TakeNew);
		expect(page.cell[kTakesRow][2].look).to_equal(Look::Off);
		expect(clipText(page)).to_equal(std::string("part=1 takes=Pn.............. freeze=0 sound=-"));
	});

	it("blinks a take waiting for its phrase line, then plays it with the earlier takes dim", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::SetTake, kAcid, 2});
		ClipPage page = piece.page();
		expect(page.cell[kTakesRow][0].look).to_equal(Look::TakePlaying);
		expect(page.cell[kTakesRow][2].look).to_equal(Look::TakeQueued);
		// The waiting take counts among the takes made: take 2 is an earlier one, take 4 the next new one.
		expect(clipText(page)).to_equal(std::string("part=1 takes=Peqn............ freeze=0 sound=-"));
		piece.part = 0;
		expect(clipText(piece.page())).to_equal(std::string("part=0 takes=Pn.............. freeze=0 sound=-"));
		piece.part = kAcid;
		piece.decideBefore(kPhraseSteps + 1);
		page = piece.page();
		expect(page.cell[kTakesRow][2].look).to_equal(Look::TakePlaying);
		expect(page.cell[kTakesRow][0].look).to_equal(Look::TakeEarlier);
		expect(clipText(page)).to_equal(std::string("part=1 takes=eePn............ freeze=0 sound=-"));
	});

	it("keeps the takes made lit when an earlier one plays again", _ {
		expect(takes({.playing = 1, .waiting = -1, .newest = 3})).to_equal(std::string("ePeen..........."));
	});

	it("shows all 16 takes, with no new take past the sixteenth", _ {
		Takes sixteenth;
		sixteenth.take[kAcid] = 15;
		Piece piece(sixteenth);
		piece.decideBefore(4);
		const ClipPage page = piece.page();
		expect(page.cell[kTakesRow][15].look).to_equal(Look::TakePlaying);
		expect(clipText(page)).to_equal(std::string("part=1 takes=eeeeeeeeeeeeeeeP freeze=0 sound=-"));
	});

	it("lets a tap queue any lit take, and not past the next new one", _ {
		const TakeRow row{.playing = 1, .waiting = -1, .newest = 3};
		expect(takeTappable(row, 0)).to_be_true();
		expect(takeTappable(row, 4)).to_be_true();
		expect(takeTappable(row, 5)).to_be_false();
	});

	it("lights FREEZE while the part is frozen", _ {
		Piece piece;
		piece.decideBefore(4);
		expect(piece.page().cell[kActionsRow][kFreezePad].lit).to_be_false();
		piece.pushAndDecide({CommandKind::SetFreeze, kAcid, kPress});
		expect(piece.page().cell[kActionsRow][kFreezePad].lit).to_be_true();
		expect(clipText(piece.page())).to_equal(std::string("part=1 takes=Pn.............. freeze=1 sound=-"));
		piece.part = 0; // another part isn't
		expect(piece.page().cell[kActionsRow][kFreezePad].lit).to_be_false();
	});

	it("blinks NEW SOUND while the sound loads, and while it waits for its phrase line", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.loading = true;
		expect(piece.page().cell[kActionsRow][kNewSoundPad].look).to_equal(Look::SoundLoading);
		expect(clipText(piece.page())).to_equal(std::string("part=1 takes=Pn.............. freeze=0 sound=p"));
		piece.loading = false;
		piece.pushAndDecide({CommandKind::SetSound, kAcid, 9});
		expect(piece.page().cell[kActionsRow][kNewSoundPad].look).to_equal(Look::SoundWaiting);
		expect(clipText(piece.page())).to_equal(std::string("part=1 takes=Pn.............. freeze=0 sound=w"));
		piece.decideBefore(kPhraseSteps + 1); // it landed
		expect(piece.page().cell[kActionsRow][kNewSoundPad].look).to_equal(Look::NewSound);
		expect(clipText(piece.page())).to_equal(std::string("part=1 takes=Pn.............. freeze=0 sound=-"));
	});

	it("holds the controls in the spec's order from row 3 down", _ {
		expect(faderControl(2) == SoundControl::Brightness).to_be_true();
		expect(faderControl(3) == SoundControl::Thin).to_be_true();
		expect(faderControl(4) == SoundControl::Decay).to_be_true();
		expect(faderControl(5) == SoundControl::Space).to_be_true();
		expect(faderControl(6) == SoundControl::Drive).to_be_true();
		expect(faderControl(7) == SoundControl::Wobble).to_be_true();
	});

	it("lights a fader up to home, with the live value's pad white where the Arc has it", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.levels[static_cast<int32_t>(SoundControl::Brightness)] = {.has = true, .home = -24, .live = 16};
		piece.levels[static_cast<int32_t>(SoundControl::Wobble)] = {.has = true, .home = 16, .live = -40};
		const ClipPage page = piece.page();
		expect(fader(page, kBrightnessRow)).to_equal(std::string("hhhhhh...L......"));
		expect(fader(page, kWobbleRow)).to_equal(std::string("hhhLhhhhhh......"));
		// A control the part's preset doesn't use is dark.
		expect(fader(page, kBrightnessRow + 1)).to_equal(std::string(16, ' '));
		expect(fadersText(page)).to_equal(std::string("part=1 6/10 - - - - 10/4"));
	});

	it("shows the live value white on home's pad too, and says at_home when every fader's is", _ {
		const ClipPage page = compose(withLevels(-24, -24));
		for (int32_t row = kBrightnessRow; row <= kWobbleRow; row++) {
			expect(fader(page, row)).to_equal(std::string("hhhhhL.........."));
		}
		expect(page.cell[kBrightnessRow][5].lit).to_be_true();
		expect(fadersText(page)).to_equal(std::string("part=0 at_home 6= 6= 6= 6= 6= 6="));
		// A live value that moved within home's pad is still at home.
		expect(fadersText(compose(withLevels(-24, -22)))).to_equal(std::string("part=0 at_home 6= 6= 6= 6= 6= 6="));
		expect(fadersText(compose(withLevels(-24, -8)))).to_equal(std::string("part=0 6/8 6/8 6/8 6/8 6/8 6/8"));
	});

	it("spans a param's whole knob range, pad 1 the bottom and pad 16 the top", _ {
		expect(faderKnobPos(0)).to_equal(-64);
		expect(faderKnobPos(15)).to_equal(64);
		expect(fader(compose(withLevels(-64, -64)), kBrightnessRow)).to_equal(std::string("L..............."));
		expect(fader(compose(withLevels(64, 64)), kBrightnessRow)).to_equal(std::string("hhhhhhhhhhhhhhhL"));
		expect(fader(compose(withLevels(-60, 60)), kBrightnessRow)).to_equal(std::string("h..............L"));
		// A knob's middle rounds up.
		expect(fader(compose(withLevels(0, 0)), kBrightnessRow)).to_equal(std::string("hhhhhhhhL......."));
	});

	it("shows a tapped pad back on that pad", _ {
		for (int32_t pad = 0; pad < kFaderPads; pad++) {
			const int32_t tapped = faderKnobPos(pad);
			const ClipPage page = compose(withLevels(tapped, tapped));
			expect(page.cell[kBrightnessRow][pad].look).to_equal(Look::FaderLive);
		}
	});

	it("fits its longest texts", _ {
		ClipPageInput in = withLevels(64, -64);
		in.part = 5;
		in.parts = 6;
		in.takes = {.playing = 15, .waiting = -1, .newest = 15};
		in.frozen = true;
		in.sound = SoundSwap::Waiting;
		const ClipPage page = compose(in);
		expect(clipText(page)).to_equal(std::string("part=5 takes=eeeeeeeeeeeeeeeP freeze=1 sound=w"));
		expect(fadersText(page)).to_equal(std::string("part=5 16/1 16/1 16/1 16/1 16/1 16/1"));
		expect(fadersText(compose(withLevels(64, 50)))).to_equal(std::string("part=0 16/14 16/14 16/14 16/14 16/14 16/14"));
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
		expect(takes({.playing = 4, .waiting = 5, .newest = 4})).to_equal(std::string("eeeePqn........."));
	});

	it("goes nowhere before the first take or past the sixteenth", _ {
		expect(steppedTake({.playing = 0, .waiting = -1, .newest = 3}, -1)).to_equal(-1);
		expect(steppedTake({.playing = 15, .waiting = -1, .newest = 15}, 1)).to_equal(-1);
	});

	it("labels the PART page's take: the one playing of the takes made, starred when frozen", _ {
		char out[kTakeLabelChars + 1];
		takeLabel({.playing = 1, .waiting = -1, .newest = 2}, false, out);
		expect(std::string(out)).to_equal(std::string("T2/3"));
		takeLabel({.playing = 0, .waiting = 4, .newest = 0}, true, out);
		expect(std::string(out)).to_equal(std::string("T1/5*"));
		takeLabel({.playing = 15, .waiting = -1, .newest = 15}, true, out);
		expect(std::string(out)).to_equal(std::string("T16/16*"));
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
