// The Respond interplay rule (spec §5.5): each bar is two half-bars, and the half where the target has more planned
// onsets is against, the first on a non-zero tie, neither when both are empty. The count is taken at the half-bar's
// first step and holds for it. A Lead Respond Bass over an offbeat bass plays in each bar's second half.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrumsPart = 0;
constexpr uint8_t kBassPart = 1;
constexpr uint8_t kLeadPart = 2;
constexpr int32_t kPhraseBars = 2;
constexpr int32_t kPhrase = kPhraseBars * kStepsPerBar;

constexpr Pattern kKick[] = {{"X...X...X...X...", "................", 0}};
constexpr LaneSpec kDrums[] = {{.lane = Lane::Kick, .pool = kKick}};
/// An offbeat bass, every hit an anchor: two in each half of every bar.
constexpr Pattern kOffbeat[] = {{"..X...X...X...X.", "................", 0}};
/// A bass with no anchor whose busier half moves with Density: nothing at the lowest, its second half (steps 12 and
/// 14) in the middle, its first (steps 2, 4 and 6) at the top.
constexpr Pattern kMoving[] = {{"..2.4.6.....8.9.", "................", 0}};
/// A Lead that would play nearly every step, anchored only on step 10, in a bar's second half.
constexpr Pattern kBusyLead[] = {{"9999999999X99999", "................", 0}};
/// Lead patterns in one tier, so a phrase line's re-pick changes what plays without a Complexity turn.
constexpr Pattern kLeads[] = {
    {"9797979797X79797", "................", 0},
    {"7979797979X97979", "................", 0},
    {"9977997799X79977", "................", 0},
    {"7799779977X97799", "................", 0},
};
/// Two bass patterns in one tier, every hit an anchor: busy in a bar's first half, or in its second. A phrase line's
/// re-pick can move the busier half.
constexpr Pattern kFrontOrBack[] = {
    {"X.X.X.X.........", "................", 0},
    {"........X.X.X.X.", "................", 0},
};
constexpr LineWeight kRoots[] = {{0, 2}, {2, 1}, {4, 1}};
constexpr Scale kScales[] = {Scale::Minor};

/// Drums, a Bass playing `BassPool` and a Lead playing `LeadPool`, the Lead with Respond Bass at `Strength` and
/// `Sensitivity`, or with no rule. No slides, so no tie hides a note-on; no lift after the Drop, so a Groove's
/// phrases share one Density.
template <const std::span<const Pattern>& BassPool, const std::span<const Pattern>& LeadPool, int8_t Strength,
          int8_t Sensitivity = 0, bool HasRule = true>
struct LeadRespondsBass {
	static constexpr InterplayRule rule[] = {
	    {.kind = RuleKind::Respond, .target = {kBassPart}, .strength = Strength, .sensitivity = Sensitivity}};
	static constexpr std::span<const InterplayRule> rules =
	    HasRule ? std::span<const InterplayRule>(rule) : std::span<const InterplayRule>();
	static constexpr PartSpec parts[] = {
	    {.role = Role::Drums, .lanes = kDrums},
	    {.role = Role::Bass,
	     .flavour = Flavour::Loop,
	     .pool = BassPool,
	     .loop = {.degrees = kRoots, .slide = 0, .gateTicks = 2 * kTicksPerStep}},
	    {.role = Role::Lead,
	     .flavour = Flavour::Loop,
	     .pool = LeadPool,
	     .rules = rules,
	     .loop = {.degrees = kRoots, .slide = 0, .gateTicks = 2 * kTicksPerStep}},
	};
	static constexpr Genre genre = {
	    .name = "respond",
	    .facts = {.scales = kScales},
	    .phraseBars = kPhraseBars,
	    .parts = parts,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {8, 8},
	            .breakPhrases = {1, 1},
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .gap = GapLength::None,
	        },
	};
};

