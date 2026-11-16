// The Arp flavour (spec §5.2, §8.1, §7.3): the current chord's notes, with its Extensions, walked in an order over an
// octave span on the onsets of a tiered rhythm pattern. The walk position counts the pattern's onsets since the pass
// start, before Density thinning, so thinning never shifts a note.

#include "chord_moves.h"
#include "core_walk.h"
#include "generative/core/chord_walk.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// 4-bar phrases and no Arc offsets, so the Recipe's macros are what plays. runOf() starts in Groove at Motion 0, so
/// the Groove never ends and bar 0 is the only Section line. An empty Harmony slot: the home chord throughout, a plain
/// triad unless `ceiling` allows Extensions.
constexpr Genre arpGenre(std::span<const PartSpec> parts, uint8_t floor = 0, uint8_t ceiling = 0) {
	return {
	    .name = "arp",
	    .facts = {.scales = kScales},
	    .phraseBars = 4,
	    .parts = parts,
	    .harmony = {.extensionFloor = floor, .extensionCeiling = ceiling},
	    .arc =
	        {
	            .introPhrases = 1,
	            .groovePhrases = {4, 4},
	            .breakPhrases = {1, 1},
	            .buildBars = kBuildBars,
	            .dropLiftEnergy = 0,
	            .dropLiftDensity = 0,
	            .offsets = {},
	        },
	};
}

/// A minor's tonic at A3 (57), the span from E♭3 (51) to C♯6 (85): two octaves of the walk from any chord root.
constexpr Register kArpRegister{.anchor = 57, .spanLow = -6, .spanHigh = 28};

constexpr Pattern kEvery16th[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};

/// A Lead Arp on every 16th, walking in `Order` over `Octaves` octaves.
template <ArpOrder Order, uint8_t Octaves, uint8_t Floor = 0, uint8_t Ceiling = 0>
struct OneArp {
	static constexpr ArpWalk walks[] = {{.order = Order, .octaves = Octaves}};
	static constexpr PartSpec parts[] = {{.role = Role::Lead,
	                                      .flavour = Flavour::Arp,
	                                      .name = "ARP",
	                                      .pool = kEvery16th,
	                                      .reg = kArpRegister,
	                                      .arp = {.walks = walks, .gateTicks = 18}}};
	static constexpr Genre genre = arpGenre(parts, Floor, Ceiling);
};

Run runOf(const Genre& genre, int32_t density = 50, uint32_t seed = 1) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.keyRoot = 9; // A
	run.recipe.scale = Scale::Minor;
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Motion, 0);
	run.seed = seed;
	run.skipIntro = true;
	return run;
}

std::vector<Event> notes(const Result& result, EventKind kind) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, kind)) {
		if (e.lane == Lane::None) {
			out.push_back(e);
		}
	}
	return out;
}

/// The pitch of each note-on, by its step.
std::map<int32_t, int32_t> pitchByStep(const Result& result) {
	std::map<int32_t, int32_t> out;
	for (const Event& e : notes(result, EventKind::NoteOn)) {
		out[stepOf(e)] = e.pitch;
	}
	return out;
}

/// The pitches of bar `bar`'s note-ons, in order.
std::vector<int32_t> barPitches(const Result& result, int32_t bar) {
	std::vector<int32_t> out;
	for (const Event& e : notes(result, EventKind::NoteOn)) {
		if (e.tick >= bar * kBarTicks && e.tick < (bar + 1) * kBarTicks) {
			out.push_back(e.pitch);
		}
	}
	return out;
}

/// `walk` repeated over a bar of 16ths.
std::vector<int32_t> cycled(const std::vector<int32_t>& walk) {
	std::vector<int32_t> out;
	for (int32_t i = 0; i < kStepsPerBar; i++) {
		out.push_back(walk[i % walk.size()]);
	}
	return out;
}

constexpr int32_t kStep = kTicksPerStep;

/// A chord a bar, i and iv in turn.
constexpr ChordMove kTurns[] = {{kI, kIV, 100}, {kIV, kI, 100}};
constexpr ChordGraph kTurnGraph{.moves = kTurns, .barsPerChord = 1};
constexpr Genre kTurningGenre = [] {
	Genre genre = OneArp<ArpOrder::Up, 2>::genre;
	genre.harmony.graph = &kTurnGraph;
	return genre;
}();

/// Anchors on each beat, and 16ths Density thins in between.
constexpr Pattern kThinned[] = {{"X375X573X735X357", "................", 0}};
constexpr ArpWalk kUpTwo[] = {{.order = ArpOrder::Up, .octaves = 2}};
constexpr ArpWalk kRandomTwo[] = {{.order = ArpOrder::Random, .octaves = 2}};
constexpr PartSpec kThinnedParts[] = {{.role = Role::Lead,
                                       .flavour = Flavour::Arp,
                                       .name = "ARP",
                                       .pool = kThinned,
                                       .reg = kArpRegister,
                                       .arp = {.walks = kUpTwo}}};
