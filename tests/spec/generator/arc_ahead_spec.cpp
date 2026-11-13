// Arc::ahead() (spec §9.3, §9.6): where the Arc moves next as things stand, the next Section's start and the Drop,
// checked against where the Arc then really moves. The chord walk reads it for a chord's place (spec §7.4).

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr int32_t kStepsPerPhrase = 8 * kStepsPerBar;

/// Every Build length an 8-bar phrase allows, short ones included, Grooves straight into a Build half the time, and
/// Breaks that a short Build shortens.
constexpr WeightedBars kEveryBuild[] = {{1, 1}, {2, 1}, {4, 1}, {8, 1}, {16, 1}};
constexpr Genre kEveryBuildGenre{
    .name = "every Build length",
    .arc = {.introPhrases = 2,
            .groovePhrases = {2, 5},
            .breakPhrases = {1, 2},
            .buildBars = kEveryBuild,
            .grooveToBuild = 500},
};

/// A 3-phrase Groove at Motion 25 straight into a 4-bar Build: the Build starts at bar 4 of the Groove's last phrase.
constexpr WeightedBars kHalfPhraseBuild[] = {{4, 1}};
constexpr Genre kGrooveIntoShortBuild{
    .name = "Groove into a 4-bar Build",
    .arc = {.introPhrases = 1, .groovePhrases = {3, 3}, .buildBars = kHalfPhraseBuild, .grooveToBuild = 1000},
};

Macros macrosAt(int32_t motion) {
	Macros macros = technoRecipe().macros;
	macros.set(Macro::Motion, motion);
	return macros;
}

/// One step of an Arc moving on its own, and what it predicted there.
struct ArcStep {
	SectionKind section = SectionKind::Intro;
	bool start = false;
	bool drop = false;
	Arc::Ahead ahead{};
};

std::vector<ArcStep> stepArc(const Genre& genre, uint32_t seed, int32_t motion, int32_t count) {
	const Macros macros = macrosAt(motion);
	Arc arc;
	arc.start(genre);
	std::vector<ArcStep> out;
	for (int32_t n = 0; n < count; n++) {
		const bool start = arc.enterStep(genre, seed, n, macros);
		out.push_back({arc.section(), start, arc.dropStartsHere(), arc.ahead(motion)});
	}
	return out;
}

/// For each step, the first later step where `is` holds, or INT32_MAX.
template <typename Is>
std::vector<int32_t> nextWhere(const std::vector<ArcStep>& steps, Is is) {
	std::vector<int32_t> out(steps.size(), INT32_MAX);
	int32_t next = INT32_MAX;
	for (auto n = static_cast<int32_t>(steps.size()) - 1; n >= 0; n--) {
		out[n] = next;
		next = is(steps[n]) ? n : next;
	}
	return out;
}

/// Moves `arc` on through steps `from` to `to - 1`.
void stepTo(Arc& arc, const Genre& genre, const Macros& macros, int32_t from, int32_t to) {
	for (int32_t n = from; n < to; n++) {
		arc.enterStep(genre, 1, n, macros);
	}
}

} // namespace