/// A kick busy in a bar's first half (steps 0, 2, 4 and 6; step 8 in its second), and a Kit whose closed hat would
/// play every step, Responding to the kick at full strength.
struct HatRespondsKick {
	static constexpr Pattern kFrontKick[] = {{"X.X.X.X.X.......", "................", 0}};
	static constexpr Pattern kHats[] = {{"9999999999999999", "................", 0}};
	static constexpr InterplayRule kRespondKick[] = {
	    {.kind = RuleKind::Respond, .target = {kDrumsPart, Lane::Kick}, .strength = 100}};
	static constexpr LaneSpec lanes[] = {
	    {.lane = Lane::Kick, .pool = kFrontKick},
	    {.lane = Lane::ClosedHat, .pool = kHats, .rules = kRespondKick},
	};
	static constexpr PartSpec parts[] = {{.role = Role::Drums, .lanes = lanes}};
	static constexpr Genre genre = {
	    .name = "respond-kit",
	    .facts = {.scales = kScales},
	    .phraseBars = kPhraseBars,
	    .parts = parts,
	    .arc = {.groovePhrases = {8, 8}, .dropLiftEnergy = 0, .dropLiftDensity = 0, .gap = GapLength::None},
	};
};

/// The same kick, and a closed hat on every step of a bar's second half, with no rule: the Kit read whole is busier
/// there (8 steps to 4), though the kick alone is busier in the first. A Lead Responds to the whole Kit.
struct LeadRespondsKit {
	static constexpr uint8_t kLead = 1;
	static constexpr Pattern kBackHats[] = {{"........XXXXXXXX", "................", 0}};
	static constexpr InterplayRule kRespondDrums[] = {
	    {.kind = RuleKind::Respond, .target = {kDrumsPart}, .strength = 100}};
	static constexpr LaneSpec lanes[] = {
	    {.lane = Lane::Kick, .pool = HatRespondsKick::kFrontKick},
	    {.lane = Lane::ClosedHat, .pool = kBackHats},
	};
	static constexpr PartSpec parts[] = {
	    {.role = Role::Drums, .lanes = lanes},
	    {.role = Role::Lead,
	     .flavour = Flavour::Loop,
	     .pool = kBusyLead,
	     .rules = kRespondDrums,
	     .loop = {.degrees = kRoots, .slide = 0, .gateTicks = 2 * kTicksPerStep}},
	};
	static constexpr Genre genre = {
	    .name = "respond-whole-kit",
	    .facts = {.scales = kScales},
	    .phraseBars = kPhraseBars,
	    .parts = parts,
	    .arc = {.groovePhrases = {8, 8}, .dropLiftEnergy = 0, .dropLiftDensity = 0, .gap = GapLength::None},
	};
};

