// The SONG page's Arc map and the ARC OLED page (spec §13.5): one column per phrase, column 4 now, the played ones from
// the driver's history and the planned ones from the Arc plan, queued actions blinking where they land. And what the
// page keeps between frames (ArcMapState): the plan and when it is worked out again, the X scroll, the flash window.

#include "generative/perform/arc_map.h"
#include "generative/perform/queue_text.h"
#include "harness.h"
#include "transition_piece.h"

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

	explicit Piece(const Recipe& recipe = acidRecipe())
	    : StepwisePiece({.recipe = recipe, .seed = 3, .skipIntro = true}) {}
	explicit Piece(const Start& start) : StepwisePiece(start) {}
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

/// The ARC page of a transition's incoming roster right after the handover, 12 ticks before its tick 0: step 0 is
/// decided, its Section event not yet heard. Empty tags if the handover wasn't reached in the window.
ArcPage pageAtHandover(TransitionStyle style) {
	TransitionPiece t;
	t.confirmAt(3, style);
	ArcPage page;
	if (!t.hook.runUntil([&] { return t.driver.playing() != t.out; }) || t.driver.clock() >= 0) {
		return page;
	}
	ArcMapState state;
	state.refreshPlan(t.driver.playing(), 1);
	const ArcMapInput in = {.piece = t.driver.playing(), .clock = 0, .history = &t.driver.history()};
	composeArcPage(in, state, page);
	return page;
}