constexpr Genre kThinnedGenre = arpGenre(kThinnedParts);
constexpr PartSpec kThinnedRandomParts[] = {{.role = Role::Lead,
                                             .flavour = Flavour::Arp,
                                             .name = "ARP",
                                             .pool = kThinned,
                                             .reg = kArpRegister,
                                             .arp = {.walks = kRandomTwo}}};
constexpr Genre kThinnedRandomGenre = arpGenre(kThinnedRandomParts);

/// Onsets on the 1, two 16ths on, then one on the bar's last 16th: a long gate ends at each next onset or the line.
constexpr Pattern kGaps[] = {{"X...X.X........X", "................", 0}};
constexpr PartSpec kGapParts[] = {{.role = Role::Lead,
                                   .flavour = Flavour::Arp,
                                   .name = "ARP",
                                   .pool = kGaps,
                                   .reg = kArpRegister,
                                   .arp = {.walks = kUpTwo, .gateTicks = 72}}};
constexpr Genre kGapGenre = arpGenre(kGapParts);

/// Three tiers, each walking its own way: Up over one octave, Down over one, Up-down over two.
constexpr Pattern kTiered[] = {
    {"XXXXXXXXXXXXXXXX", "................", 0},
    {"XXXXXXXXXXXXXXXX", "................", 1},
    {"XXXXXXXXXXXXXXXX", "................", 2},
};
constexpr ArpWalk kTieredWalks[] = {
    {.order = ArpOrder::Up, .octaves = 1},
    {.order = ArpOrder::Down, .octaves = 1},
    {.order = ArpOrder::UpDown, .octaves = 2},
};
constexpr PartSpec kTieredParts[] = {{.role = Role::Lead,
                                      .flavour = Flavour::Arp,
                                      .name = "ARP",
                                      .pool = kTiered,
                                      .band = {0, 2},
                                      .reg = kArpRegister,
                                      .arp = {.walks = kTieredWalks}}};
constexpr Genre kTieredGenre = arpGenre(kTieredParts);

/// Whether a genre of one Lead Arp part with `walks`, `gate`, `reg` and `expression` over `pool` passes
/// arpNotesValid().
bool arpValid(std::span<const Pattern> pool, std::span<const ArpWalk> walks, uint16_t gate, Register reg,
              const Expression& expression = {}) {
	const PartSpec parts[] = {{.role = Role::Lead,
	                           .flavour = Flavour::Arp,
	                           .pool = pool,
	                           .reg = reg,
	                           .arp = {.walks = walks, .gateTicks = gate},
	                           .expression = expression}};
	return arpNotesValid(Genre{.name = "arp", .parts = parts});
}

} // namespace