/// Rule chains for interplayRulesValid(): a Respond rule reading a Respond source directly or through other rules.
namespace chains {
constexpr InterplayRule kRespondKick[] = {{.kind = RuleKind::Respond, .target = {kDrumsPart, Lane::Kick}}};
constexpr InterplayRule kLockHat[] = {{.kind = RuleKind::Lock, .target = {kDrumsPart, Lane::ClosedHat}}};
constexpr InterplayRule kLockKick[] = {{.kind = RuleKind::Lock, .target = {kDrumsPart, Lane::Kick}}};
constexpr InterplayRule kRespondBass[] = {{.kind = RuleKind::Respond, .target = {kBassPart}}};
constexpr InterplayRule kRespondLead[] = {{.kind = RuleKind::Respond, .target = {kLeadPart}}};
constexpr InterplayRule kLockLead[] = {{.kind = RuleKind::Lock, .target = {kLeadPart}}};
constexpr InterplayRule kLockOpenHat[] = {{.kind = RuleKind::Lock, .target = {kDrumsPart, Lane::OpenHat}}};
constexpr InterplayRule kRespondOpenHat[] = {{.kind = RuleKind::Respond, .target = {kDrumsPart, Lane::OpenHat}}};

/// Drums (Kick, and a closed hat with `Hat`'s rules), a Bass with `Bass`'s, a Lead with `Lead`'s and an FX part with
/// `Fx`'s.
template <const std::span<const InterplayRule>& Hat, const std::span<const InterplayRule>& Bass,
          const std::span<const InterplayRule>& Lead, const std::span<const InterplayRule>& Fx>
struct Parts {
	static constexpr LaneSpec lanes[] = {{.lane = Lane::Kick, .pool = kKick},
	                                     {.lane = Lane::ClosedHat, .pool = kKick, .rules = Hat}};
	static constexpr PartSpec parts[] = {
	    {.role = Role::Drums, .lanes = lanes},
	    {.role = Role::Bass, .flavour = Flavour::Loop, .pool = kOffbeat, .rules = Bass},
	    {.role = Role::Lead, .flavour = Flavour::Loop, .pool = kBusyLead, .rules = Lead},
	    {.role = Role::FX, .rules = Fx}};
	static constexpr Genre genre = {.name = "chain", .facts = {.scales = kScales}, .parts = parts};
};
constexpr std::span<const InterplayRule> kNone;
constexpr std::span<const InterplayRule> kHatRespondsKick = kRespondKick;
constexpr std::span<const InterplayRule> kHatLocksKick = kLockKick;
constexpr std::span<const InterplayRule> kBassLocksHat = kLockHat;
constexpr std::span<const InterplayRule> kLeadRespondsBass = kRespondBass;
constexpr std::span<const InterplayRule> kFxRespondsLead = kRespondLead;
constexpr std::span<const InterplayRule> kFxLocksLead = kLockLead;

/// A Kit of four lanes: a closed hat with `Hat`'s rules, an open hat with `Open`'s, a Perc with `Perc`'s.
template <const std::span<const InterplayRule>& Hat, const std::span<const InterplayRule>& Open,
          const std::span<const InterplayRule>& Perc>
struct Lanes {
	static constexpr LaneSpec lanes[] = {{.lane = Lane::Kick, .pool = kKick},
	                                     {.lane = Lane::ClosedHat, .pool = kKick, .rules = Hat},
	                                     {.lane = Lane::OpenHat, .pool = kKick, .rules = Open},
	                                     {.lane = Lane::Perc1, .pool = kKick, .rules = Perc}};
	static constexpr PartSpec parts[] = {{.role = Role::Drums, .lanes = lanes}};
	static constexpr Genre genre = {.name = "lane-chain", .facts = {.scales = kScales}, .parts = parts};
};
constexpr std::span<const InterplayRule> kOpenLocksHat = kLockHat;
constexpr std::span<const InterplayRule> kPercRespondsOpen = kRespondOpenHat;
constexpr std::span<const InterplayRule> kPercLocksOpen = kLockOpenHat;
} // namespace chains

constexpr std::span<const Pattern> kOffbeatPool = kOffbeat;
constexpr std::span<const Pattern> kMovingPool = kMoving;
constexpr std::span<const Pattern> kBusyLeadPool = kBusyLead;
constexpr std::span<const Pattern> kLeadsPool = kLeads;

template <int8_t Strength, int8_t Sensitivity = 0, bool HasRule = true>
using OverOffbeat = LeadRespondsBass<kOffbeatPool, kBusyLeadPool, Strength, Sensitivity, HasRule>;
template <bool HasRule = true>
using OverMoving = LeadRespondsBass<kMovingPool, kBusyLeadPool, 100, 0, HasRule>;
template <int8_t Strength>
using Repicking = LeadRespondsBass<kOffbeatPool, kLeadsPool, Strength>;
constexpr std::span<const Pattern> kFrontOrBackPool = kFrontOrBack;
template <bool HasRule = true>
using OverRepickingBass = LeadRespondsBass<kFrontOrBackPool, kBusyLeadPool, 100, 0, HasRule>;

