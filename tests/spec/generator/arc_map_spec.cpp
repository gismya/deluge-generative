// The SONG page's Arc map and the ARC OLED page (spec §13.5): one column per phrase, column 4 now, the played ones from
// the driver's history and the planned ones from the Arc plan, queued actions blinking where they land.

#include "generative/perform/arc_map.h"
#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <span>
#include <string>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kDrumsRow = 0;                   // spec row 1, the top row
constexpr int32_t kAcidRow = 1;
constexpr int32_t kEnergyRow = 6;  // spec row 7
constexpr int32_t kSectionRow = 7; // spec row 8, the bottom row

/// A Techno piece from SHIFT+PLAY, played step by step, with the Arc map composed as the SONG page would.
struct Piece {
	Generator generator;
	SteeringQueue queue;
	RecordingSink sink;
	PerformDriver driver;
	ArcPlan plan;
	QueuedAction actions[PendingList::kCapacity];

	Piece() {
		generator.start(technoRecipe(), 3, {}, true);
		driver.begin(generator);
	}
	void decideTo(int32_t step) {
		while (generator.nextStep() < step && !driver.ended()) {
			driver.advance(1, kGeneratorTickMagnitude, queue, nullptr, sink);
		}
	}
	void steer(const Command& command) {
		queue.push(command);
		decideTo(generator.nextStep() + 1);
	}
	ArcMapInput input(int32_t scroll = 0, int32_t focus = -1) {
		generator.planArc(plan);
		ArcMapInput in;
		in.playing = true;
		in.clock = driver.clock();
		in.phraseSteps = kPhraseSteps;
		in.parts = partCount(genres::techno);
		in.scroll = scroll;
		in.focus = static_cast<int8_t>(focus);
		in.hitFrom = in.clock - in.clock % kTicksPerStep; // the step being heard
		in.hitTo = in.clock;
		in.history = &driver.history();
		in.plan = &plan;
		in.queued = std::span<const QueuedAction>(actions, queuedByTime(generator, actions));
		return in;
	}
	ArcMap map(int32_t scroll = 0, int32_t focus = -1) {
		ArcMap out;
		composeArcMap(input(scroll, focus), out);
		return out;
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
		piece.decideTo(2 * kPhraseSteps + 20);
		ArcMap map = piece.map();
		expect(ArcMap::kNowColumn).to_equal(3);
		expect(map.cell[kSectionRow][column(0)].when).to_equal(ArcMapCell::When::Now);
		expect(map.cell[kSectionRow][column(-1)].when).to_equal(ArcMapCell::When::Played);
		expect(map.cell[kSectionRow][column(1)].when).to_equal(ArcMapCell::When::Planned);
		expect(map.cell[kSectionRow][column(0)].kind).to_equal(SectionKind::Groove);
		// Phrases before step 0 never played.
		expect(map.cell[kSectionRow][column(-2)].look).to_equal(ArcMapCell::Look::Section);
		expect(map.cell[kSectionRow][column(-3)].look).to_equal(ArcMapCell::Look::Off);
		expect(piece.map(-1).cell[kSectionRow][column(-2)].look).to_equal(ArcMapCell::Look::Off);
	});

	it("shows the parts from the top row down: sounded behind, planned ahead", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 20);
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
		piece.decideTo(kPhraseSteps + 1);
		// The Kick plays on the bar line: on its step the drums' now pad is lit as a hit.
		while (piece.driver.clock() < stepTick(kPhraseSteps)) {
			piece.driver.advance(1, kGeneratorTickMagnitude, piece.queue, nullptr, piece.sink);
		}
		expect(piece.map().cell[kDrumsRow][column(0)].hit).to_be_true();
		expect(piece.map(0, 0).cell[kDrumsRow][column(0)].look).to_equal(ArcMapCell::Look::Focus);
	});

	it("shows Energy against the centre: below it in a Break", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 40);
		piece.steer({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcMap map = piece.map();
		expect(map.cell[kEnergyRow][column(0)].look).to_equal(ArcMapCell::Look::Energy);
		expect(map.cell[kEnergyRow][column(0)].energy).to_equal(0); // Groove: at the centre
		expect(map.cell[kEnergyRow][column(1)].energy < -1).to_be_true();
	});

	it("blinks a queued force in the Section row of the column where it lands, and the plan shows its result", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 40);
		expect(piece.map().cell[kSectionRow][column(1)].blink).to_be_false();
		piece.steer({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcMap map = piece.map();
		expect(map.cell[kSectionRow][column(1)].blink).to_be_true();
		expect(map.cell[kSectionRow][column(1)].kind).to_equal(SectionKind::Break);
		expect(map.cell[kSectionRow][column(2)].blink).to_be_false();
		expect(map.cell[kAcidRow][column(1)].look).to_equal(ArcMapCell::Look::Off); // the Break keeps no ACID
	});

	it("blinks a queued mute in its part's row where it lands, then shows the part muted", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 5);
		piece.steer({CommandKind::SetMute, 1, kPress});
		ArcMap map = piece.map();
		expect(map.cell[kAcidRow][column(0)].blink).to_be_true(); // it lands on this phrase's next bar line
		expect(map.cell[kAcidRow][column(1)].blink).to_be_false();
		expect(map.cell[kAcidRow][column(1)].look).to_equal(ArcMapCell::Look::Muted);
	});

	it("scrolls with X: back over what played, forward over the plan, no further than either", _ {
		Piece piece;
		piece.decideTo(6 * kPhraseSteps + 5);
		ArcMapRange range = arcMapScrollRange(piece.input());
		expect(range.back).to_equal(-3); // phrases 0–2 lie left of the three played columns
		expect(range.ahead).to_equal(ArcPlan::kMaxPhrases - 1 - (ArcMap::kColumns - 1 - ArcMap::kNowColumn));
		ArcMap back = piece.map(range.back);
		expect(back.cell[kSectionRow][0].when).to_equal(ArcMapCell::When::Played);
		expect(back.cell[kSectionRow][column(3)].when).to_equal(ArcMapCell::When::Now);
		ArcMap ahead = piece.map(range.ahead);
		expect(ahead.cell[kSectionRow][ArcMap::kColumns - 1].look).to_equal(ArcMapCell::Look::Section);
	});

	it("keeps column 1 lit when scrolled fully back, while the next phrase is already being decided", _ {
		Piece piece;
		// Phrase 16 is heard; phrase 17's first step is decided, 12 ticks before it is heard.
		piece.decideTo(17 * kPhraseSteps + 1);
		expect(piece.driver.clock() < stepTick(17 * kPhraseSteps)).to_be_true();
		ArcMapRange range = arcMapScrollRange(piece.input());
		expect(range.back < 0).to_be_true();
		ArcMap back = piece.map(range.back);
		expect(back.cell[kSectionRow][0].look).to_equal(ArcMapCell::Look::Section);
		expect(back.cell[kSectionRow][0].when).to_equal(ArcMapCell::When::Played);
	});

	it("shows nothing after the line where a queued End stops the piece", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 40);
		piece.steer({CommandKind::End, 0, 1});
		ArcMap map = piece.map();
		expect(map.cell[kSectionRow][column(1)].kind).to_equal(SectionKind::Outro);
		expect(map.cell[kSectionRow][column(1)].blink).to_be_true();
		expect(map.cell[kSectionRow][column(2)].look).to_equal(ArcMapCell::Look::Off);
		expect(map.cell[kDrumsRow][column(2)].look).to_equal(ArcMapCell::Look::Off);
	});

	it("ARC page: boxes now and the next 7 phrases, the queued ones blinking, and names the Arc's next change", _ {
		Piece piece;
		piece.decideTo(kPhraseSteps + 40);
		piece.steer({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcPage page;
		composeArcPage(piece.input(), page);
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
		piece.decideTo(kPhraseSteps + 40);
		piece.steer({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
		ArcPage page;
		composeArcPage(piece.input(), page);
		int32_t drops = 0;
		for (const ArcBox& box : page.box) {
			drops += box.drop ? 1 : 0;
		}
		expect(drops > 0).to_be_true();

		Piece ending;
		ending.decideTo(kPhraseSteps + 40);
		ending.steer({CommandKind::End, 0, 1});
		composeArcPage(ending.input(), page);
		expect(std::string{page.next}).to_equal(std::string{"NEXT OUTRO 6 BARS"});
		expect(page.box[2].ended).to_be_true();
	});
});

CPPSPEC_SPEC(arc_map)
