// The Arc plan (spec §2.8, §13.5): the SONG page's phrases ahead, worked out on demand from the Arc's counters, its
// keyed draws and the pending list, so a queued action shows its result.

#include "generative/core/arc_plan.h"
#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <string>
#include <tuple>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars

/// A Techno piece driven step by step, so a spec can steer it and ask for the plan between steps. Acid techno by
/// default, on its spec Recipe.
struct Piece : StepwisePiece<> {
	explicit Piece(bool skipIntro = false, uint32_t seed = 3, int32_t motion = macroPoints(25),
	               const Genre* genre = nullptr)
	    : StepwisePiece(startOf(skipIntro, seed, motion, genre)) {}
	static Start startOf(bool skipIntro, uint32_t seed, int32_t motion, const Genre* genre) {
		Start start{.seed = seed, .skipIntro = skipIntro};
		if (genre != nullptr) {
			start.recipe = newRecipe(*genre, seed);
		}
		start.recipe.macros.set(Macro::Motion, motion);
		return start;
	}
	ArcPlan plan() const {
		ArcPlan plan;
		generator.planArc(plan);
		return plan;
	}
};

/// The Techno subgenres: the shipped genres.
const Genre* const kTechnos[] = {&genres::technoAcid, &genres::technoDub, &genres::technoMelodic,
                                 &genres::technoDetroit};

/// Every part of `genre` but FX, the roster's last: its lanes play only by section rules, so it can be planned (its
/// gate is open) in a phrase where nothing placed a note.
uint8_t notFx(const Genre& genre) {
	return static_cast<uint8_t>((1U << (partCount(genre) - 1)) - 1);
}

/// The parts with a note ringing across `tick`: started before it, ended after it. A Sustain holds its voicing over a
/// bar line with no new note-on (core-music.md), which the driver's history, counting note-ons, doesn't hear.
uint8_t ringingAcross(const std::vector<Event>& events, int32_t tick) {
	std::map<std::tuple<uint8_t, Lane, uint8_t>, bool> ringing;
	for (const Event& e : events) {
		if (e.tick > tick) {
			break;
		}
		if (e.kind == EventKind::NoteOn) {
			ringing[{e.part, e.lane, e.pitch}] = true;
		}
		else if (e.kind == EventKind::NoteOff) {
			ringing.erase({e.part, e.lane, e.pitch});
		}
	}
	uint8_t parts = 0;
	for (const auto& [key, on] : ringing) {
		parts |= static_cast<uint8_t>(1U << std::get<0>(key));
	}
	return parts;
}

/// The first phrase from `from` on, up to `to`, that the plan holds differently from what the driver then heard
/// (Section, Drop, Energy, and the parts in `parts` that played or rang on from before it), described; empty if every
/// one matches. It stops at the phrase where the piece ends.
std::string unlikeHeard(Piece& piece, const ArcPlan& plan, int32_t from, int32_t to, uint8_t parts = 0b011) {
	for (int32_t phrase = from; phrase < to; phrase++) {
		// Heard once its last step is: the history holds only the latest kPhrases phrases.
		piece.decideBefore((phrase + 1) * kPhraseSteps + 1);
		ArcPhrase heard;
		const ArcPhrase* planned = plan.at(phrase);
		if (planned != nullptr && planned->ended) {
			return ""; // nothing plays from the line where the piece ends
		}
		if (planned == nullptr || !piece.driver.history().at(phrase, heard)) {
			return "phrase " + std::to_string(phrase) + " not planned or not heard";
		}
		heard.sounds |= ringingAcross(piece.sink.events, stepTick(phrase * kPhraseSteps));
		if (planned->kind != heard.kind || planned->drop != heard.drop || planned->energy != heard.energy
		    || (planned->sounds & parts) != (heard.sounds & parts)) {
			return "phrase " + std::to_string(phrase) + ": planned kind " + std::to_string(int(planned->kind))
			       + " drop " + std::to_string(planned->drop) + " energy " + std::to_string(planned->energy)
			       + " sounds " + std::to_string(planned->sounds & parts) + ", heard kind "
			       + std::to_string(int(heard.kind)) + " drop " + std::to_string(heard.drop) + " energy "
			       + std::to_string(heard.energy) + " sounds " + std::to_string(heard.sounds & parts);
		}
	}
	return "";
}

/// Whether every phrase the plan holds from `from` on, up to `to`, is what the driver then heard: Section, Drop, Energy
/// and the parts that played.
bool heardAsPlanned(Piece& piece, const ArcPlan& plan, int32_t from, int32_t to) {
	return unlikeHeard(piece, plan, from, to).empty();
}