Run respondRun(const Genre& genre, uint32_t seed, int32_t density, int32_t complexity = 25) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, macroPoints(25));
	run.seed = seed;
	run.skipIntro = true;
	return run;
}

Command density(int32_t points) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), macroPoints(points)};
}

/// On each decided step, the spec's own reading of Respond: at a half-bar's first step, the Bass's planned onsets in
/// each half of the bar at the Density then, and whether this half is against; on its later steps, whether a count
/// taken now would have said otherwise. And the Lead's planned onsets, as a rule reading it sees them.
struct Halves {
	std::map<int32_t, bool> against; ///< By half-bar, from the count at its first step.
	std::set<int32_t> lead;
	int32_t ties = 0;    ///< Half-bars counted on a non-zero tie.
	int32_t empty = 0;   ///< Half-bars counted with both halves empty.
	int32_t busier = 0;  ///< Half-bars counted busier than the other half.
	int32_t quieter = 0; ///< Half-bars counted quieter than the other half.
	int32_t moved = 0;   ///< Later steps where a count taken then would have said otherwise.
	int32_t flips = 0;   ///< Half-bars whose verdict differs from the same half of the bar before.
	static inline Halves* current = nullptr;
	Halves() {
		current = this;
		Generator::testDecided_ = [](const Generator& g, int32_t step, int32_t stepDensity) {
			Halves& h = *current;
			const RespondHalves counts = respondHalves(g, {kBassPart}, step, stepDensity);
			const int32_t mine = counts.mine;
			const int32_t other = counts.other;
			const bool against = counts.against();
			if (step % kHalfBarSteps == 0) {
				h.against[step / kHalfBarSteps] = against;
				const int32_t before = step / kHalfBarSteps - 2;
				h.flips += h.against.count(before) != 0 && h.against[before] != against ? 1 : 0;
				h.ties += mine == other && mine > 0 ? 1 : 0;
				h.empty += mine == 0 && other == 0 ? 1 : 0;
				h.busier += mine > other ? 1 : 0;
				h.quieter += mine < other ? 1 : 0;
			}
			else if (h.against.count(step / kHalfBarSteps) != 0 && h.against[step / kHalfBarSteps] != against) {
				h.moved++;
			}
			if (g.testTargetPlanned({kLeadPart}, step, stepDensity)) {
				h.lead.insert(step);
			}
		};
	}
	~Halves() {
		Generator::testDecided_ = nullptr;
		current = nullptr;
	}
	Halves(const Halves&) = delete;
	Halves& operator=(const Halves&) = delete;
};

/// The Lead's planned onsets with no rule, over `bars`.
std::set<int32_t> freeLead(const Run& run, int32_t bars) {
	Halves halves;
	play(run, bars);
	return halves.lead;
}

/// The steps Lead note-ons start on.
std::set<int32_t> leadSteps(const Result& result) {
	std::set<int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == kLeadPart) {
			out.insert(e.tick / kTicksPerStep);
		}
	}
	return out;
}

/// The steps the Lead's note-ons start on from `from` to `to`, counted from `from`: its rhythm, which Respond shapes.
/// Not its pitches: a take's step-back rolls a fresh Loop line, and freeze holds the line as it is (spec §6.6, §6.7).
std::vector<int32_t> leadRhythm(const Result& result, int32_t from, int32_t to) {
	std::vector<int32_t> out;
	for (const Event& e : ons(result, kLeadPart, Lane::None, from, to)) {
		out.push_back(e.tick / kTicksPerStep - from);
	}
	return out;
}

bool subset(const std::set<int32_t>& inner, const std::set<int32_t>& outer) {
	for (int32_t step : inner) {
		if (outer.count(step) == 0) {
			return false;
		}
	}
	return true;
}

