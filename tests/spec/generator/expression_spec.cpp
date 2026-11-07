// The expression stage (spec §8.2, §8.4, §8.5): Velocity spread, Lean and Ornaments on the notes that survived
// Density, from per-part and per-lane genre settings.

#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <tuple>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kOnTheBeat[] = {{"X...X...X...X...", "................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// One-bar phrases and no Arc offsets: the Intro is bar 0, the Groove bars 1–4, so bars 1 and 5 are Section lines.
constexpr Genre streamGenre(std::span<const PartSpec> parts) {
	return {
	    .name = "expression",
	    .facts = {.scales = kScales},
	    .phraseBars = 1,
	    .parts = parts,
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {4, 4},
	            .buildBars = kBuildBars,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	        },
	};
}

/// A Drums part of one lane, playing `pool` with `expression`.
template <const Pattern* Pool, size_t PoolSize, Expression Expr, uint8_t Gate = 6>
struct OneLane {
	static constexpr LaneSpec lanes[] = {{.lane = Lane::ClosedHat,
	                                      .pool = std::span<const Pattern>(Pool, PoolSize),
	                                      .gateTicks = Gate,
	                                      .expression = Expr}};
	static constexpr PartSpec parts[] = {{.role = Role::Drums, .lanes = lanes}};
	static constexpr Genre genre = streamGenre(parts);
};
template <Expression Expr, uint8_t Gate = 6>
using EveryStep = OneLane<kEveryStep, 1, Expr, Gate>;
template <Expression Expr, uint8_t Gate = 6>
using OnTheBeat = OneLane<kOnTheBeat, 1, Expr, Gate>;

Run runOf(const Genre& genre, uint32_t seed = 1, int32_t complexity = 25) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, 0);
	run.seed = seed;
	return run;
}

std::vector<uint8_t> barVelocities(const Result& result, int32_t bar) {
	std::vector<uint8_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.tick >= bar * kBarTicks && e.tick < (bar + 1) * kBarTicks) {
			out.push_back(e.velocity);
		}
	}
	return out;
}

/// Each note-on's offset from its step's grid line, by step (the nearest grid line).
std::map<int32_t, int32_t> offsets(const Result& result, bool ornaments = false) {
	std::map<int32_t, int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.ornament == ornaments) {
			int32_t step = (e.tick + kTicksPerStep / 2) / kTicksPerStep;
			out[step] = e.tick - stepTick(step);
		}
	}
	return out;
}

/// Every note-on gets a note-off after it, and no note-off is stray (spec §21.2).
bool notesPair(const Result& result) {
	std::map<std::tuple<uint8_t, Lane, uint8_t>, int32_t> sounding;
	for (const Event& e : result.events) {
		auto key = std::make_tuple(e.part, e.lane, e.pitch);
		if (e.kind == EventKind::NoteOn) {
			if (sounding.count(key) != 0) {
				return false;
			}
			sounding[key] = e.tick;
		}
		else if (e.kind == EventKind::NoteOff) {
			auto found = sounding.find(key);
			if (found == sounding.end() || e.tick <= found->second) {
				return false;
			}
			sounding.erase(found);
		}
	}
	return sounding.empty();
}

/// Note-ons from tick `from` to `to`, exclusive.
std::vector<Event> onsBetween(const Result& result, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.tick >= from && e.tick < to) {
			out.push_back(e);
		}
	}
	return out;
}

/// The note-off that ends the note `on` started.
int32_t offTickOf(const Result& result, const Event& on) {
	for (const Event& e : only(result.events, EventKind::NoteOff)) {
		if (e.sameKey(on) && e.tick > on.tick) {
			return e.tick;
		}
	}
	return -1;
}

constexpr Ornaments always(uint8_t two, uint8_t three, uint8_t four, uint8_t flam = 0, uint8_t gap = 3) {
	return {.rateFloor = 1000, .rateCeiling = 1000, .ratchet = {two, three, four}, .flam = flam, .flamGap = gap};
}

} // namespace

