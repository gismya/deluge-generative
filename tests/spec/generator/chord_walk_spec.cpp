// The Chord-graph walk's rules (spec §7.4): the guard, the cadence target's swap, the weights each move, colour move,
// cadence and home bonus gives, and the keyed pick. Expected values are worked out by hand from the spec's tables.

#include "chord_moves.h"
#include "generative/core/chord_walk.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <vector>

using namespace deluge::generative;

namespace {

constexpr ChordGraph kGraph{
    .homePull = 10, .moves = chord_specs::kMelodicMoves, .cadenceTarget = kVII, .cadencePeriod = 4};

std::vector<int32_t> weights(const ChordWeights& w) {
	return {w.weight, w.weight + kDegreesPerScale};
}
using Weights = std::vector<int32_t>;

} // namespace

// clang-format off
describe chord_walk("The Chord-graph walk", $ {
	it("guards a degree whose triad isn't plain major or minor, under the strict rule", _ {
		// Minor: ii is diminished. Harmonic Minor: III is augmented and vii diminished.
		expect(chordGuarded(Scale::Minor, kII, ChordGuard{})).to_be_true();
		expect(chordGuarded(Scale::Minor, kIV, ChordGuard{})).to_be_false();
		expect(chordGuarded(Scale::Minor, kVII, ChordGuard{})).to_be_false();
		expect(chordGuarded(Scale::HarmonicMinor, 2, ChordGuard{})).to_be_true();
		expect(chordGuarded(Scale::HarmonicMinor, kVII, ChordGuard{})).to_be_true();
		expect(chordGuarded(Scale::HarmonicMinor, kV, ChordGuard{})).to_be_false();
		// Hungarian Minor ii: degrees 1, 3, 5 sit 2, 6 and 8 semitones up, a major 3rd and a tritone (major ♭5).
		expect(chordGuarded(Scale::HungarianMinor, kII, ChordGuard{})).to_be_true();
	});

	it("guards only a triad with a tritone fifth, under the tritone-fifth rule", _ {
		constexpr ChordGuard tritone{.rule = ChordGuardRule::TritoneFifth};
		expect(chordGuarded(Scale::Minor, kII, tritone)).to_be_true();           // diminished
		expect(chordGuarded(Scale::HarmonicMinor, 2, tritone)).to_be_false();    // augmented
		expect(chordGuarded(Scale::HungarianMinor, kII, tritone)).to_be_true(); // major ♭5
	});

	it("never guards the home chord, even where it is diminished", _ {
		expect(chordGuarded(Scale::Locrian, kI, ChordGuard{})).to_be_false();
		expect(chordGuarded(Scale::Arabian, kI, ChordGuard{})).to_be_false();
	});

	it("swaps a guarded cadence target to V, else IV", _ {
		ChordGraph graph{.cadenceTarget = static_cast<uint8_t>(kVII), .cadencePeriod = 4};
		expect(cadenceTarget(graph, Scale::Minor, ChordGuard{})).to_equal(kVII);
		expect(cadenceTarget(graph, Scale::HarmonicMinor, ChordGuard{})).to_equal(kV); // vii° → V
		graph.cadenceTarget = static_cast<uint8_t>(kII);
		expect(cadenceTarget(graph, Scale::Minor, ChordGuard{})).to_equal(kV); // ii° → V
		// Arabian: ii is diminished and V augmented (6, 10, 14 semitones), so the cadence goes to IV.
		expect(cadenceTarget(graph, Scale::Arabian, ChordGuard{})).to_equal(kIV);
	});

	it("weighs the graph's moves from the current chord, with home pull from anywhere but home", _ {
		expect(weights(chordWeights(kGraph, Scale::Minor, kI, -1, {})) == Weights{0, 0, 0, 20, 0, 30, 20}).to_be_true();
		expect(weights(chordWeights(kGraph, Scale::Minor, kVI, -1, {})) == Weights{10, 0, 0, 0, 0, 0, 30}).to_be_true();
	});

	it("makes staying on the current chord a candidate by the repeat weight", _ {
		ChordGraph graph = kGraph;
		graph.repeatWeight = 5;
		expect(weights(chordWeights(graph, Scale::Minor, kVI, -1, {})) == Weights{10, 0, 0, 0, 0, 5, 30}).to_be_true();
	});

	it("adds the scale's colour moves to whichever graph plays", _ {
		// Dorian's i–IV on top of the graph's i–iv. Dorian's vi is diminished, so the guard thins i–vi to 4.
		expect(weights(chordWeights(kGraph, Scale::Dorian, kI, -1, {}, {}, colourMoves(Scale::Dorian)))
		       == Weights{0, 0, 0, 40, 0, 4, 20}).to_be_true();
		// Harmonic Minor's V–i and iv–V: from V home gets the pull and the colour move.
		expect(weights(chordWeights(kGraph, Scale::HarmonicMinor, kV, -1, {}, {}, colourMoves(Scale::HarmonicMinor)))
		       == Weights{30, 0, 0, 0, 0, 0, 0}).to_be_true();
	});

	it("multiplies a guarded degree's weight by the guard factor", _ {
		constexpr ChordMove toII[] = {{0, 1, 40}, {0, 3, 40}};
		const ChordGraph graph{.moves = toII};
		expect(weights(chordWeights(graph, Scale::Minor, kI, -1, {})) == Weights{0, 6, 0, 40, 0, 0, 0}).to_be_true();
		expect(weights(chordWeights(graph, Scale::Minor, kI, -1, {}, ChordGuard{.factor = 500}))
		       == Weights{0, 20, 0, 40, 0, 0, 0}).to_be_true();
		// Major ii is minor: unguarded.
		expect(weights(chordWeights(graph, Scale::Major, kI, -1, {})) == Weights{0, 40, 0, 40, 0, 0, 0}).to_be_true();
	});

	it("thins a guarded degree's weight but never to 0, unless the factor is 0", _ {
		constexpr ChordMove light[] = {{0, 1, 6}, {0, 3, 6}};
		const ChordGraph graph{.moves = light};
		// 6 × 0.15 rounds down to 0: kept at 1.
		expect(weights(chordWeights(graph, Scale::Minor, kI, -1, {})) == Weights{0, 1, 0, 6, 0, 0, 0}).to_be_true();
		expect(weights(chordWeights(graph, Scale::Minor, kI, -1, {}, ChordGuard{.factor = 0}))
		       == Weights{0, 0, 0, 6, 0, 0, 0}).to_be_true();
	});

	it("gives the cadence target the cadence bonus, and the Drop cadence bonus on top before a Drop", _ {
		expect(weights(chordWeights(kGraph, Scale::Minor, kI, -1, {.cadence = true}))
		       == Weights{0, 0, 0, 20, 0, 30, 40}).to_be_true();
		expect(weights(chordWeights(kGraph, Scale::Minor, kI, -1, {.beforeDrop = true}))
		       == Weights{0, 0, 0, 20, 0, 30, 60}).to_be_true();
		expect(weights(chordWeights(kGraph, Scale::Minor, kI, -1, {.cadence = true, .beforeDrop = true}))
		       == Weights{0, 0, 0, 20, 0, 30, 80}).to_be_true();
		// Harmonic Minor's vii° swaps the cadence to V.
		expect(weights(chordWeights(kGraph, Scale::HarmonicMinor, kI, -1, {.cadence = true}))
		       == Weights{0, 0, 0, 20, 20, 30, 3}).to_be_true();
	});

	it("lets a graph have no cadence target, or home as one", _ {
		ChordGraph graph = kGraph;
		graph.cadenceTarget = kNoCadenceTarget;
		expect(cadenceTarget(graph, Scale::Minor, ChordGuard{})).to_equal(-1);
		expect(weights(chordWeights(graph, Scale::Minor, kI, -1, {.cadence = true, .beforeDrop = true}))
		       == Weights{0, 0, 0, 20, 0, 30, 20}).to_be_true();
		graph.cadenceTarget = static_cast<uint8_t>(kI);
		expect(weights(chordWeights(graph, Scale::Minor, kVI, -1, {.cadence = true}))
		       == Weights{30, 0, 0, 0, 0, 0, 30}).to_be_true();
	});

	it("gives home the phrase home bonus or the smaller Section home bonus, not both", _ {
		expect(weights(chordWeights(kGraph, Scale::Minor, kVI, -1, {.phraseFirst = true}))[kI]).to_equal(40);
		expect(weights(chordWeights(kGraph, Scale::Minor, kVI, -1, {.sectionStart = true}))[kI]).to_equal(25);
		expect(weights(chordWeights(kGraph, Scale::Minor, kVI, -1, {.sectionStart = true, .phraseFirst = true}))[kI])
		    .to_equal(40);
	});

	it("shrinks the home bonuses linearly with Complexity, to a quarter at 50", _ {
		const WalkRules rules{.phraseHomeBonus = 40};
		auto home = [&](int32_t points) {
			const ChordPlace place{.phraseFirst = true, .complexity = macroPoints(points)};
			return weights(chordWeights(kGraph, Scale::Minor, kVI, -1, place, {}, {}, rules))[kI];
		};
		expect(home(0)).to_equal(10 + 40);
		expect(home(25)).to_equal(10 + 25);
		expect(home(50)).to_equal(10 + 10);
	});

	it("excludes home on a phrase's last chord", _ {
		expect(weights(chordWeights(kGraph, Scale::Minor, kVII, -1, {.phraseLast = true}))[kI]).to_equal(0);
		expect(weights(chordWeights(kGraph, Scale::Minor, kVII, -1, {}))[kI]).to_equal(40);
	});

	it("excludes the chord before the current one when A-B-A isn't allowed", _ {
		ChordGraph graph = kGraph;
		expect(weights(chordWeights(graph, Scale::Minor, kVI, kVII, {}))[kVII]).to_equal(30);
		graph.abaAllowed = false;
		expect(weights(chordWeights(graph, Scale::Minor, kVI, kVII, {}))[kVII]).to_equal(0);
		expect(weights(chordWeights(graph, Scale::Minor, kVI, kI, {}))[kVII]).to_equal(30);
	});

	it("picks the next chord by its weight from the draw", _ {
		// From i: iv 20, ♭VI 30, ♭VII 20, in degree order.
		expect(nextChord(kGraph, Scale::Minor, kI, -1, {}, 0)).to_equal(kIV);
		expect(nextChord(kGraph, Scale::Minor, kI, -1, {}, 0x80000000U)).to_equal(kVI);
		expect(nextChord(kGraph, Scale::Minor, kI, -1, {}, 0xFFFFFFFFU)).to_equal(kVII);
	});

	it("goes home on the piece's first chord and on a Drop, whatever the draw", _ {
		for (uint32_t draw : {0U, 0x80000000U, 0xFFFFFFFFU}) {
			expect(nextChord(kGraph, Scale::Minor, kVI, -1, {.home = true}, draw)).to_equal(kI);
		}
	});

	it("stays put when nothing weighs, or goes to the cadence target when home is excluded", _ {
		const ChordGraph stuck{.cadenceTarget = kVII};
		expect(nextChord(stuck, Scale::Minor, kVI, -1, {}, 0)).to_equal(kVI);
		expect(nextChord(stuck, Scale::Minor, kI, -1, {.phraseLast = true}, 0)).to_equal(kVII);
	});

	it("leaves home for V, else IV, when home is excluded, nothing weighs and the graph has no target off home", _ {
		for (uint8_t target : {kNoCadenceTarget, static_cast<uint8_t>(kI)}) {
			const ChordGraph stuck{.cadenceTarget = target};
			expect(nextChord(stuck, Scale::Minor, kI, -1, {.phraseLast = true}, 0)).to_equal(kV);
			expect(nextChord(stuck, Scale::Arabian, kI, -1, {.phraseLast = true}, 0)).to_equal(kIV); // V augmented
		}
	});
});

CPPSPEC_SPEC(chord_walk)