/// The steps where the Lead's planned onsets in `halves` don't follow Respond, read against the Lead's with no rule:
/// in an against half only its anchor (step 10 of a bar) is left, in a free half everything.
std::vector<int32_t> unfollowed(const Halves& halves, const std::set<int32_t>& free, int32_t steps) {
	std::vector<int32_t> out;
	for (int32_t step = 0; step < steps; step++) {
		const bool anchor = step % kStepsPerBar == 10;
		const bool expected = free.count(step) != 0 && (!halves.against.at(step / kHalfBarSteps) || anchor);
		if ((halves.lead.count(step) != 0) != expected) {
			out.push_back(step);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe respond("The Respond rule", $ {
	it("at full strength over an offbeat bass, plays the Lead only in each bar's second half", _ {
		int32_t second = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			for (int32_t d : {20, 35, 50}) {
				for (const int32_t step : leadSteps(play(respondRun(OverOffbeat<100>::genre, seed, d), 8))) {
					expect(step % kStepsPerBar).to_be_greater_than(kHalfBarSteps - 1);
					second++;
				}
				// With no rule it plays both halves.
				int32_t first = 0;
				for (const int32_t step : leadSteps(play(respondRun(OverOffbeat<100, 0, false>::genre, seed, d), 8))) {
					first += halfOf(step) == 0 ? 1 : 0;
				}
				expect(first).to_be_greater_than(0);
			}
		}
		expect(second).to_be_greater_than(0);
	});

	it("makes the busier half against, the first on a non-zero tie, and neither when both are empty", _ {
		constexpr int32_t kBars = 8;
		Halves total;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			for (int32_t d : {0, 15, 30, 40, 50}) {
				const std::set<int32_t> free = freeLead(respondRun(OverMoving<false>::genre, seed, d), kBars);
				Halves halves;
				play(respondRun(OverMoving<>::genre, seed, d), kBars);
				expect(unfollowed(halves, free, kBars * kStepsPerBar).empty()).to_be_true();
				total.ties += halves.ties;
				total.empty += halves.empty;
				total.busier += halves.busier;
				total.quieter += halves.quieter;
			}
		}
		// Every case came up.
		expect(total.ties).to_be_greater_than(0);
		expect(total.empty).to_be_greater_than(0);
		expect(total.busier).to_be_greater_than(0);
		expect(total.quieter).to_be_greater_than(0);
	});

	it("holds a half-bar's count when Density moves in the middle of it", _ {
		constexpr int32_t kBars = 12;
		int32_t moved = 0;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			// Density turns on the 4th and 12th steps of bars, from the top to the middle and back.
			std::vector<std::pair<int32_t, Command>> turns;
			for (int32_t bar = 1; bar < kBars; bar++) {
				turns.emplace_back(bar * kStepsPerBar + (bar % 2 == 0 ? 3 : 11), density(bar % 4 < 2 ? 30 : 50));
			}
			Run free = respondRun(OverMoving<false>::genre, seed, 50);
			free.steer = steerAt(turns);
			Run ruled = respondRun(OverMoving<>::genre, seed, 50);
			ruled.steer = steerAt(turns);
			const std::set<int32_t> lead = freeLead(free, kBars);
			Halves halves;
			play(ruled, kBars);
			expect(unfollowed(halves, lead, kBars * kStepsPerBar).empty()).to_be_true();
			moved += halves.moved;
		}
		expect(moved).to_be_greater_than(0); // a count taken later would have differed
	});

	it("drops a keyed share below full strength, more at a higher strength or, with sensitivity, a higher Complexity", _ {
		size_t byStrength[4] = {};
		size_t byComplexity[3] = {};
		for (uint32_t seed = 1; seed <= 10; seed++) {
			const std::set<int32_t> strengths[] = {
			    leadSteps(play(respondRun(OverOffbeat<0>::genre, seed, 50), 8)),
			    leadSteps(play(respondRun(OverOffbeat<30>::genre, seed, 50), 8)),
			    leadSteps(play(respondRun(OverOffbeat<60>::genre, seed, 50), 8)),
			    leadSteps(play(respondRun(OverOffbeat<100>::genre, seed, 50), 8)),
			};
			for (size_t i = 0; i < std::size(strengths); i++) {
				byStrength[i] += strengths[i].size();
				expect(i == 0 || subset(strengths[i], strengths[i - 1])).to_be_true();
			}
			// Strength 60 with sensitivity +40: 40 % at Complexity 0, 60 % at the default 25, 80 % at 50.
			const std::set<int32_t> complexities[] = {
			    leadSteps(play(respondRun(OverOffbeat<60, 40>::genre, seed, 50, 0), 8)),
			    leadSteps(play(respondRun(OverOffbeat<60, 40>::genre, seed, 50, 25), 8)),
			    leadSteps(play(respondRun(OverOffbeat<60, 40>::genre, seed, 50, 50), 8)),
			};
			for (size_t i = 0; i < std::size(complexities); i++) {
				byComplexity[i] += complexities[i].size();
				expect(i == 0 || subset(complexities[i], complexities[i - 1])).to_be_true();
			}
			expect(complexities[1] == strengths[2]).to_be_true();
		}
		for (size_t i = 1; i < std::size(byStrength); i++) {
			expect(byStrength[i]).to_be_less_than(byStrength[i - 1]);
		}
		for (size_t i = 1; i < std::size(byComplexity); i++) {
			expect(byComplexity[i]).to_be_less_than(byComplexity[i - 1]);
		}
	});

	it("replays the same Lead after a take step-back", _ {
		constexpr int32_t kBars = 6 * kPhraseBars;
		int32_t changed = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			Run plain = respondRun(Repicking<60>::genre, seed, 50);
			Run stepped = plain;
			// Take 1 lands on phrase 2, the step back on phrase 4 (takes land on phrase lines).
			stepped.steer = steerAt({{kPhrase + 5, {CommandKind::SetTake, kLeadPart, 1}},
			                         {3 * kPhrase + 5, {CommandKind::SetTake, kLeadPart, 0}}});
			const Result a = play(plain, kBars);
			const Result b = play(stepped, kBars);
			constexpr int32_t kEnd = kBars * kStepsPerBar;
			expect(leadRhythm(b, 0, 2 * kPhrase) == leadRhythm(a, 0, 2 * kPhrase)).to_be_true();
			changed += leadRhythm(b, 2 * kPhrase, 4 * kPhrase) == leadRhythm(a, 2 * kPhrase, 4 * kPhrase) ? 0 : 1;
			expect(leadRhythm(b, 4 * kPhrase, kEnd) == leadRhythm(a, 4 * kPhrase, kEnd)).to_be_true();
		}
		expect(changed).to_be_greater_than(0);
	});

	it("replays the same Lead while it is frozen", _ {
		int32_t repicked = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			// Frozen on phrase 3's line: it replays phrase 2, the one just heard.
			Run frozen = respondRun(Repicking<60>::genre, seed, 50);
			frozen.steer = at(3 * kPhrase, {CommandKind::SetFreeze, kLeadPart, 1});
			const Result held = play(frozen, 6 * kPhraseBars);
			const auto heard = leadRhythm(held, 2 * kPhrase, 3 * kPhrase);
			expect(heard.empty()).to_be_false();
			for (int32_t phrase = 3; phrase < 6; phrase++) {
				expect(leadRhythm(held, phrase * kPhrase, (phrase + 1) * kPhrase) == heard).to_be_true();
			}
			// Unfrozen, the Lead changes at some phrase line.
			const Result free = play(respondRun(Repicking<60>::genre, seed, 50), 6 * kPhraseBars);
			for (int32_t phrase = 3; phrase < 6; phrase++) {
				repicked += leadRhythm(free, phrase * kPhrase, (phrase + 1) * kPhrase) == heard ? 0 : 1;
			}
		}
		expect(repicked).to_be_greater_than(0);
	});

	it("counts the target's halves after its phrase-line re-pick", _ {
		constexpr int32_t kBars = 24;
		int32_t flips = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const std::set<int32_t> free = freeLead(respondRun(OverRepickingBass<false>::genre, seed, 50), kBars);
			Halves halves;
			play(respondRun(OverRepickingBass<>::genre, seed, 50), kBars);
			expect(unfollowed(halves, free, kBars * kStepsPerBar).empty()).to_be_true();
			flips += halves.flips;
		}
		expect(flips).to_be_greater_than(0); // a re-pick moved the busier half
	});

	it("plays a Drums lane against an earlier lane's busier half", _ {
		// The kick is busier in the first half: the hat plays only in the second.
		std::set<int32_t> hats;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			for (const Event& e : ons(play(respondRun(HatRespondsKick::genre, seed, 50), 8), kDrumsPart,
			                          Lane::ClosedHat, 0, 8 * kStepsPerBar)) {
				hats.insert(e.tick / kTicksPerStep % kStepsPerBar);
			}
		}
		// Every step of the second half, none of the first.
		std::set<int32_t> secondHalf;
		for (int32_t step = kHalfBarSteps; step < kStepsPerBar; step++) {
			secondHalf.insert(step);
		}
		expect(hats == secondHalf).to_be_true();
	});

	it("reads a Drums part named whole as the union of its lanes", _ {
		// The Kit is busier in the second half (the hats), though the kick alone is busier in the first: the Lead plays
		// the first half, and its anchor on step 10.
		std::set<int32_t> lead;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			for (const Event& e : ons(play(respondRun(LeadRespondsKit::genre, seed, 50), 8), LeadRespondsKit::kLead,
			                          Lane::None, 0, 8 * kStepsPerBar)) {
				lead.insert(e.tick / kTicksPerStep % kStepsPerBar);
			}
		}
		expect(lead.count(0) + lead.count(1) + lead.count(2)).to_be_greater_than(size_t{0});
		expect(lead.count(10)).to_equal(size_t{1});
		for (const int32_t step : lead) {
			expect(step < kHalfBarSteps || step == 10).to_be_true();
		}
	});

	it("is refused by interplayRulesValid() when its target responds, or reads a Respond source at any depth", _ {
		using namespace chains;
		expect(interplayRulesValid(OverOffbeat<100>::genre)).to_be_true();
		// Parts: FX Respond Lead, Lead Respond Bass.
		expect(interplayRulesValid(Parts<kNone, kNone, kLeadRespondsBass, kFxRespondsLead>::genre)).to_be_false();
		expect(interplayRulesValid(Parts<kNone, kNone, kLeadRespondsBass, kFxLocksLead>::genre)).to_be_true();
		// One level down: Lead Respond Bass, Bass Lock HatC, HatC Respond Kick.
		expect(interplayRulesValid(Parts<kHatRespondsKick, kBassLocksHat, kLeadRespondsBass, kNone>::genre))
		    .to_be_false();
		expect(interplayRulesValid(Parts<kHatLocksKick, kBassLocksHat, kLeadRespondsBass, kNone>::genre)).to_be_true();
		expect(interplayRulesValid(Parts<kHatRespondsKick, kBassLocksHat, kNone, kNone>::genre)).to_be_true();
		// Lanes: Perc1 Respond OpenHat, OpenHat Lock HatC, HatC Respond Kick.
		expect(interplayRulesValid(Lanes<kHatRespondsKick, kOpenLocksHat, kPercRespondsOpen>::genre)).to_be_false();
		expect(interplayRulesValid(Lanes<kHatLocksKick, kOpenLocksHat, kPercRespondsOpen>::genre)).to_be_true();
		expect(interplayRulesValid(Lanes<kHatRespondsKick, kOpenLocksHat, kPercLocksOpen>::genre)).to_be_true();
	});
});

CPPSPEC_SPEC(respond)