/// What a sweep case was, for a failing expectation.
std::string caseName(const Genre& genre, uint32_t seed, int32_t motion, bool skipIntro) {
	return std::string(genre.name) + " seed " + std::to_string(seed) + " motion " + std::to_string(motion)
	       + (skipIntro ? " no Intro" : " Intro");
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
		piece.decideBefore(3 * kPhraseSteps + 1);
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
			piece.decideBefore(kPhraseSteps);
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
		piece.decideBefore(2 * kPhraseSteps + 1);
		expect(piece.driver.clock() < stepTick(2 * kPhraseSteps)).to_be_true();
		const ArcPlan plan = piece.plan();
		expect(plan.first).to_equal(2);
		expect(heardAsPlanned(piece, plan, 2, 2 + ArcPlan::kMaxPhrases - 1)).to_be_true();
	});

	it("shows a queued force where it lands", _ {
		Piece piece(true);
		piece.decideBefore(kPhraseSteps + 40);
		expect(piece.plan().at(2)->kind).to_equal(SectionKind::Groove);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		const ArcPlan plan = piece.plan();
		expect(plan.at(1)->kind).to_equal(SectionKind::Groove);
		expect(plan.at(2)->kind).to_equal(SectionKind::Break);
		expect(plan.at(2)->sounds & 0b010).to_equal(0); // the Break keeps no ACID
		expect(heardAsPlanned(piece, plan, 2, 1 + ArcPlan::kMaxPhrases)).to_be_true();
	});

	it("shows a queued End as the Outro, and nothing after the line where it ends", _ {
		Piece piece(true);
		piece.decideBefore(kPhraseSteps + 40);
		piece.pushAndDecide({CommandKind::End, 0, 1});
		const ArcPlan plan = piece.plan();
		expect(plan.at(2)->kind).to_equal(SectionKind::Outro);
		expect(plan.at(2)->ended).to_be_false();
		expect(plan.at(3)->ended).to_be_true();
		expect(plan.at(3)->sounds).to_equal(0);
		expect(heardAsPlanned(piece, plan, 2, 3)).to_be_true();
	});

	it("shows a queued mute from the bar it lands on", _ {
		Piece piece(true);
		piece.decideBefore(kPhraseSteps + 5);
		piece.pushAndDecide({CommandKind::SetMute, 1, kPress});
		const ArcPlan plan = piece.plan();
		expect(plan.at(1)->sounds & 0b010).to_equal(0b010); // ACID plays the bar before it lands
		expect(plan.at(2)->sounds & 0b010).to_equal(0);
		expect(plan.at(2)->muted).to_equal(0b010);
		expect(heardAsPlanned(piece, plan, 2, 1 + ArcPlan::kMaxPhrases)).to_be_true();
	});
	it("plays as planned for every Techno subgenre and every part but FX, with and without an Intro", _ {
		for (const Genre* genre : kTechnos) {
			for (uint32_t seed = 1; seed <= 3; seed++) {
				for (int32_t motion : {macroPoints(25), macroPoints(50)}) {
					for (bool skipIntro : {false, true}) {
						Piece piece(skipIntro, seed, motion, genre);
						const ArcPlan plan = piece.plan();
						expect(caseName(*genre, seed, motion, skipIntro)
						       + unlikeHeard(piece, plan, 0, ArcPlan::kMaxPhrases, notFx(*genre)))
						    .to_equal(caseName(*genre, seed, motion, skipIntro));
					}
				}
			}
		}
	});

	it("plays as planned with a mute, a solo, a force or End queued, for every Techno subgenre", _ {
		struct Press {
			Command command;
			int32_t at; ///< The step decided before it is pressed.
		};
		for (const Genre* genre : kTechnos) {
			const uint8_t parts = notFx(*genre);
			const Press presses[] = {
			    {{CommandKind::SetMute, 1, kPress}, kPhraseSteps + 5},
			    {{CommandKind::SetMute, 0, kPress}, 2 * kPhraseSteps + 37},
			    {{CommandKind::SetSolo, 1, kPress}, kPhraseSteps + 70},
			    {{CommandKind::SetSolo, static_cast<uint8_t>(partCount(*genre) - 2), kPress}, 3 * kPhraseSteps + 9},
			    {{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0}, kPhraseSteps + 40},
			    {{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0}, 2 * kPhraseSteps + 3},
			    {{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0}, kPhraseSteps + 40},
			    {{CommandKind::End, 0, 1}, kPhraseSteps + 40},
			};
			for (const Press& press : presses) {
				for (uint32_t seed = 1; seed <= 2; seed++) {
					Piece piece(seed == 2, seed, macroPoints(25 * seed), genre);
					piece.decideBefore(press.at);
					piece.pushAndDecide(press.command);
					const ArcPlan plan = piece.plan();
					const int32_t from = plan.first + 1;
					const std::string name = caseName(*genre, seed, macroPoints(25 * seed), seed == 2) + " command "
					                         + std::to_string(int(press.command.kind)) + ": ";
					expect(name + unlikeHeard(piece, plan, from, plan.first + ArcPlan::kMaxPhrases, parts))
					    .to_equal(name);
				}
			}
		}
	});
});

CPPSPEC_SPEC(arc_plan)
