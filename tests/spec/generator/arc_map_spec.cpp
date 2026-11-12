// The SONG page's Arc map and the ARC OLED page (spec §13.5): one column per phrase, column 4 now, the played ones from
// the driver's history and the planned ones from the Arc plan, queued actions blinking where they land. And what the
// page keeps between frames (ArcMapState): the plan and when it is worked out again, the X scroll, the flash window.

#include "generative/perform/arc_map.h"
#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <string>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kDrumsRow = 0;                   // spec row 1, the top row
constexpr int32_t kAcidRow = 1;
constexpr int32_t kEnergyRow = 6;  // spec row 7
constexpr int32_t kSectionRow = 7; // spec row 8, the bottom row

/// A Techno piece from SHIFT+PLAY, played step by step, with the Arc map composed as the SONG page does: the state
/// re-plans and follows the clock in every frame, then the map composes from it.
struct Piece : StepwisePiece<> {
	ArcMapState state;
	int32_t focus = -1;

	Piece() : StepwisePiece({.seed = 3, .skipIntro = true}) {}
	ArcMapInput input() {
		ArcMapInput in;
		in.piece = driver.playing();
		in.clock = std::max<int32_t>(driver.clock(), 0);
		in.focus = static_cast<int8_t>(focus);
		in.history = &driver.history();
		return in;
	}
	/// A frame: what the view does before anything reads the map. @return the input it used.
	ArcMapInput frame() {
		const ArcMapInput in = input();
		state.refreshPlan(in.piece, 0);
		state.sampleFlash(in.clock);
		state.clampScroll(in);
		return in;
	}
	ArcMap map() {
		ArcMap out;
		composeArcMap(frame(), state, out);
		return out;
	}
	ArcMapRange range() { return state.scrollRange(frame()); }
	/// X turned by `offset` columns.
	bool scroll(int32_t offset) { return state.scrollBy(frame(), offset); }
	ArcPage page() {
		ArcPage out;
		composeArcPage(frame(), state, out);
		return out;
	}
	std::string sections() {
		char text[kArcMapTextChars + 1];
		describeArcMapSections(map(), text);
		return text;
	}
	std::string part(int32_t row) {
		char text[ArcMap::kColumns + 1];
		describeArcMapPart(map(), row, text);
		return text;
	}
	/// Whether `part` plays a note-on from tick `from` up to but not including `to`, in what has played so far.
	bool playsIn(int32_t part, int32_t from, int32_t to) const {
		return std::any_of(sink.events.begin(), sink.events.end(), [&](const Event& event) {
			return event.kind == EventKind::NoteOn && event.part == part && event.tick >= from && event.tick < to;
		});
	}
};

constexpr int32_t column(int32_t phrasesFromNow) {
	return ArcMap::kNowColumn + phrasesFromNow;
}

} // namespace

