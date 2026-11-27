// Handover overlaps between a yielding part and its target (spec §5.6, #542): generic over any part with a YieldSpec,
// not Psytrance-specific (Goa #458 inherits it). Covers the 1-in-3 draw rate, 1:2 length weighting, configurability,
// Section-line guards, deep Brightness close + 1-beat hold + reopen snap, steady volume, no flip-flop, and Compose /
// ArcPlan parity.

#include "compose_helpers.h"
#include "core_walk.h"
#include "generative/compose/compose_driver.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kLeadPart = 1;
constexpr int32_t kArpPart = 2;

constexpr Pattern kEveryBeat[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kEveryEighth[] = {{"X.X.X.X.X.X.X.X.", "................", 0}};
constexpr LaneSpec kKickOnly[] = {{.lane = Lane::Kick, .pool = kEveryBeat}};
constexpr LineWeight kRootOnly[] = {{0, 1}};
constexpr Scale kMinorOnly[] = {Scale::Minor};
constexpr WeightedBars kFourBarBuild[] = {{4, 1}};
constexpr DropOutEntry kLeadDropOut[] = {
    {.mask = {.parts = partBit(kLeadPart)}, .bars = 4, .weight = 1},
};

/// A minimal generic 3-part genre (Drums + Lead + Arp yielding to Lead) with frequent 4-bar Lead drop-outs in
/// Grooves so handovers happen in both directions (Lead->Arp when the drop-out starts, Arp->Lead when it ends).
struct TestGenre {
	PartSpec parts[3];
	Genre genre;

	explicit TestGenre(YieldSpec yield, int16_t dropOutChance = 1000, uint8_t leadGate = 0, uint8_t groovePhrases = 4,
	                   std::span<const Pattern> leadPool = kEveryEighth, uint16_t leadNoteTicks = 16) {
		parts[kDrumsPart] = {.role = Role::Drums, .flavour = Flavour::None, .name = "DRUMS", .lanes = kKickOnly};
		parts[kLeadPart] = {.role = Role::Lead,
		                    .flavour = Flavour::Loop,
		                    .name = "LEAD",
		                    .pool = leadPool,
		                    .densityGate = static_cast<int16_t>(macroPoints(leadGate)),
		                    .loop = {.degrees = kRootOnly, .gateTicks = leadNoteTicks}};
		parts[kArpPart] = {.role = Role::Lead,
		                   .flavour = Flavour::Loop,
		                   .name = "ARP",
		                   .pool = kEveryEighth,
		                   .loop = {.degrees = kRootOnly},
		                   .yield = yield};
		genre = Genre{
		    .name = "test-overlap",
		    .oledName = "TEST OVERLAP",
		    .facts = {.tempoLowBpm = 140, .tempoHighBpm = 140, .scales = kMinorOnly},
		    .phraseBars = 8,
		    .parts = parts,
		    .dropOuts = kLeadDropOut,
		    .dropOutChance = dropOutChance,
		    .dropOutGapBars = 4,
		    .dropOutGridBars = 4,
		    .arc =
		        {
		            .introPhrases = 1,
		            .outroPhrases = 1,
		            .groovePhrases = {groovePhrases, groovePhrases},
		            .breakPhrases = {1, 1},
		            .buildBars = kFourBarBuild,
		            .grooveToBuild = 0,
		            .dropDecayPhrases = 2,
		            .dropLiftEnergy = macroPoints(12),
		            .breakKeeps = {.parts = partBit(kArpPart)},
		            .buildKeeps = {.parts = partBit(kArpPart)},
		        },
		};
	}
};

struct BarActivity {
	SectionKind section = SectionKind::Intro;
	bool sectionStart = false;
	bool lead = false;
	bool arp = false;
};

std::vector<BarActivity> activityOf(const Walked& walked) {
	const int32_t bars = static_cast<int32_t>(walked.steps.size()) / kStepsPerBar;
	std::vector<BarActivity> out(static_cast<size_t>(bars));
	for (int32_t b = 0; b < bars; b++) {
		const Walked::Step& s = walked.steps[static_cast<size_t>(b * kStepsPerBar)];
		out[static_cast<size_t>(b)].section = s.section;
		out[static_cast<size_t>(b)].sectionStart = s.sectionStart;
	}
	for (const Event& e : walked.noteOns) {
		const int32_t b = e.tick / (kStepsPerBar * kTicksPerStep);
		if (b >= 0 && b < bars) {
			if (e.part == kLeadPart) {
				out[static_cast<size_t>(b)].lead = true;
			}
			else if (e.part == kArpPart) {
				out[static_cast<size_t>(b)].arp = true;
			}
		}
	}
	return out;
}

/// Counts handovers (with and without overlap) and overlap lengths inside Grooves (off Section lines).
struct OverlapStats {
	int32_t handovers = 0;
	int32_t overlaps = 0;
	int32_t leadToArp = 0;
	int32_t arpToLead = 0;
	std::map<int32_t, int32_t> lengths;
	int32_t onSectionLine = 0;
	int32_t crossedSection = 0;
};

OverlapStats measure(const Genre& genre, uint32_t seeds, int32_t bars, bool skipIntro = true) {
	OverlapStats stats;
	for (uint32_t seed = 1; seed <= seeds; seed++) {
		Recipe recipe = newRecipe(genre, seed);
		recipe.macros.set(Macro::Motion, macroPoints(25));
		const Walked walked = walk(recipe, seed, bars * kStepsPerBar, {}, skipIntro);
		const std::vector<BarActivity> act = activityOf(walked);
		for (int32_t b = 0; b < bars; b++) {
			const BarActivity& cur = act[static_cast<size_t>(b)];
			if (cur.sectionStart && cur.lead && cur.arp) {
				stats.onSectionLine++;
			}
		}
		for (int32_t b = 1; b < bars;) {
			const BarActivity& prev = act[static_cast<size_t>(b - 1)];
			const BarActivity& cur = act[static_cast<size_t>(b)];
			if (cur.section != SectionKind::Groove || cur.sectionStart) {
				b++;
				continue;
			}
			const bool lToA = prev.lead && !prev.arp && cur.arp;
			const bool aToL = prev.arp && !prev.lead && cur.lead;
			if (!lToA && !aToL) {
				b++;
				continue;
			}
			stats.handovers++;
			if (cur.lead && cur.arp) {
				int32_t len = 0;
				while (b + len < bars && act[static_cast<size_t>(b + len)].lead
				       && act[static_cast<size_t>(b + len)].arp) {
					if (act[static_cast<size_t>(b + len)].sectionStart) {
						stats.crossedSection++;
					}
					len++;
				}
				stats.overlaps++;
				(lToA ? stats.leadToArp : stats.arpToLead)++;
				stats.lengths[len]++;
				b += len;
			}
			else {
				b++;
			}
		}
	}
	return stats;
}

/// Brightness control value on `part` at every step line of `result.events`.
std::vector<int32_t> brightnessCurve(const std::vector<Event>& events, uint8_t part, int32_t totalSteps) {
	std::vector<int32_t> curve(static_cast<size_t>(totalSteps + 1), 0);
	for (const Event& e : events) {
		if (e.kind != EventKind::Control || e.part != part || e.lane != Lane::None
		    || e.control != SoundControl::Brightness) {
			continue;
		}
		const int32_t step = e.tick / kTicksPerStep;
		const int32_t from = e.snap ? step : step + 1;
		for (int32_t s = std::max<int32_t>(from, 0); s <= totalSteps; s++) {
			curve[static_cast<size_t>(s)] = e.controlOffset();
		}
	}
	return curve;
}

} // namespace