// clang-format off
describe arp("The Arp flavour", $ {
	context("walking the chord (spec §5.2, §8.1)", _ {
		it("walks the chord's notes up, one a 16th, from the root's lowest place in the Register", _ {
			// A minor's i: A C E from A3, the lowest A at or above E♭3.
			Result result = play(runOf(OneArp<ArpOrder::Up, 1>::genre), 2);
			expect(barPitches(result, 0) == cycled({57, 60, 64})).to_be_true();
			expect(result.evictedNotes).to_equal(0U);
			expect(result.droppedEvents).to_equal(0U);
		});

		it("walks down, and up and down without repeating the ends", _ {
			Result down = play(runOf(OneArp<ArpOrder::Down, 1>::genre), 1);
			expect(barPitches(down, 0) == cycled({64, 60, 57})).to_be_true();
			Result upDown = play(runOf(OneArp<ArpOrder::UpDown, 2>::genre), 1);
			expect(barPitches(upDown, 0) == cycled({57, 60, 64, 69, 72, 76, 72, 69, 64, 60})).to_be_true();
		});

		it("climbs as many octaves as its pool entry spans", _ {
			Result result = play(runOf(OneArp<ArpOrder::Up, 2>::genre), 1);
			expect(barPitches(result, 0) == cycled({57, 60, 64, 69, 72, 76})).to_be_true();
		});

		it("starts the walk again at each pass of its pattern, keeping no state", _ {
			// 16 onsets over a walk of 6: a walk that carried on would start bar 1 on its 5th note.
			Result result = play(runOf(OneArp<ArpOrder::Up, 2>::genre), 3);
			expect(barPitches(result, 1) == barPitches(result, 0)).to_be_true();
			expect(barPitches(result, 2) == barPitches(result, 0)).to_be_true();
		});

		it("walks the chord's Extensions too", _ {
			// A minor's i at Extension level 2: A C G B (the 9th drops the 5th), then an octave up.
			Result result = play(runOf(OneArp<ArpOrder::Up, 2, 2, 2>::genre), 1);
			expect(barPitches(result, 0) == cycled({57, 60, 67, 71, 69, 72, 79, 83})).to_be_true();
		});

		it("takes the chord's Extensions at the bar line, so a Complexity turn mid-bar waits for the next", _ {
			// Complexity 0 plays the triad; turned up to 50 on the bar's fifth 16th, the 9th chord comes at bar 1.
			Run run = runOf(OneArp<ArpOrder::Up, 2, 0, 2>::genre);
			run.recipe.macros.set(Macro::Complexity, 0);
			run.steer = steerAt({{4, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Complexity), macroPoints(50)}}});
			Result result = play(run, 2);
			expect(barPitches(result, 0) == cycled({57, 60, 64, 69, 72, 76})).to_be_true();
			expect(barPitches(result, 1) == cycled({57, 60, 67, 71, 69, 72, 79, 83})).to_be_true();
		});

		it("follows each chord change on its bar line", _ {
			// i, then iv: D F A from D4 (62), the lowest D at or above E♭3.
			Result result = play(runOf(kTurningGenre), 2);
			expect(barPitches(result, 0) == cycled({57, 60, 64, 69, 72, 76})).to_be_true();
			expect(barPitches(result, 1) == cycled({62, 65, 69, 74, 77, 81})).to_be_true();
		});

		it("draws a Random walk once per onset from the chord's notes, the same on every pass of the pick", _ {
			Result result = play(runOf(kThinnedRandomGenre), 3);
			const std::set<int32_t> walk{57, 60, 64, 69, 72, 76};
			std::set<int32_t> heard;
			for (const Event& e : notes(result, EventKind::NoteOn)) {
				expect(walk.count(e.pitch)).to_equal(size_t{1});
				heard.insert(e.pitch);
			}
			expect(heard.size()).to_be_greater_than(size_t{3});
			expect(barPitches(result, 1) == barPitches(result, 0)).to_be_true();
			// Another take draws another walk.
			Run other = runOf(kThinnedRandomGenre);
			other.takes.take[0] = 1;
			expect(barPitches(play(other, 1), 0) != barPitches(result, 0)).to_be_true();
		});

		it("takes each pool entry's own order and octave span with its tier", _ {
			const std::vector<int32_t> up = cycled({57, 60, 64});
			const std::vector<int32_t> down = cycled({64, 60, 57});
			const std::vector<int32_t> upDown = cycled({57, 60, 64, 69, 72, 76, 72, 69, 64, 60});
			std::set<int32_t> seen; // 0 Up, 1 Down, 2 Up-down
			for (int32_t complexity : {0, 25, 50}) {
				Run run = runOf(kTieredGenre);
				run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
				const std::vector<int32_t> bar = barPitches(play(run, 1), 0);
				expect(bar == up || bar == down || bar == upDown).to_be_true();
				seen.insert(bar == up ? 0 : bar == down ? 1 : 2);
			}
			expect(seen == std::set<int32_t>{0, 1, 2}).to_be_true();
		});
	});

	context("under Density (spec §8.1)", _ {
		it("never shifts a note when Density thins it: each step plays the pitch it plays at full Density", _ {
			for (const Genre* genre : {&kThinnedGenre, &kThinnedRandomGenre}) {
				const std::map<int32_t, int32_t> full = pitchByStep(play(runOf(*genre, 50), 2));
				expect(full.size()).to_equal(size_t{2 * kStepsPerBar});
				for (int32_t density : {0, 15, 25, 35}) {
					const std::map<int32_t, int32_t> thinned = pitchByStep(play(runOf(*genre, density), 2));
					expect(thinned.size()).to_be_less_than(full.size());
					expect(thinned.size()).to_be_greater_than(size_t{7}); // the anchors at least
					for (const auto& [step, pitch] : thinned) {
						expect(full.at(step)).to_equal(pitch);
					}
				}
			}
		});
	});

	context("note lengths (spec §5.2, §2.7)", _ {
		it("lasts its gate, ending sooner at the pattern's next onset or the bar line", _ {
			Result result = play(runOf(kGapGenre, 50), 2);
			auto ons = notes(result, EventKind::NoteOn);
			auto offs = notes(result, EventKind::NoteOff);
			expect(ons.size()).to_equal(size_t{8});
			expect(offs.size()).to_equal(size_t{8});
			// The 1 to the next onset four 16ths on (96) is cut to the gate (72); the next two end at the next onset;
			// the last ends on the bar line.
			const std::vector<int32_t> lengths{72, 2 * kStep, 72, kStep};
			for (size_t i = 0; i < lengths.size(); i++) {
				expect(offs[i].tick - ons[i].tick).to_equal(lengths[i]);
			}
		});

		it("ends by the bar line when it leans late", _ {
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Arp,
			                                       .name = "ARP",
			                                       .pool = kGaps,
			                                       .reg = kArpRegister,
			                                       .arp = {.walks = kUpTwo, .gateTicks = 72},
			                                       .expression = {.lean = {0, 0, 0, 12}}}};
			static constexpr Genre kGenre = arpGenre(kParts);
			Result result = play(runOf(kGenre, 50), 2);
			auto ons = notes(result, EventKind::NoteOn);
			auto offs = notes(result, EventKind::NoteOff);
			expect(ons.size()).to_equal(size_t{8});
			expect(ons[3].tick).to_equal(15 * kStep + 12);
			expect(offs[3].tick).to_equal(kBarTicks);
		});

		it("ends every note by the time the onset after its next one starts, whatever its Lean", _ {
			// The general bound (spec §5.2): a note's length stops at the next onset on the grid, and Lean moves either
			// note by 12 ticks at most, so the onset after next always starts after it ends.
			static constexpr Expression kCrossing{
			    .lean = {12, -12, 12, -12},
			    .ornaments = {.rateFloor = 500, .rateCeiling = 500, .ratchet = {1, 0, 0}, .flam = 1, .flamGap = 6}};
			static constexpr PartSpec kParts[] = {{.role = Role::Lead,
			                                       .flavour = Flavour::Arp,
			                                       .name = "ARP",
			                                       .pool = kEvery16th,
			                                       .reg = kArpRegister,
			                                       .arp = {.walks = kRandomTwo, .gateTicks = 96},
			                                       .expression = kCrossing}};
			static constexpr Genre kGenre = arpGenre(kParts);
			expect(arpNotesValid(kGenre)).to_be_true();
			for (uint32_t seed : {1U, 2U, 3U}) {
				Result result = play(runOf(kGenre, 50, seed), 8);
				expect(result.evictedNotes).to_equal(0U);
				expect(result.droppedEvents).to_equal(0U);
			}
		});
	});

	context("its genre data (spec §7.3, §5.2)", _ {
		static constexpr Register kOneOctave{.anchor = 57, .spanLow = -6, .spanHigh = 16};
		static constexpr ArpWalk kOne[] = {{.order = ArpOrder::Up, .octaves = 1}};
		static constexpr ArpWalk kOneAndTwo[] = {{.order = ArpOrder::Up, .octaves = 1},
		                                         {.order = ArpOrder::Down, .octaves = 2}};
		static constexpr Pattern kTwo[] = {kEvery16th[0], kEvery16th[0]};

		it("holds its widest pool entry's octave range in its Register, from a root anywhere in an octave", _ {
			// One octave of a walk with a 7th on top, from a root up to 11 semitones above the span's bottom: 22.
			expect(arpRange(1)).to_equal(22);
			expect(arpRange(2)).to_equal(34);
			expect(arpValid(kEvery16th, kOne, 18, kOneOctave)).to_be_true();
			expect(arpValid(kEvery16th, kOne, 18, {.anchor = 57, .spanLow = -6, .spanHigh = 15})).to_be_false();
			expect(arpValid(kTwo, kOneAndTwo, 18, kOneOctave)).to_be_false();
			expect(arpValid(kTwo, kOneAndTwo, 18, kArpRegister)).to_be_true();
		});

		it("gives every pool entry a walk of 1 to kMaxArpOctaves octaves, and a gate of at least a tick", _ {
			static constexpr ArpWalk kNone[] = {{.order = ArpOrder::Up, .octaves = 0}};
			expect(arpValid(kTwo, kOne, 18, kArpRegister)).to_be_false(); // a pool entry with no walk
			expect(arpValid(kEvery16th, kNone, 18, kArpRegister)).to_be_false();
			expect(arpValid(kEvery16th, kOne, 0, kArpRegister)).to_be_false();
			expect(arpValid({}, {}, 18, kArpRegister)).to_be_false(); // nothing to play
			// Two octaves at most (spec §8.1).
			static constexpr ArpWalk kThree[] = {{.order = ArpOrder::Up, .octaves = 3}};
			expect(arpValid(kEvery16th, kThree, 18, {.anchor = 57, .spanLow = -6, .spanHigh = 48})).to_be_false();
		});
	});
});

CPPSPEC_SPEC(arp)