// clang-format off
describe arc_map("Arc map", $ {
	it("has now in column 4, what played to its left and the plan to its right", _ {
		Piece piece;
		piece.decideBefore(2 * kPhraseSteps + 20);
		ArcMap map = piece.map();
		expect(ArcMap::kNowColumn).to_equal(3);
		expect(map.cell[kSectionRow][column(0)].when).to_equal(ArcMapCell::When::Now);
		expect(map.cell[kSectionRow][column(-1)].when).to_equal(ArcMapCell::When::Played);
		expect(map.cell[kSectionRow][column(1)].when).to_equal(ArcMapCell::When::Planned);
		expect(map.cell[kSectionRow][column(0)].kind).to_equal(SectionKind::Groove);
		// Phrases before step 0 never played.
		expect(map.cell[kSectionRow][column(-2)].look).to_equal(ArcMapCell::Look::Section);
		expect(map.cell[kSectionRow][column(-3)].look).to_equal(ArcMapCell::Look::Off);
		// As text: the plan's Break, Build and Drop ahead, nothing queued.
		expect(piece.sections()).to_equal(std::string{"s=.GGGGGGGBBUDGGGG q=................"});
	});

	it("shows the parts from the top row down: sounded behind, planned ahead", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 20);
		ArcMap map = piece.map();
		expect(map.cell[kDrumsRow][column(-1)].look).to_equal(ArcMapCell::Look::Part);
		expect(map.cell[kDrumsRow][column(-1)].when).to_equal(ArcMapCell::When::Played);
		expect(map.cell[kAcidRow][column(2)].look).to_equal(ArcMapCell::Look::Part);
		expect(map.cell[kAcidRow][column(2)].when).to_equal(ArcMapCell::When::Planned);
		expect(map.cell[kAcidRow][column(2)].part).to_equal(1);
		// Techno has three parts: rows 4–6 stay dark.
		expect(map.cell[3][column(2)].look).to_equal(ArcMapCell::Look::Off);
	});

	it("flashes a part in the now column on the step it plays, and shows the focused part white", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 1);
		// The Kick plays on the bar line: on its step the drums' now pad is lit as a hit.
		piece.hook.runUntil([&] { return piece.driver.clock() >= stepTick(kPhraseSteps); });
		expect(piece.driver.clock()).to_equal(stepTick(kPhraseSteps)); // the hook is called on the step it plays
		expect(piece.map().cell[kDrumsRow][column(0)].hit).to_be_true();
		expect(piece.part(kDrumsRow).substr(0, 5)).to_equal(std::string{"..hnp"});
		piece.focus = kDrumsRow;
		expect(piece.map().cell[kDrumsRow][column(0)].look).to_equal(ArcMapCell::Look::Focus);
		expect(piece.part(kDrumsRow).substr(0, 5)).to_equal(std::string{"..hfp"});
	});

	it("flashes a hit for two 16ths: on the step the part plays and the one after (spec §13.5)", _ {
		// A step in the second phrase where ACID plays on the grid line, with nothing in the two steps before it or
		// the two after: found in a first pass, then played again frame by frame.
		int32_t step = -1;
		{
			Piece scout;
			scout.runToStepLine(2 * kPhraseSteps);
			for (int32_t s = kPhraseSteps + 2; s < 2 * kPhraseSteps - 3 && step < 0; s++) {
				const bool alone = scout.playsIn(kAcidRow, stepTick(s), stepTick(s) + 1)
				                   && !scout.playsIn(kAcidRow, stepTick(s - 2), stepTick(s))
				                   && !scout.playsIn(kAcidRow, stepTick(s) + 1, stepTick(s + 3));
				step = alone ? s : step;
			}
		}
		expect(step > 0).to_be_true();
		Piece piece;
		const auto hit = [&](int32_t at) {
			piece.runToStepLine(at); // the first frame of the step, as the view samples the window
			return piece.map().cell[kAcidRow][column(0)].hit;
		};
		expect(hit(step - 1)).to_be_false();
		expect(hit(step)).to_be_true();
		expect(hit(step + 1)).to_be_true();
		expect(hit(step + 2)).to_be_false();
	});

	it("shows Energy against the centre: below it in a Break", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcMap map = piece.map();
		expect(map.cell[kEnergyRow][column(0)].look).to_equal(ArcMapCell::Look::Energy);
		expect(map.cell[kEnergyRow][column(0)].energy).to_equal(0); // Groove: at the centre
		expect(map.cell[kEnergyRow][column(1)].energy < -1).to_be_true();
	});

	it("blinks a queued force in the Section row of the column where it lands, and the plan shows its result", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 40);
		expect(piece.map().cell[kSectionRow][column(1)].blink).to_be_false();
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcMap map = piece.map();
		expect(map.cell[kSectionRow][column(1)].blink).to_be_true();
		expect(map.cell[kSectionRow][column(1)].kind).to_equal(SectionKind::Break);
		expect(map.cell[kSectionRow][column(2)].blink).to_be_false();
		expect(map.cell[kAcidRow][column(1)].look).to_equal(ArcMapCell::Look::Off); // the Break keeps no ACID
	});

	it("blinks a queued mute in its part's row where it lands, then shows the part muted", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		piece.pushAndDecide({CommandKind::SetMute, 1, kPress});
		ArcMap map = piece.map();
		expect(map.cell[kAcidRow][column(0)].blink).to_be_true(); // it lands on this phrase's next bar line
		expect(map.cell[kAcidRow][column(1)].blink).to_be_false();
		expect(map.cell[kAcidRow][column(1)].look).to_equal(ArcMapCell::Look::Muted);
	});

	it("blinks a queued sound re-roll in its part's row where it lands", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		piece.pushAndDecide({CommandKind::SetSound, 1, 9});
		ArcMap map = piece.map();
		expect(map.cell[kAcidRow][column(0)].blink).to_be_false();
		expect(map.cell[kAcidRow][column(1)].blink).to_be_true(); // the next phrase line
	});

	it("scrolls with X: back over what played, forward over the plan, no further than either", _ {
		Piece piece;
		piece.decideBefore(6 * kPhraseSteps + 5);
		const ArcMapRange range = piece.range();
		expect(range.back).to_equal(-3); // phrases 0–2 lie left of the three played columns
		expect(range.ahead).to_equal(ArcPlan::kMaxPhrases - 1 - (ArcMap::kColumns - 1 - ArcMap::kNowColumn));
		// A turn further than the range stops at its end, and a turn at the end moves nothing.
		expect(piece.scroll(-100)).to_be_true();
		expect(piece.state.scroll()).to_equal(range.back);
		expect(piece.scroll(-1)).to_be_false();
		ArcMap back = piece.map();
		expect(back.cell[kSectionRow][0].when).to_equal(ArcMapCell::When::Played);
		expect(back.cell[kSectionRow][column(3)].when).to_equal(ArcMapCell::When::Now);
		expect(piece.scroll(100)).to_be_true();
		expect(piece.state.scroll()).to_equal(range.ahead);
		ArcMap ahead = piece.map();
		expect(ahead.cell[kSectionRow][ArcMap::kColumns - 1].look).to_equal(ArcMapCell::Look::Section);
		// X tapped: back to now.
		expect(piece.state.scrollToNow()).to_be_true();
		expect(piece.state.scrollToNow()).to_be_false();
		expect(piece.map().cell[kSectionRow][column(0)].when).to_equal(ArcMapCell::When::Now);
	});

	it("keeps the scroll inside the plan when a queued End makes it shorter", _ {
		Piece piece;
		piece.decideBefore(6 * kPhraseSteps + 5);
		piece.scroll(100);
		expect(piece.state.scroll() > 0).to_be_true();
		piece.pushAndDecide({CommandKind::End, 0, 1});
		// The next frame: the plan ends after the Outro, and the scroll comes back with it.
		const ArcMapInput in = piece.input();
		expect(piece.state.refreshPlan(in.piece, 0)).to_be_true();
		expect(piece.state.clampScroll(in)).to_be_true();
		expect(piece.state.scroll()).to_equal(0);
		expect(piece.state.clampScroll(in)).to_be_false();
	});

	it("works the plan out again only when a phrase line passes or the steering changes", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true();
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_false();
		piece.decideBefore(kPhraseSteps + 40); // the same phrase
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_false();
		piece.pushAndDecide({CommandKind::SetMute, 1, kPress}); // a mute waits for its bar line
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true();
		expect(static_cast<int32_t>(piece.state.queued().size())).to_equal(1);
		piece.decideBefore(2 * kPhraseSteps + 1); // the next phrase on the decided side
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true();
		expect(piece.state.refreshPlan(piece.driver.playing(), 1)).to_be_true(); // a new roster
		piece.state.forgetPlan();
		expect(piece.state.refreshPlan(piece.driver.playing(), 1)).to_be_true();
		// Stopped: no plan and nothing queued.
		expect(piece.state.refreshPlan(nullptr, 1)).to_be_false();
		expect(piece.state.plan().count).to_equal(0);
		expect(static_cast<int32_t>(piece.state.queued().size())).to_equal(0);
	});

	it("keeps column 1 lit when scrolled fully back, while the next phrase is already being decided", _ {
		Piece piece;
		// Phrase 16 is heard; phrase 17's first step is decided, 12 ticks before it is heard.
		piece.decideBefore(17 * kPhraseSteps + 1);
		expect(piece.driver.clock() < stepTick(17 * kPhraseSteps)).to_be_true();
		const ArcMapRange range = piece.range();
		expect(range.back < 0).to_be_true();
		piece.scroll(range.back);
		ArcMap back = piece.map();
		expect(back.cell[kSectionRow][0].look).to_equal(ArcMapCell::Look::Section);
		expect(back.cell[kSectionRow][0].when).to_equal(ArcMapCell::When::Played);
	});

	it("shows nothing after the line where a queued End stops the piece", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::End, 0, 1});
		ArcMap map = piece.map();
		expect(map.cell[kSectionRow][column(1)].kind).to_equal(SectionKind::Outro);
		expect(map.cell[kSectionRow][column(1)].blink).to_be_true();
		expect(map.cell[kSectionRow][column(2)].look).to_equal(ArcMapCell::Look::Off);
		expect(map.cell[kDrumsRow][column(2)].look).to_equal(ArcMapCell::Look::Off);
	});

	it("ARC page: boxes now and the next 7 phrases, the queued ones blinking, and names the Arc's next change", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		const ArcPage page = piece.page();
		expect(std::string{page.box[0].tag}).to_equal(std::string{"GR"});
		expect(page.box[0].now).to_be_true();
		expect(std::string{page.box[1].tag}).to_equal(std::string{"BR"});
		expect(page.box[1].blink).to_be_true();
		expect(page.box[2].blink).to_be_false();
		// The Break lands on step 256, 88 steps (5.5 bars) after step 168.
		expect(std::string{page.next}).to_equal(std::string{"NEXT BREAK 6 BARS"});
	});

	it("ARC page: marks a Drop, and says STOP for the line where the Outro ends", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
		ArcPage page = piece.page();
		int32_t drops = 0;
		for (const ArcBox& box : page.box) {
			drops += box.drop ? 1 : 0;
		}
		expect(drops > 0).to_be_true();

		Piece ending;
		ending.decideBefore(kPhraseSteps + 40);
		ending.pushAndDecide({CommandKind::End, 0, 1});
		page = ending.page();
		expect(std::string{page.next}).to_equal(std::string{"NEXT OUTRO 6 BARS"});
		expect(page.box[2].ended).to_be_true();
	});
});

CPPSPEC_SPEC(arc_map)