// clang-format off
describe arc_ahead("Arc::ahead()", $ {
	it("predicts the next Section start and Drop where the Arc then moves on its own", _ {
		int32_t checked = 0;
		int32_t unknown = 0;
		int32_t wrongStart = 0;
		int32_t wrongDrop = 0;
		for (const Genre* genre : {&genres::techno, &kEveryBuildGenre}) {
			for (int32_t points : {1, 10, 25, 50}) {
				for (uint32_t seed = 1; seed <= 6; seed++) {
					const std::vector<ArcStep> steps = stepArc(*genre, seed, macroPoints(points), 240 * kStepsPerBar);
					const std::vector<int32_t> nextStart = nextWhere(steps, [](const ArcStep& s) { return s.start; });
					const std::vector<int32_t> nextDrop = nextWhere(steps, [](const ArcStep& s) { return s.drop; });
					const auto count = static_cast<int32_t>(steps.size());
					for (int32_t n = 0; n < count; n++) {
						const Arc::Ahead ahead = steps[n].ahead;
						// Motion above 0 and no hold: every Section but the Outro ends on its own.
						if (ahead.start == Arc::kNotAhead) {
							unknown++;
							continue;
						}
						if (n + ahead.start >= count) {
							continue;
						}
						checked++;
						wrongStart += nextStart[n] != n + ahead.start ? 1 : 0;
						const ArcStep& next = steps[n + ahead.start];
						if (ahead.drop == Arc::kNotAhead) {
							wrongDrop += next.drop || next.section == SectionKind::Build ? 1 : 0;
						}
						else if (n + ahead.drop < count) {
							wrongDrop += nextDrop[n] != n + ahead.drop ? 1 : 0;
						}
					}
				}
			}
		}
		expect(unknown).to_equal(0);
		expect(wrongStart).to_equal(0);
		expect(wrongDrop).to_equal(0);
		expect(checked).to_be_greater_than(100000);
	});

	it("puts the next Section start on its next line once a Section held past its length is released", _ {
		const Macros macros = macrosAt(macroPoints(25));
		for (int32_t release : {3 * kStepsPerPhrase + 2 * kStepsPerBar + 5, 5 * kStepsPerPhrase}) {
			Arc arc;
			arc.start(kEveryBuildGenre);
			arc.hold(true);
			stepTo(arc, kEveryBuildGenre, macros, 0, release);
			expect(arc.section()).to_equal(SectionKind::Intro);
			arc.hold(false);
			const Arc::Ahead ahead = arc.ahead(macroPoints(25));
			expect(ahead.start).to_equal(kStepsPerPhrase - (release - 1) % kStepsPerPhrase);
			stepTo(arc, kEveryBuildGenre, macros, release, release - 1 + ahead.start);
			expect(arc.section()).to_equal(SectionKind::Intro);
			expect(arc.enterStep(kEveryBuildGenre, 1, release - 1 + ahead.start, macros)).to_be_true();
		}
	});

	it("lets a force landing between a Build's start and its Drop replace the Drop", _ {
		// Bar 1 of the Groove's last phrase: the Build starts at bar 4 and its Drop is the next phrase line.
		const int32_t motion = macroPoints(25);
		const Macros macros = macrosAt(motion);
		const int32_t now = 3 * kStepsPerPhrase + kStepsPerBar;
		Arc arc;
		arc.start(kGrooveIntoShortBuild);
		stepTo(arc, kGrooveIntoShortBuild, macros, 0, now + 1);
		const Arc::Ahead own = arc.ahead(motion);
		expect(arc.section()).to_equal(SectionKind::Groove);
		expect(own.start).to_equal(3 * kStepsPerBar);
		expect(own.drop).to_equal(7 * kStepsPerBar);

		struct Case {
			Arc::Move move;
			int32_t drop;
		};
		const int32_t at = own.drop;
		const Case cases[] = {
		    {{.at = at, .section = SectionKind::Break}, Arc::kNotAhead},
		    {{.at = at, .section = SectionKind::Outro}, Arc::kNotAhead},
		    {{.at = at, .section = SectionKind::Groove, .stops = true}, Arc::kNotAhead},
		    {{.at = at, .section = SectionKind::Groove}, at},
		    {{.at = at, .section = SectionKind::Build, .buildSteps = kStepsPerPhrase}, at + kStepsPerPhrase},
		    {{.at = at + 1, .section = SectionKind::Break}, own.drop},
		};
		for (const Case& c : cases) {
			const Arc::Ahead ahead = arc.ahead(motion, c.move);
			expect(ahead.start).to_equal(own.start);
			expect(ahead.drop).to_equal(c.drop);
			if (c.move.stops || c.move.at != at) {
				continue;
			}
			// The Arc agrees: the force starts its Section on the Drop step instead, and the Drop comes where predicted.
			Arc forced = arc;
			stepTo(forced, kGrooveIntoShortBuild, macros, now + 1, now + at);
			forced.force(c.move.section, c.move.buildSteps);
			const int32_t last = now + (c.drop == Arc::kNotAhead ? at : c.drop);
			int32_t drops = 0;
			for (int32_t n = now + at; n <= last; n++) {
				forced.enterStep(kGrooveIntoShortBuild, 1, n, macros);
				drops += forced.dropStartsHere() ? 1 : 0;
			}
			expect(drops).to_equal(c.drop == Arc::kNotAhead ? 0 : 1);
			expect(forced.dropStartsHere()).to_equal(c.drop != Arc::kNotAhead);
		}
	});
});

CPPSPEC_SPEC(arc_ahead)