/// Frames of 256 steps until the plan under way is done. @return Whether one was (at most 100 frames).
bool planToEnd(Piece& piece, uint32_t rosters) {
	for (int32_t frame = 0; frame < 100; frame++) {
		if (piece.state.refreshPlan(piece.driver.playing(), rosters, 256)) {
			return true;
		}
	}
	return false;
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
		// Acid techno has four parts: rows 5–6 stay dark.
		expect(map.cell[4][column(2)].look).to_equal(ArcMapCell::Look::Off);
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
		expect(piece.part(kEnergyRow).substr(0, 5)).to_equal(std::string{"..eee"});
	});

	it("shows Energy on row 7 for a 6-part genre (Detroit techno), and the 7th part for a 7-part roster", _ {
		// 6-part genre: rows 0..5 are parts, row 6 (spec row 7) is Energy, row 7 (spec row 8) is Section.
		Recipe sixRecipe = newRecipe(genres::technoDetroit, 1);
		sixRecipe.variants = {};
		Piece six(sixRecipe);
		six.decideBefore(kPhraseSteps + 20);
		ArcMap sixMap = six.map();
		expect(sixMap.cell[5][column(0)].look == ArcMapCell::Look::Part
		       || sixMap.cell[5][column(0)].look == ArcMapCell::Look::Off)
		    .to_be_true();
		expect(sixMap.cell[kEnergyRow][column(0)].look).to_equal(ArcMapCell::Look::Energy);
		expect(sixMap.cell[kEnergyRow][column(1)].look).to_equal(ArcMapCell::Look::Energy);
		expect(sixMap.cell[kSectionRow][column(0)].look).to_equal(ArcMapCell::Look::Section);
		expect(six.part(kEnergyRow).substr(0, 5)).to_equal(std::string{"..eee"});

		// 7-part roster: row 6 (spec row 7) shows the 7th part (never Energy), and row 7 stays Section.
		static const PartSpec kLead7 = [] {
			PartSpec p = genres::technoAcid.parts[1];
			p.role = Role::Lead;
			p.name = "ACID7";
			p.densityGate = 0;
			return p;
		}();
		static const PartSpec kSevenParts[kMaxParts] = {
		    genres::technoDetroit.parts[0], genres::technoDetroit.parts[1], genres::technoDetroit.parts[2],
		    genres::technoDetroit.parts[3], genres::technoDetroit.parts[4], genres::technoDetroit.parts[5],
		    kLead7, // part 6 (row 7): plays in Groove, kept out in Break
		};
		static const Genre kSevenGenre = [] {
			Genre genre = genres::technoDetroit;
			genre.name = "arc-seven";
			genre.parts = kSevenParts;
			genre.variantSets = {};
			return genre;
		}();
		Recipe sevenRecipe = newRecipe(kSevenGenre, 1);
		sevenRecipe.variants = {};
		Piece seven(sevenRecipe);
		// Silent trap check: PhraseHistory::lastNote(6) must start at INT32_MIN, not 0.
		expect(seven.driver.history().lastNote(6)).to_equal(INT32_MIN);
		seven.decideBefore(kPhraseSteps + 20);
		seven.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcMap sevenMap = seven.map();
		expect(sevenMap.cell[kEnergyRow][column(-1)].look).to_equal(ArcMapCell::Look::Part);
		expect(sevenMap.cell[kEnergyRow][column(-1)].part).to_equal(6);
		expect(sevenMap.cell[kEnergyRow][column(0)].look).to_equal(ArcMapCell::Look::Part);
		// Break does not keep part 6: cell must be Off, never Energy!
		expect(sevenMap.cell[kEnergyRow][column(1)].look).to_equal(ArcMapCell::Look::Off);
		expect(sevenMap.cell[kSectionRow][column(0)].look).to_equal(ArcMapCell::Look::Section);
		expect(seven.part(kEnergyRow).substr(0, 5)).to_equal(std::string{"..hn."});
		// Focus on part 6 shows Look::Focus on row 6.
		seven.focus = 6;
		expect(seven.map().cell[kEnergyRow][column(0)].look).to_equal(ArcMapCell::Look::Focus);
		expect(seven.part(kEnergyRow).substr(0, 5)).to_equal(std::string{"..hf."});
		// Muting part 6 blinks row 6 where it lands and shows Muted ahead (after cancelling Break).
		seven.focus = -1;
		seven.pushAndDecide({CommandKind::CancelNewest});
		seven.pushAndDecide({CommandKind::SetMute, 6, kPress});
		ArcMap mutedMap = seven.map();
		expect(mutedMap.cell[kEnergyRow][column(0)].blink).to_be_true();
		expect(mutedMap.cell[kEnergyRow][column(1)].look).to_equal(ArcMapCell::Look::Muted);
		expect(seven.part(kEnergyRow).substr(0, 5)).to_equal(std::string{"..hnm"});
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
		piece.pushAndDecide({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(40)});
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true(); // a macro centre moved (spec §2.8)
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_false();
		Command bias{CommandKind::SetBias, 1, -macroPoints(20)};
		bias.length = static_cast<uint16_t>(Macro::Density);
		piece.pushAndDecide(bias);
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true(); // a Part bias moves its gate (§3.3)
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_false();
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

	it("spreads the plan over frames, showing the last one until the new one is done (#454)", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		expect(piece.state.refreshPlan(piece.driver.playing(), 0)).to_be_true();
		const ArcPlan before = piece.state.plan();
		piece.pushAndDecide({CommandKind::SetMute, 1, kPress});
		// The queue at once; the plan when its last slice is done.
		expect(piece.state.refreshPlan(piece.driver.playing(), 0, 256)).to_be_false();
		expect(static_cast<int32_t>(piece.state.queued().size())).to_equal(1);
		expect(piece.state.planning()).to_be_true();
		expect(piece.state.plan() == before).to_be_true();
		int32_t frames = 2; // the one above, and the one that finishes
		while (!piece.state.refreshPlan(piece.driver.playing(), 0, 256)) {
			expect(piece.state.plan() == before).to_be_true();
			frames++;
		}
		const int32_t steps = (1 + ArcPlan::kMaxPhrases) * kPhraseSteps - piece.driver.playing()->nextStep();
		expect(frames).to_equal((steps + 255) / 256);
		ArcPlan atOnce;
		piece.driver.playing()->planArc(atOnce);
		expect(piece.state.plan() == atOnce).to_be_true();
		expect(piece.state.refreshPlan(piece.driver.playing(), 0, 256)).to_be_false(); // nothing changed
	});

	it("finishes a plan under way when the steering changes, then plans again from the newest", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		piece.state.refreshPlan(piece.driver.playing(), 0);
		const auto turn = [&](int32_t points) {
			piece.pushAndDecide({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(points)});
		};
		turn(20);
		expect(piece.state.refreshPlan(piece.driver.playing(), 0, 256)).to_be_false();
		ArcPlan at20;
		piece.driver.playing()->planArc(at20);
		turn(30); // a gold knob turned faster than a plan takes
		turn(40);
		expect(planToEnd(piece, 0)).to_be_true();
		expect(piece.state.plan() == at20).to_be_true(); // shown, though a turn behind
		expect(piece.state.planning()).to_be_true();     // and the newest is under way
		expect(planToEnd(piece, 0)).to_be_true();
		ArcPlan at40;
		piece.driver.playing()->planArc(at40);
		expect(piece.state.plan() == at40).to_be_true();
		expect(piece.state.planning()).to_be_false();
	});

	it("empties the plan at once for a new roster or a stop, while the new one is under way", _ {
		Piece piece;
		piece.decideBefore(kPhraseSteps + 5);
		piece.state.refreshPlan(piece.driver.playing(), 0);
		expect(piece.state.refreshPlan(piece.driver.playing(), 1, 256)).to_be_false(); // a new roster
		expect(piece.state.plan().count).to_equal(0);
		expect(planToEnd(piece, 1)).to_be_true();
		expect(piece.state.plan().count).to_equal(ArcPlan::kMaxPhrases);
		piece.state.refreshPlan(nullptr, 1, 256);
		expect(piece.state.plan().count).to_equal(0);
		expect(piece.state.planning()).to_be_false();
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

	it("ARC page: counts down to where a Build shorter than a phrase starts, not to its phrase's line", _ {
		// A made-up genre whose Builds last 4 bars: each starts halfway into the phrase on whose line its Drop lands.
		static constexpr WeightedBars kFourBars[] = {{4, 1}};
		auto edited = technoEdited([](PartSpec&) {});
		edited->genre.arc.buildBars = kFourBars;
		Recipe recipe = acidRecipe();
		recipe.genre = &edited->genre;
		Piece piece(recipe);
		piece.decideBefore(kPhraseSteps + 80);
		piece.page();
		// The plan's first phrase is a Groove that started before it: from its line, whatever step was decided last.
		expect(static_cast<int32_t>(piece.state.plan().at(1)->kindFrom)).to_equal(0);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
		ArcPage page = piece.page();
		// Phrase 2's box is split: the Groove it starts with, the Build that ends it.
		expect(std::string{page.box[1].startTag}).to_equal(std::string{"GR"});
		expect(std::string{page.box[1].tag}).to_equal(std::string{"BU"});
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		// The Build starts on step 320, 4 bars into phrase 2, and ends on phrase 3's line.
		expect(static_cast<int32_t>(piece.state.plan().at(2)->kindFrom)).to_equal(4 * kStepsPerBar);
		// Heard, now is a few ticks before step 208: 7 bars and those ticks to the Build, rounded up. Phrase 2's line
		// is 3 bars and those ticks away ("NEXT BUILD 4 BARS").
		expect(std::string{page.next}).to_equal(std::string{"NEXT BUILD 8 BARS"});
		// In phrase 2, before the Build: still the Build, 3 bars and a step from step 271 (heard a few ticks before),
		// not the Drop on phrase 3's line ("NEXT DROP 8 BARS").
		piece.decideBefore(2 * kPhraseSteps + 16);
		page = piece.page();
		// Now's box: the Groove heard so far, then the Build planned.
		expect(std::string{page.box[0].startTag}).to_equal(std::string{"GR"});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"BU"});
		expect(std::string{page.next}).to_equal(std::string{"NEXT BUILD 4 BARS"});
		// Once the Build plays, the Drop.
		piece.decideBefore(5 * kPhraseSteps / 2 + 8);
		page = piece.page();
		expect(std::string{page.next}).to_equal(std::string{"NEXT DROP 4 BARS"});
	});

	it("ARC page: splits the box of a 1-phrase Break a short Build ends, and names the Break, then the Build (#545)", _ {
		// A made-up Techno whose Breaks last a phrase and whose Builds last 4 bars: the Build takes the second half of
		// the Break's phrase.
		static constexpr WeightedBars kFourBars[] = {{4, 1}};
		auto edited = technoEdited([](PartSpec&) {});
		edited->genre.arc.buildBars = kFourBars;
		edited->genre.arc.breakPhrases = {1, 1};
		Recipe recipe = acidRecipe();
		recipe.genre = &edited->genre;
		Piece piece(recipe);
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		ArcPage page = piece.page();
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		expect(std::string{page.box[1].startTag}).to_equal(std::string{"BR"});
		expect(std::string{page.box[1].tag}).to_equal(std::string{"BU"});
		expect(page.box[1].blink).to_be_true(); // the force lands on its line
		expect(page.box[2].drop).to_be_true();
		expect(std::string{page.box[2].startTag}).to_equal(std::string{});
		// The Break lands on step 256, 88 steps (5.5 bars) after step 168, not the Build 4 bars later.
		expect(std::string{page.next}).to_equal(std::string{"NEXT BREAK 6 BARS"});
		// In the Break: the Build 4 bars into its phrase.
		piece.decideBefore(2 * kPhraseSteps + 16);
		page = piece.page();
		expect(std::string{page.box[0].startTag}).to_equal(std::string{"BR"});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"BU"});
		expect(std::string{page.next}).to_equal(std::string{"NEXT BUILD 4 BARS"});
		// In the Build: heard, the box stays split; the Drop is next.
		piece.decideBefore(5 * kPhraseSteps / 2 + 8);
		page = piece.page();
		expect(std::string{page.box[0].startTag}).to_equal(std::string{"BR"});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"BU"});
		expect(std::string{page.next}).to_equal(std::string{"NEXT DROP 4 BARS"});
		// After the Drop: a Groove from its line, whole.
		piece.decideBefore(3 * kPhraseSteps + 8);
		page = piece.page();
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"GR"});
		expect(page.box[0].drop).to_be_true();
	});

	it("ARC page: reads the plan, not the Section before the start, in the lookahead before tick 0 (#545 R1)", _ {
		// SHIFT+PLAY on an external clock, a song tick past a bar line: the piece waits for the next one.
		Piece piece(Start{.recipe = acidRecipe(), .seed = 3, .skipIntro = true, .songTick = 1});
		piece.decideBefore(1);
		expect(piece.driver.clock() < 0).to_be_true();
		// The history's phrase 0 is open with the Section from before the start (Intro), the Groove not yet heard.
		ArcPhrase heard;
		expect(piece.driver.history().at(0, heard)).to_be_true();
		expect(piece.driver.history().sectionHeard()).to_be_false();
		ArcPage page = piece.page();
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"GR"});

		// A transition's handover: the outgoing roster's Build (on a Drop) or Outro (FADE THROUGH) isn't the new one's.
		page = pageAtHandover(TransitionStyle::BuildIntoDrop);
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"GR"});
		page = pageAtHandover(TransitionStyle::Fade);
		expect(std::string{page.box[0].startTag}).to_equal(std::string{});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"IN"});
	});

	it("ARC page: names a short Build in phrase 0 as next before tick 0 is heard (#545 R1)", _ {
		// A made-up Techno whose Builds last 4 bars, SHIFT+PLAY on an external clock with a Build forced at once: it
		// lands on step 0's line and ends on phrase 1's, so it starts 4 bars into phrase 0, a Groove's first half.
		static constexpr WeightedBars kFourBars[] = {{4, 1}};
		auto edited = technoEdited([](PartSpec&) {});
		edited->genre.arc.buildBars = kFourBars;
		Recipe recipe = acidRecipe();
		recipe.genre = &edited->genre;
		Piece piece(Start{.recipe = recipe, .seed = 3, .skipIntro = true, .songTick = 1});
		piece.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
		piece.decideBefore(1);
		expect(piece.driver.clock() < 0).to_be_true();
		const ArcPage page = piece.page();
		expect(static_cast<int32_t>(piece.state.plan().at(0)->kindFrom)).to_equal(4 * kStepsPerBar);
		expect(std::string{page.box[0].startTag}).to_equal(std::string{"GR"});
		expect(std::string{page.box[0].tag}).to_equal(std::string{"BU"});
		// Compared with the Groove the plan starts phrase 0 with, not the Intro from before the start.
		expect(std::string{page.next}).to_equal(std::string{"NEXT BUILD 4 BARS"}); // from tick 0
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