// clang-format off
describe handover_overlap("Handover overlap between a yielding pair (spec §5.6, #542)", $ {
	it("draws overlaps on about 1 handover in 3, weighted 1:2 for 1 vs 2 bars, in both directions on any yielding pair", _ {
		const TestGenre g({.part = kLeadPart, .inDrops = YieldMode::Rest, .outside = YieldMode::Rest});
		const OverlapStats stats = measure(g.genre, 40, 128);
		expect(stats.handovers).to_be_greater_than(300);
		// ~333 per-mille (1 in 3).
		expect(stats.overlaps * 100).to_be_greater_than(stats.handovers * 25);
		expect(stats.overlaps * 100).to_be_less_than(stats.handovers * 42);
		// Both Lead->Arp and Arp->Lead overlap.
		expect(stats.leadToArp).to_be_greater_than(20);
		expect(stats.arpToLead).to_be_greater_than(20);
		// Only 1- and 2-bar overlaps, weighted 1:2 (2 bars ~67%).
		expect(stats.lengths.size()).to_equal(size_t{2});
		const int32_t one = stats.lengths.at(1);
		const int32_t two = stats.lengths.at(2);
		expect(one + two).to_equal(stats.overlaps);
		expect(two * 100).to_be_greater_than(stats.overlaps * 55);
		expect(two * 100).to_be_less_than(stats.overlaps * 78);
		// Never on or crossing a Section line.
		expect(stats.onSectionLine).to_equal(0);
		expect(stats.crossedSection).to_equal(0);
		// Deterministic for a given seed.
		expect(measure(g.genre, 5, 64).overlaps).to_equal(measure(g.genre, 5, 64).overlaps);
	});

	it("honours YieldSpec::overlapChance and YieldSpec::overlapBars as parameters", _ {
		const TestGenre never({.part = kLeadPart, .inDrops = YieldMode::Rest, .outside = YieldMode::Rest, .overlapChance = 0});
		expect(measure(never.genre, 10, 96).overlaps).to_equal(0);

		static constexpr WeightedBars kOneOnly[] = {{1, 1}};
		const TestGenre alwaysOne({.part = kLeadPart,
		                           .inDrops = YieldMode::Rest,
		                           .outside = YieldMode::Rest,
		                           .overlapChance = 1000,
		                           .overlapBars = kOneOnly});
		const OverlapStats s1 = measure(alwaysOne.genre, 8, 96);
		expect(s1.overlaps).to_equal(s1.handovers);
		expect(s1.lengths.size()).to_equal(size_t{1});
		expect(s1.lengths.at(1)).to_equal(s1.overlaps);

		static constexpr WeightedBars kTwoOnly[] = {{2, 1}};
		const TestGenre alwaysTwo({.part = kLeadPart,
		                           .inDrops = YieldMode::Rest,
		                           .outside = YieldMode::Rest,
		                           .overlapChance = 1000,
		                           .overlapBars = kTwoOnly});
		const OverlapStats s2 = measure(alwaysTwo.genre, 8, 96);
		expect(s2.overlaps).to_equal(s2.handovers);
		expect(s2.lengths.size()).to_equal(size_t{1});
		expect(s2.lengths.at(2)).to_equal(s2.overlaps);
	});

	it("overlaps even when the leaving part exits because its Density gate closed (Drop decay handover)", _ {
		// Lead gated at 31 with Drop lift +12 over 2 phrases: Lead enters on the Drop, plays 1 phrase, then its gate
		// closes inside the Groove handing the stage back to Arp. No drop-outs so the gate close is the only handover.
		static constexpr WeightedBars kTwoOnly[] = {{2, 1}};
		const TestGenre g({.part = kLeadPart,
		                   .inDrops = YieldMode::Rest,
		                   .outside = YieldMode::Rest,
		                   .overlapChance = 1000,
		                   .overlapBars = kTwoOnly},
		                  0, 31, 3);
		int32_t gateOverlaps = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			Recipe recipe = newRecipe(g.genre, seed);
			recipe.macros.set(Macro::Density, macroPoints(25));
			recipe.macros.set(Macro::Motion, macroPoints(25));
			const Walked walked = walk(recipe, seed, 96 * kStepsPerBar, {}, false);
			const std::vector<BarActivity> act = activityOf(walked);
			for (int32_t b = 1; b < 95; b++) {
				if (act[static_cast<size_t>(b)].section == SectionKind::Groove
				    && !act[static_cast<size_t>(b)].sectionStart && act[static_cast<size_t>(b - 1)].lead
				    && !act[static_cast<size_t>(b - 1)].arp && act[static_cast<size_t>(b)].lead
				    && act[static_cast<size_t>(b)].arp && act[static_cast<size_t>(b + 1)].lead
				    && act[static_cast<size_t>(b + 1)].arp) {
					gateOverlaps++;
				}
			}
		}
		expect(gateOverlaps).to_be_greater_than(0);
	});

	it("never overlaps on a Section line and cuts an active overlap immediately if a forced Section lands", _ {
		static constexpr WeightedBars kTwoOnly[] = {{2, 1}};
		const TestGenre g({.part = kLeadPart,
		                   .inDrops = YieldMode::Rest,
		                   .outside = YieldMode::Rest,
		                   .overlapChance = 1000,
		                   .overlapBars = kTwoOnly});
		// From Intro into Groove (bar 8) and Break/Build into Groove: never overlap on the Section line.
		const OverlapStats stats = measure(g.genre, 10, 96, false);
		expect(stats.onSectionLine).to_equal(0);
		expect(stats.crossedSection).to_equal(0);

		// A force queued once an overlap runs lands on the next phrase line, inside it: the overlap ends on that line,
		// the leaving Lead (not kept in the Break) plays no more and its Brightness snaps back there. Here the Lead
		// leaves by its gate (as the gate-close test below), for good, and an overlap of 12 bars spans the phrase line
		// after the handover.
		static constexpr WeightedBars kTwelve[] = {{12, 1}};
		const TestGenre gated({.part = kLeadPart,
		                       .inDrops = YieldMode::Rest,
		                       .outside = YieldMode::Rest,
		                       .overlapChance = 1000,
		                       .overlapBars = kTwelve},
		                      0, 31, 3);
		int32_t forced = 0;
		for (uint32_t seed = 1; seed <= 6; seed++) {
			Recipe recipe = newRecipe(gated.genre, seed);
			recipe.macros.set(Macro::Density, macroPoints(25));
			recipe.macros.set(Macro::Motion, macroPoints(25));
			const std::vector<BarActivity> act = activityOf(walk(recipe, seed, 96 * kStepsPerBar, {}, false));
			int32_t start = -1;
			for (int32_t b = 1; b + 1 < 96 && start < 0; b++) {
				const BarActivity& before = act[static_cast<size_t>(b - 1)];
				const BarActivity& first = act[static_cast<size_t>(b)];
				if (first.section == SectionKind::Groove && !first.sectionStart && before.lead && !before.arp
				    && first.lead && first.arp) {
					start = b;
				}
			}
			if (start < 0) {
				continue;
			}
			const int32_t lineBar = (start / gated.genre.phraseBars + 1) * gated.genre.phraseBars;
			if (lineBar >= start + 12 || !act[static_cast<size_t>(lineBar - 1)].lead
			    || !act[static_cast<size_t>(lineBar - 1)].arp) {
				continue; // the overlap must still run on the bar before the line
			}
			forced++;
			Run run{.recipe = recipe, .seed = seed};
			run.steer = steerAt(
			    {{start * kStepsPerBar + 2, {CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0}}});
			const Result steered = play(run, 96);
			const int32_t line = stepTick(lineBar * kStepsPerBar);
			bool breakOnLine = false;
			bool snapped = false;
			int32_t leadAfter = 0;
			for (const Event& e : steered.events) {
				breakOnLine =
				    breakOnLine || (e.kind == EventKind::Section && e.section == SectionKind::Break && e.tick == line);
				if (e.part != kLeadPart || e.tick < line || e.tick >= line + 8 * kBarTicks) {
					continue;
				}
				leadAfter += e.kind == EventKind::NoteOn ? 1 : 0;
				snapped = snapped
				          || (e.kind == EventKind::Control && e.control == SoundControl::Brightness && e.tick == line
				              && e.snap && e.controlOffset() >= -kControlFull);
			}
			expect(breakOnLine).to_be_true();
			expect(leadAfter).to_equal(0);
			expect(snapped).to_be_true();
		}
		expect(forced).to_be_greater_than(0);
	});

	it("keeps a leaving part's notes past its closing gate through the overlap, and cuts them where it ends", _ {
		// The Lead plays one note a bar, on its last 16th, a step and 6 ticks long (a Loop note's longest), so it lasts
		// over the bar line, and leaves by its gate (as the gate-close test above). The note entering the overlap's
		// first bar plays on; the gate cuts the one entering the bar after the overlap on its line (spec §5.6).
		static constexpr Pattern kLastSixteenth[] = {{"...............X", "................", 0}};
		static constexpr WeightedBars kTwoOnly[] = {{2, 1}};
		const TestGenre g({.part = kLeadPart,
		                   .inDrops = YieldMode::Rest,
		                   .outside = YieldMode::Rest,
		                   .overlapChance = 1000,
		                   .overlapBars = kTwoOnly},
		                  0, 31, 3, kLastSixteenth, 30);
		int32_t kept = 0;
		int32_t cut = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			Recipe recipe = newRecipe(g.genre, seed);
			recipe.macros.set(Macro::Density, macroPoints(25));
			recipe.macros.set(Macro::Motion, macroPoints(25));
			const Result res = play({.recipe = recipe, .seed = seed}, 96);
			std::map<int32_t, int32_t> leadOffAfterOn; // note-on tick -> its note-off tick
			std::map<int32_t, int32_t> arpBars;        // bar -> Arp note-ons
			std::map<uint8_t, int32_t> open;
			for (const Event& e : res.events) {
				if (e.kind == EventKind::NoteOn && e.part == kArpPart) {
					arpBars[e.tick / kBarTicks]++;
				}
				if (e.part != kLeadPart) {
					continue;
				}
				if (e.kind == EventKind::NoteOn) {
					open[e.pitch] = e.tick;
				}
				else if (e.kind == EventKind::NoteOff && open.contains(e.pitch)) {
					leadOffAfterOn[open[e.pitch]] = e.tick;
					open.erase(e.pitch);
				}
			}
			auto leadOnIn = [&](int32_t bar) {
				const auto found = leadOffAfterOn.lower_bound(bar * kBarTicks);
				return found != leadOffAfterOn.end() && found->first < (bar + 1) * kBarTicks ? found : leadOffAfterOn.end();
			};
			for (int32_t b = 2; b + 2 < 96; b++) {
				// A handover overlap: the Lead's notes in b-1, b and b+1 with the Arp only from b, none from the Lead in
				// b+2 (its 2 bars are over), and the notes entering b and b+2 both start a bar before.
				const auto before = leadOnIn(b - 1);
				const auto last = leadOnIn(b + 1);
				if (arpBars.contains(b - 1) || !arpBars.contains(b) || before == leadOffAfterOn.end()
				    || leadOnIn(b) == leadOffAfterOn.end() || last == leadOffAfterOn.end()
				    || leadOnIn(b + 2) != leadOffAfterOn.end()) {
					continue;
				}
				kept += before->second > b * kBarTicks ? 1 : 0;
				expect(before->second).to_be_greater_than(b * kBarTicks);
				cut += last->second == (b + 2) * kBarTicks ? 1 : 0;
				expect(last->second).to_equal((b + 2) * kBarTicks);
			}
		}
		expect(kept).to_be_greater_than(0);
		expect(cut).to_equal(kept);
	});

	it("eases Brightness deeply closed to -200, holds 1 beat, snaps back, and leaves volume and velocities untouched", _ {
		static constexpr WeightedBars kTwoOnly[] = {{2, 1}};
		const TestGenre g({.part = kLeadPart,
		                   .inDrops = YieldMode::Rest,
		                   .outside = YieldMode::Rest,
		                   .overlapChance = 1000,
		                   .overlapBars = kTwoOnly});
		Recipe recipe = newRecipe(g.genre, 1);
		recipe.macros.set(Macro::Motion, macroPoints(25));
		const Result res = play({.recipe = recipe, .seed = 1, .skipIntro = true}, 48);
		const Walked walked = walk(recipe, 1, 48 * kStepsPerBar, {}, true);
		const std::vector<BarActivity> act = activityOf(walked);

		int32_t startBar = -1;
		uint8_t leavingPart = 0;
		for (int32_t b = 1; b < 46; b++) {
			if (act[static_cast<size_t>(b)].section == SectionKind::Groove && act[static_cast<size_t>(b)].lead
			    && act[static_cast<size_t>(b)].arp) {
				startBar = b;
				leavingPart = act[static_cast<size_t>(b - 1)].lead ? kLeadPart : kArpPart;
				break;
			}
		}
		expect(startBar).to_be_greater_than(0);
		const int32_t startStep = startBar * kStepsPerBar;
		const int32_t endStep = (startBar + 2) * kStepsPerBar;
		const int32_t holdEnd = endStep + kStepsPerBeat;
		const std::vector<int32_t> bri = brightnessCurve(res.events, leavingPart, 48 * kStepsPerBar);

		const int32_t normal = bri[static_cast<size_t>(startStep)];
		const int32_t mid = bri[static_cast<size_t>(startStep + kStepsPerBar)];
		const int32_t end = bri[static_cast<size_t>(endStep)];
		const int32_t held = bri[static_cast<size_t>(holdEnd - 1)];
		const int32_t reopened = bri[static_cast<size_t>(holdEnd)];

		constexpr int32_t kDeep = -(kControlFull + kCloseControlExtra);
		// Reaches deep close (-200) by overlap end, and quadratic ease-out is already past 70% of the fall at halfway.
		expect(end).to_equal(kDeep);
		expect(normal - mid).to_be_greater_than((normal - kDeep) * 70 / 100);
		// Holds closed for 1 beat after the overlap ends, then snaps back to normal.
		expect(held).to_equal(kDeep);
		expect(std::abs(reopened - normal)).to_be_less_than(15);

		bool foundReopenSnap = false;
		for (const Event& e : res.events) {
			if (e.kind == EventKind::Control && e.part == leavingPart && e.control == SoundControl::Brightness
			    && e.tick == stepTick(holdEnd) && e.snap) {
				foundReopenSnap = true;
			}
		}
		expect(foundReopenSnap).to_be_true();

		// Volume stays steady: note velocities during the overlap match the bar before it (same Energy, zero spread).
		std::set<uint8_t> beforeVels;
		std::set<uint8_t> overlapVels;
		for (const Event& e : res.events) {
			if (e.kind == EventKind::NoteOn && e.part == leavingPart) {
				const int32_t b = e.tick / (kStepsPerBar * kTicksPerStep);
				if (b == startBar - 1) {
					beforeVels.insert(e.velocity);
				}
				else if (b == startBar || b == startBar + 1) {
					overlapVels.insert(e.velocity);
				}
			}
		}
		expect(overlapVels.empty()).to_be_false();
		expect(overlapVels == beforeVels).to_be_true();
	});

	it("never flip-flops the pair bar-by-bar and keeps Compose and ArcPlan in agreement with Perform", _ {
		const TestGenre g({.part = kLeadPart, .inDrops = YieldMode::Rest, .outside = YieldMode::Rest, .overlapChance = 1000});
		Recipe recipe = newRecipe(g.genre, 3);
		recipe.macros.set(Macro::Motion, macroPoints(25));

		// Perform vs Compose parity (notes and extended Brightness curve down to -200).
		Generator gen;
		ComposeDriver driver;
		RecordingWriter writer;
		driver.begin(gen, recipe, 3, {}, 32 * kStepsPerBar, writer, true);
		while (!driver.run(64)) {
		}
		const int32_t endStep = writer.endTick / kTicksPerStep;
		const Result performed = play({.recipe = recipe, .seed = 3, .skipIntro = true}, endStep / kStepsPerBar);
		const int32_t cutStep = std::min(endStep - 8 * kStepsPerBar, 24 * kStepsPerBar);
		const auto curves = performCurves(performed.events, cutStep);
		bool sawDeepClose = false;
		for (const auto& [key, expected] : curves) {
			const Automated id{std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key)};
			const std::vector<Node> nodes = writer.nodesOf(id);
			int32_t base = 0;
			for (const auto& [bid, val] : writer.bases) {
				if (bid == id) {
					base = val;
				}
			}
			size_t from = 0;
			for (int32_t s = 0; s <= cutStep; s++) {
				const int32_t actual = nodeValue(nodes, base, stepTick(s), from);
				sawDeepClose = sawDeepClose || actual <= -180;
				expect(std::abs(actual - expected[static_cast<size_t>(s)])).to_be_less_than(ClipBuilder::kBarTolerance + 2);
			}
		}
		expect(sawDeepClose).to_be_true();
		// The close past -kControlFull has its own lane, Brightness's extra one (the firmware's treble shelf follows it
		// alone, perform::onExtraLane()), in percent past -kControlFull.
		int32_t closeNodes = 0;
		for (const auto& [id, node] : writer.nodes) {
			if (id.control == SoundControl::Brightness && id.extra) {
				closeNodes++;
				expect(node.value >= 0 && node.value <= kCloseControlExtra).to_be_true();
			}
		}
		expect(closeNodes).to_be_greater_than(0);

		// ArcPlan matches PhraseHistory over 8 phrases.
		StepwisePiece<> piece({.recipe = recipe, .seed = 3, .skipIntro = true});
		ArcPlan plan;
		piece.generator.planArc(plan);
		for (int32_t phrase = 0; phrase < 8; phrase++) {
			piece.decideBefore((phrase + 1) * 8 * kStepsPerBar + 1);
			ArcPhrase heard;
			expect(piece.driver.history().at(phrase, heard)).to_be_true();
			expect(plan.at(phrase)->sounds).to_equal(heard.sounds);
		}
	});

	it("gives no lane to the close past -kControlFull where no handover overlaps (spec §15.6)", _ {
		// Psytrance as written (dropboth): the pair plays together, so nothing overlaps, and no Brightness extra lane
		// starts: the firmware's treble shelf, which only that lane writes, gets no automation.
		for (uint32_t seed = 1; seed <= 3; seed++) {
			const Recipe recipe = withFirstVariants(newRecipe(genres::psytranceClassic, seed));
			Generator gen;
			ComposeDriver driver;
			RecordingWriter writer;
			driver.begin(gen, recipe, seed, {}, 160 * kStepsPerBar, writer, false);
			while (!driver.run(256)) {
			}
			int32_t closeLanes = 0;
			for (const auto& [id, node] : writer.nodes) {
				closeLanes += id.control == SoundControl::Brightness && id.extra ? 1 : 0;
			}
			for (const auto& [id, value] : writer.bases) {
				closeLanes += id.control == SoundControl::Brightness && id.extra ? 1 : 0;
			}
			expect(closeLanes).to_equal(0);
		}
	});
});

CPPSPEC_SPEC(handover_overlap)