// clang-format off
describe expression("The expression stage", $ {
	it("nudges velocities within the spread, repeating with the pattern in Repeat mode", _ {
		Result plain = play(runOf(EveryStep<Expression{}>::genre), 4);
		Result spread = play(runOf(EveryStep<Expression{.spread = 20}>::genre), 4);
		auto flat = barVelocities(plain, 2);
		auto nudged = barVelocities(spread, 2);
		expect(nudged.size()).to_equal(flat.size());
		std::set<int32_t> nudges;
		for (size_t i = 0; i < flat.size(); i++) {
			int32_t nudge = nudged[i] - flat[i];
			expect(nudge).to_be_between(-20, 20);
			nudges.insert(nudge);
		}
		expect(nudges.size()).to_be_greater_than(size_t{4});
		expect(barVelocities(spread, 3) == nudged).to_be_true();
	});

	it("draws a fresh nudge on every pass in Fresh mode", _ {
		Result spread = play(runOf(EveryStep<Expression{.spread = 20, .spreadMode = SpreadMode::Fresh}>::genre), 4);
		expect(barVelocities(spread, 3) == barVelocities(spread, 2)).to_be_false();
		Result plain = play(runOf(EveryStep<Expression{}>::genre), 4);
		for (size_t i = 0; i < barVelocities(plain, 2).size(); i++) {
			expect(barVelocities(spread, 2)[i] - barVelocities(plain, 2)[i]).to_be_between(-20, 20);
		}
	});

	it("moves each 16th of the beat by its Lean, but never a Section's first step earlier", _ {
		Run run = runOf(EveryStep<Expression{.lean = {-6, 3, 11, -2}}>::genre);
		run.ticksPerCall = 1; // so the run stops between note-ons, not on one
		Result result = play(run, 3);
		auto shifted = offsets(result);
		expect(shifted.size()).to_equal(size_t{3 * kStepsPerBar + 1}); // and the next bar's first, pushed 6 early
		for (auto [step, offset] : shifted) {
			bool sectionStart = step == 0 || step == kStepsPerBar;
			int32_t lean = std::vector<int32_t>{-6, 3, 11, -2}[step % kStepsPerBeat];
			expect(offset).to_equal(sectionStart ? 0 : lean);
		}
		expect(notesPair(result)).to_be_true();
	});

	it("clamps Lean to -12..+12", _ {
		std::set<int32_t> ticks;
		for (const Event& e : only(play(runOf(EveryStep<Expression{.lean = {0, 30, 0, -30}}>::genre), 1).events,
		                           EventKind::NoteOn)) {
			ticks.insert(e.tick);
		}
		expect(ticks.count(stepTick(1) + 12)).to_equal(size_t{1});
		expect(ticks.count(stepTick(3) - 12)).to_equal(size_t{1});
	});

	it("moves a whole note, keeping its length", _ {
		static constexpr PartSpec kParts[] = {{
		    .role = Role::Bass,
		    .flavour = Flavour::Loop,
		    .pool = kOnTheBeat,
		    .loop = {.degrees = kRoot, .octaveUp = 0, .slide = 0, .gateTicks = 16},
		    .expression = {.lean = {5, 0, 0, 0}},
		}};
		static constexpr Genre kGenre = streamGenre(kParts);
		Result result = play(runOf(kGenre), 3);
		std::map<int32_t, int32_t> on;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn && e.tick >= 2 * kBarTicks) {
				expect(e.tick % (4 * kTicksPerStep)).to_equal(5);
				on[e.pitch] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff && on.count(e.pitch) != 0) {
				expect(e.tick - on[e.pitch]).to_equal(16);
				on.erase(e.pitch);
			}
		}
	});

	it("ratchets a note into 2, 3 or 4 even hits inside its 16th, each softer, the later ones Ornament hits", _ {
		const int32_t beat = stepTick(4);
		Result two = play(runOf(OnTheBeat<Expression{.ornaments = always(1, 0, 0)}>::genre), 1);
		Result three = play(runOf(OnTheBeat<Expression{.ornaments = always(0, 1, 0)}>::genre), 1);
		Result four = play(runOf(OnTheBeat<Expression{.ornaments = always(0, 0, 1)}>::genre), 1);
		for (auto [result, spacing, hits] : {std::tuple{&two, 12, 2}, std::tuple{&three, 8, 3}, std::tuple{&four, 6, 4}}) {
			auto ons = onsBetween(*result, beat, beat + kTicksPerStep);
			expect(static_cast<int32_t>(ons.size())).to_equal(hits);
			for (int32_t i = 0; i < static_cast<int32_t>(ons.size()); i++) {
				expect(ons[i].tick).to_equal(beat + i * spacing);
				expect(ons[i].ornament).to_equal(i > 0);
				expect(offTickOf(*result, ons[i])).to_equal(ons[i].tick + (spacing < 6 ? spacing : 6));
				if (i > 0) {
					expect(ons[i].velocity).to_be_less_than(ons[i - 1].velocity);
				}
			}
			expect(notesPair(*result)).to_be_true();
		}
	});

	it("gives each ratchet hit the spacing, or the note's length if shorter", _ {
		const int32_t beat = stepTick(4);
		Result result = play(runOf(OnTheBeat<Expression{.ornaments = always(1, 0, 0)}, 4>::genre), 1);
		for (const Event& on : onsBetween(result, beat, beat + kTicksPerStep)) {
			expect(offTickOf(result, on)).to_equal(on.tick + 4);
		}
	});

	it("keeps a ratchet's spacing from its Lean-shifted start, shrinking it so the last hit stays in the step", _ {
		const int32_t beat = stepTick(4);
		Result result = play(runOf(OnTheBeat<Expression{.lean = {12, 0, 0, 0}, .ornaments = always(0, 0, 1)}>::genre), 1);
		auto ons = onsBetween(result, beat, beat + 2 * kTicksPerStep);
		std::vector<int32_t> ticks;
		for (const Event& e : ons) {
			ticks.push_back(e.tick - beat);
		}
		expect(ticks == std::vector<int32_t>{12, 15, 18, 21}).to_be_true();
		expect(notesPair(result)).to_be_true();
	});

	it("flams a note: a soft grace hit a gap before the main hit, which stays at its Lean position", _ {
		const int32_t beat = stepTick(4);
		Result result = play(runOf(OnTheBeat<Expression{.lean = {-3, 0, 0, 0}, .ornaments = always(0, 0, 0, 1, 4)}>::genre), 1);
		auto ons = onsBetween(result, beat - kMaxLean, beat + kTicksPerStep);
		expect(ons.size()).to_equal(size_t{2});
		ons.resize(2);
		expect(ons[0].tick).to_equal(beat - 7);
		expect(ons[0].ornament).to_be_true();
		expect(ons[0].velocity).to_be_less_than(ons[1].velocity);
		expect(ons[1].tick).to_equal(beat - 3);
		expect(ons[1].ornament).to_be_false();
		expect(notesPair(result)).to_be_true();
	});

	it("never lands a flam's grace hit before -12, or before the line on a Section's first step", _ {
		Run run = runOf(OnTheBeat<Expression{.lean = {-9, 0, 0, 0}, .ornaments = always(0, 0, 0, 1, 6)}>::genre);
		run.ticksPerCall = 1;
		Result result = play(run, 2);
		for (int32_t beat = 0; beat < 2 * kStepsPerBar; beat += kStepsPerBeat) {
			auto ons = onsBetween(result, stepTick(beat) - kMaxLean, stepTick(beat) + kTicksPerStep);
			if (beat == 0 || beat == kStepsPerBar) {
				// Lean clamps to 0 and the grace hit has no room before the line: a single plain hit.
				expect(ons.size()).to_equal(size_t{1});
				ons.resize(1);
				expect(ons[0].tick).to_equal(stepTick(beat));
				expect(ons[0].ornament).to_be_false();
			}
			else {
				// The gap shrinks from 6 to 3; the main hit stays at -9.
				expect(ons.size()).to_equal(size_t{2});
				ons.resize(2);
				expect(ons[0].tick).to_equal(stepTick(beat) - kMaxLean);
				expect(ons[1].tick).to_equal(stepTick(beat) - 9);
			}
		}
		expect(notesPair(result)).to_be_true();
	});

	it("plays Ornaments more often as Complexity rises, between the genre's floor and ceiling", _ {
		constexpr Expression kRising{.ornaments = {.rateFloor = 0, .rateCeiling = 1000, .ratchet = {1, 0, 0}}};
		constexpr Expression kFlat{.ornaments = {.rateFloor = 300, .rateCeiling = 300, .ratchet = {1, 0, 0}}};
		auto ornamented = [](const Genre& genre, int32_t complexity) {
			int32_t count = 0;
			for (uint32_t seed = 1; seed <= 20; seed++) {
				for (const Event& e : only(play(runOf(genre, seed, complexity), 1).events, EventKind::NoteOn)) {
					count += e.ornament ? 1 : 0;
				}
			}
			return count; // of 20 × 16 notes
		};
		expect(ornamented(EveryStep<kRising>::genre, 0)).to_equal(0);
		expect(ornamented(EveryStep<kRising>::genre, 50)).to_equal(20 * 16);
		expect(ornamented(EveryStep<kRising>::genre, 25)).to_be_between(20 * 16 * 35 / 100, 20 * 16 * 65 / 100);
		for (int32_t complexity : {0, 50}) {
			expect(ornamented(EveryStep<kFlat>::genre, complexity)).to_be_between(20 * 16 * 18 / 100, 20 * 16 * 42 / 100);
		}
	});

	it("re-rolls Ornaments with the take, and repeats them with the pattern", _ {
		constexpr Expression kHalf{.ornaments = {.rateFloor = 500, .rateCeiling = 500, .ratchet = {1, 0, 0}}};
		auto ornamentedSteps = [](const Result& result, int32_t bar) {
			std::set<int32_t> out;
			for (const Event& e : onsBetween(result, bar * kBarTicks, (bar + 1) * kBarTicks)) {
				if (e.ornament) {
					out.insert((e.tick % kBarTicks) / kTicksPerStep);
				}
			}
			return out;
		};
		int32_t differ = 0;
		for (uint32_t seed = 1; seed <= 10; seed++) {
			Run run = runOf(EveryStep<kHalf>::genre, seed);
			Result first = play(run, 4);
			expect(ornamentedSteps(first, 3) == ornamentedSteps(first, 2)).to_be_true();
			run.takes.take[0] = 1;
			differ += ornamentedSteps(play(run, 3), 2) != ornamentedSteps(first, 2) ? 1 : 0;
		}
		expect(differ).to_be_greater_than(7);
	});

	it("never starts a hit before an earlier step's hit on the same key", _ {
		// The & ratchets to its step's end, and the a leans 12 early into it; the & flams late and the a's grace hit
		// leans back across it.
		static constexpr Pattern kAndA[] = {{"..XX..XX..XX..XX", "................", 0}};
		static constexpr Expression kRatchets{.lean = {0, 0, 0, -12}, .ornaments = always(0, 0, 1)};
		static constexpr Expression kFlams{.lean = {0, 0, 10, -10}, .ornaments = always(0, 0, 0, 1, 6)};
		for (const Genre* genre : {&OneLane<kAndA, 1, kRatchets>::genre, &OneLane<kAndA, 1, kFlams>::genre}) {
			Run run = runOf(*genre);
			run.ticksPerCall = 1;
			Result result = play(run, 2);
			expect(notesPair(result)).to_be_true();
			int32_t last = -1;
			for (const Event& e : only(result.events, EventKind::NoteOn)) {
				expect(e.tick).to_be_greater_than(last);
				last = e.tick;
			}
		}
	});
});

CPPSPEC_SPEC(expression)
