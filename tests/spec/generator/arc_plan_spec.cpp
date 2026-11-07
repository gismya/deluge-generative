// The Arc plan (spec §2.8, §13.5): the SONG page's phrases ahead, worked out on demand from the Arc's counters, its
// keyed draws and the pending list, so a queued action shows its result.

#include "generative/core/arc_plan.h"
#include "harness.h"

#include "cppspec.hpp"

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars

/// A Techno piece driven step by step, so a spec can steer it and ask for the plan between steps.
struct Piece {
	Generator generator;
	SteeringQueue queue;
	RecordingSink sink;
	perform::PerformDriver driver;

	explicit Piece(bool skipIntro = false, uint32_t seed = 3, int32_t motion = macroPoints(25)) {
		Recipe recipe = technoRecipe();
		recipe.macros.set(Macro::Motion, motion);
		generator.start(recipe, seed, {}, skipIntro);
		driver.begin(generator);
	}
	/// Plays until `step` is the next to decide, or the piece ends.
	void decideTo(int32_t step) {
		while (generator.nextStep() < step && !driver.ended()) {
			driver.advance(1, kGeneratorTickMagnitude, queue, nullptr, sink);
		}
	}
	/// Pushes a command, then decides one step so the core takes it into its pending list.
	void steer(const Command& command) {
		queue.push(command);
		decideTo(generator.nextStep() + 1);
	}
	ArcPlan plan() const {
		ArcPlan plan;
		generator.planArc(plan);
		return plan;
	}
};

/// Whether every phrase the plan holds from `from` on, up to `to`, is what the driver then heard: Section, Drop, Energy
/// and the parts that played. FX is left out: its lanes play only by section rules, so it can be planned (its gate is
/// open) in a phrase where nothing placed a note.
bool heardAsPlanned(Piece& piece, const ArcPlan& plan, int32_t from, int32_t to) {
	piece.decideTo(to * kPhraseSteps + 1);
	bool same = true;
	for (int32_t phrase = from; phrase < to; phrase++) {
		ArcPhrase heard;
		const ArcPhrase* planned = plan.at(phrase);
		if (planned == nullptr || !piece.driver.history().at(phrase, heard)) {
			return false;
		}
		same = same && planned->kind == heard.kind && planned->drop == heard.drop && planned->energy == heard.energy
		       && (planned->sounds & 0b011) == (heard.sounds & 0b011);
	}
	return same;
}

} // namespace

// clang-format off
describe arc_plan("Arc plan", $ {
	it("plans a piece from its first phrase: Techno's two Intro phrases, then Groove", _ {
		Piece piece;
		ArcPlan plan = piece.plan();
		expect(plan.first).to_equal(0);
		expect(plan.count).to_equal(ArcPlan::kMaxPhrases);
		expect(plan.at(0)->kind).to_equal(SectionKind::Intro);
		expect(plan.at(1)->kind).to_equal(SectionKind::Intro);
		expect(plan.at(2)->kind).to_equal(SectionKind::Groove);
		expect(plan.at(2)->drop).to_be_false();
		expect(plan.at(2)->ended).to_be_false();
	});

	it("keeps each heard phrase's Section and the parts that played in it, from the events the driver sent", _ {
		Piece piece;
		piece.decideTo(3 * kPhraseSteps + 1);
		const perform::PhraseHistory& history = piece.driver.history();
		ArcPhrase heard;
		expect(history.at(0, heard)).to_be_true();
		expect(heard.kind).to_equal(SectionKind::Intro);
		expect(history.at(2, heard)).to_be_true();
		expect(heard.kind).to_equal(SectionKind::Groove);
		expect(heard.drop).to_be_false();
		expect(heard.sounds & 0b011).to_equal(0b011); // DRUMS and ACID play in the Groove
		expect(history.at(5, heard)).to_be_false(); // not heard yet
	});

	it("plays as planned: Sections, Drops, Energy and the parts that sound, phrase by phrase", _ {
		int32_t drops = 0;
		int32_t breaks = 0;
		for (uint32_t seed = 1; seed <= 12; seed++) {
			Piece piece(true, seed, macroPoints(50));
			piece.decideTo(kPhraseSteps);
			const ArcPlan plan = piece.plan();
			expect(plan.first).to_equal(0);
			expect(heardAsPlanned(piece, plan, 1, ArcPlan::kMaxPhrases)).to_be_true();
			for (int32_t phrase = 1; phrase < ArcPlan::kMaxPhrases; phrase++) {
				drops += plan.at(phrase)->drop ? 1 : 0;
				breaks += plan.at(phrase)->kind == SectionKind::Break ? 1 : 0;
			}
		}
		expect(drops > 0).to_be_true();
		expect(breaks > 0).to_be_true();
	});

	it("plans from the phrase whose first step is decided, 12 ticks before it is heard", _ {
		Piece piece(true);
		piece.decideTo(2 * kPhraseSteps + 1);
		expect(piece.driver.clock() < stepTick(2 * kPhraseSteps)).to_be_true();
		const ArcPlan plan = piece.plan();
		expect(plan.first).to_equal(2);
		expect(heardAsPlanned(piece, plan, 2, 2 + ArcPlan::kMaxPhrases - 1)).to_be_true();
	});

	it("shows a queued force where it lands", _ {
		Piece piece(true);
		piece.decideTo(kPhraseSteps + 40);
		expect(piece.plan().at(2)->kind).to_equal(SectionKind::Groove);
		piece.steer({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		const ArcPlan plan = piece.plan();
		expect(plan.at(1)->kind).to_equal(SectionKind::Groove);
		expect(plan.at(2)->kind).to_equal(SectionKind::Break);
		expect(plan.at(2)->sounds & 0b010).to_equal(0); // the Break keeps no ACID
		expect(heardAsPlanned(piece, plan, 2, 1 + ArcPlan::kMaxPhrases)).to_be_true();
	});

	it("shows a queued End as the Outro, and nothing after the line where it ends", _ {
		Piece piece(true);
		piece.decideTo(kPhraseSteps + 40);
		piece.steer({CommandKind::End, 0, 1});
		const ArcPlan plan = piece.plan();
		expect(plan.at(2)->kind).to_equal(SectionKind::Outro);
		expect(plan.at(2)->ended).to_be_false();
		expect(plan.at(3)->ended).to_be_true();
		expect(plan.at(3)->sounds).to_equal(0);
		expect(heardAsPlanned(piece, plan, 2, 3)).to_be_true();
	});

	it("shows a queued mute from the bar it lands on", _ {
		Piece piece(true);
		piece.decideTo(kPhraseSteps + 5);
		piece.steer({CommandKind::SetMute, 1, kPress});
		const ArcPlan plan = piece.plan();
		expect(plan.at(1)->sounds & 0b010).to_equal(0b010); // ACID plays the bar before it lands
		expect(plan.at(2)->sounds & 0b010).to_equal(0);
		expect(plan.at(2)->muted).to_equal(0b010);
		expect(heardAsPlanned(piece, plan, 2, 1 + ArcPlan::kMaxPhrases)).to_be_true();
	});
});

CPPSPEC_SPEC(arc_plan)
