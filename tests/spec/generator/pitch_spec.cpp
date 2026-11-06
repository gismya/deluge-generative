// Pitch (spec §7): scale steps inside and absolute notes out, Registers, Degree sets, and the home chord with its
// Extensions. Expected values are worked out by hand from the spec's tables and examples.

#include "generative/core/pitch.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <vector>

using namespace deluge::generative;

namespace {

constexpr uint8_t kC = 0;
constexpr uint8_t kFSharp = 6;
constexpr uint8_t kA = 9;

constexpr Register kBass{36, -5, 12};
constexpr Register kLead{72, -5, 19};

/// Semitones above the tonic of a non-negative count of scale steps.
int32_t semitones(int32_t steps, Scale scale) {
	return 12 * (steps / kDegreesPerScale) + kScaleSemitones[static_cast<int32_t>(scale)][steps % kDegreesPerScale];
}

/// The chord's notes as semitones above its root, for comparing with chord spellings.
std::vector<int32_t> spelling(const Chord& chord, Scale scale) {
	std::vector<int32_t> out;
	for (int32_t i = 0; i < chord.size; i++) {
		out.push_back(semitones(chord.steps[i], scale) - semitones(chord.steps[0], scale));
	}
	return out;
}

using Notes = std::vector<int32_t>;

} // namespace

// clang-format off
describe pitch("Pitch", $ {
	it("puts a part's tonic on the key root nearest its anchor, the lower one on a tie", _ {
		expect(tonicNote(kBass, kA)).to_equal(33);      // A1, 3 below C2
		expect(tonicNote(kBass, kC)).to_equal(36);      // the anchor itself
		expect(tonicNote(kBass, 2)).to_equal(38);       // D2, 2 above
		expect(tonicNote(kBass, kFSharp)).to_equal(30); // F#1 and F#2 are both 6 away
		expect(tonicNote(kLead, kA)).to_equal(69);
	});

	it("turns scale steps from the tonic into notes of the key's scale", _ {
		Key aMinor{kA, Scale::Minor};
		expect(noteFor(0, aMinor, kBass)).to_equal(33);  // A
		expect(noteFor(2, aMinor, kBass)).to_equal(36);  // C
		expect(noteFor(6, aMinor, kBass)).to_equal(43);  // G, the ♭7
		Key aPhrygian{kA, Scale::Phrygian};
		expect(noteFor(1, aPhrygian, kBass)).to_equal(34); // B♭, the ♭2
		Key cMarva{kC, Scale::Marva};
		expect(noteFor(3, cMarva, kBass)).to_equal(42);    // F#
	});

	it("plays the 5th below the root for a Bass step on the 5th at octave -1", _ {
		Key aMinor{kA, Scale::Minor};
		expect(noteFor(4 - kDegreesPerScale, aMinor, kBass)).to_equal(28); // E1, the tonic minus 5
	});

	it("folds notes outside the span back by octaves, and lets the note's own octave pick where it fits twice", _ {
		Key aMinor{kA, Scale::Minor};
		expect(noteFor(9, aMinor, kBass)).to_equal(36);   // C an octave up is +15, so it folds to +3
		expect(noteFor(-5, aMinor, kBass)).to_equal(36);  // C an octave down is -9, so it folds to +3
		expect(noteFor(-1, aMinor, kBass)).to_equal(31);  // G at -2 fits, and stays below
		expect(noteFor(6, aMinor, kBass)).to_equal(43);   // G at +10 fits too, and stays above
		expect(noteFor(14, aMinor, kLead)).to_equal(81);  // A two octaves up is +24, past the Lead's +19
		expect(noteFor(-14, aMinor, kLead)).to_equal(69); // A two octaves down folds up to the tonic
	});

	it("snaps a step outside the Degree set to the nearest allowed degree, a tie going down", _ {
		// Minor Pentatonic is degrees 1 3 4 5 7 (spec §7.5), steps 0 2 3 4 6.
		expect(snap(1, DegreeSet::Pentatonic, Scale::Minor, 0)).to_equal(0);
		expect(snap(5, DegreeSet::Pentatonic, Scale::Minor, 0)).to_equal(4);
		expect(snap(6, DegreeSet::Pentatonic, Scale::Minor, 0)).to_equal(6);
		expect(snap(8, DegreeSet::Pentatonic, Scale::Minor, 0)).to_equal(7);
		// Major Pentatonic is degrees 1 2 3 5 6: the 7th is one step from the 6th and from the octave.
		expect(snap(3, DegreeSet::Pentatonic, Scale::Major, 0)).to_equal(2);
		expect(snap(6, DegreeSet::Pentatonic, Scale::Major, 0)).to_equal(5);
		expect(snap(-1, DegreeSet::Pentatonic, Scale::Major, 0)).to_equal(-2);
	});

	it("leaves every step alone in the Full set", _ {
		for (int32_t step = -14; step <= 14; step++) {
			expect(snap(step, DegreeSet::Full, Scale::Phrygian, 0)).to_equal(step);
		}
	});

	it("always allows the current chord's root", _ {
		expect(snap(5, DegreeSet::Pentatonic, Scale::Minor, 5)).to_equal(5);
		expect(snap(12, DegreeSet::Pentatonic, Scale::Minor, 5)).to_equal(12);
		expect(snap(5, DegreeSet::Pentatonic, Scale::Minor, 0)).to_equal(4);
	});

	it("resolves Pentatonic per scale with the semitone counts the spec's table gives", _ {
		// Adjacent pairs a semitone apart within the set, the octave wrap included (spec §7.5).
		const int32_t kSemitones[kNumScales] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0};
		for (int32_t s = 0; s < kNumScales; s++) {
			Scale scale = static_cast<Scale>(s);
			int32_t pitches[kDegreesPerScale];
			int32_t count = 0;
			for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
				if (snap(degree, DegreeSet::Pentatonic, scale, 0) == degree) {
					pitches[count++] = kScaleSemitones[s][degree];
				}
			}
			expect(count).to_equal(5);
			expect(pitches[0]).to_equal(0);
			int32_t semitones = 0;
			for (int32_t i = 0; i < count; i++) {
				int32_t next = i + 1 < count ? pitches[i + 1] : pitches[0] + 12;
				semitones += next - pitches[i] == 1 ? 1 : 0;
			}
			expect(semitones).to_equal(kSemitones[s]);
		}
	});
	it("voices a plain triad at Extension level 0", _ {
		expect(spelling(voiceChord(Scale::Minor, 0, 0), Scale::Minor) == Notes{0, 3, 7}).to_be_true();
		expect(spelling(voiceChord(Scale::Major, 0, 0), Scale::Major) == Notes{0, 4, 7}).to_be_true();
		expect(spelling(voiceChord(Scale::Locrian, 0, 0), Scale::Locrian) == Notes{0, 3, 6}).to_be_true();
	});

	it("adds the 7 first, then the 9, dropping the 5th rather than play a 5th note", _ {
		expect(spelling(voiceChord(Scale::Minor, 0, 1), Scale::Minor) == Notes{0, 3, 7, 10}).to_be_true();
		expect(spelling(voiceChord(Scale::Major, 0, 1), Scale::Major) == Notes{0, 4, 7, 11}).to_be_true();
		// Dorian i at level 2 is m9: R ♭3 ♭7 9 (spec §7.4).
		expect(spelling(voiceChord(Scale::Dorian, 0, 2), Scale::Dorian) == Notes{0, 3, 10, 14}).to_be_true();
		expect(spelling(voiceChord(Scale::Major, 0, 2), Scale::Major) == Notes{0, 4, 11, 14}).to_be_true();
	});

	it("rejects an Extension a semitone above a chord tone and tries the next, sus4 replacing the 3rd", _ {
		// Phrygian i at level 2: the ♭9 is a semitone above the root, so it's i7sus4, R 4 5 ♭7 (spec §7.4).
		expect(spelling(voiceChord(Scale::Phrygian, 0, 2), Scale::Phrygian) == Notes{0, 5, 7, 10}).to_be_true();
		// Minor ii (B D F in A Minor): the ♭9 (C) sits a semitone above the root, so sus4 comes after the 7.
		expect(spelling(voiceChord(Scale::Minor, 1, 2), Scale::Minor) == Notes{0, 5, 6, 10}).to_be_true();
	});

	it("tries the 6 only when the 7 was rejected", _ {
		// No v1 scale guards a 7 by default, so a stricter guard (data, spec §7.4) rejects it: anything 10 or 11
		// semitones above a chord tone. The 9 and sus4 fall to it as well, and the 6 comes in.
		ExtensionGuard strict{.rejectedAbove = 1U << 10 | 1U << 11};
		expect(spelling(voiceChord(Scale::Major, 0, 1, strict), Scale::Major) == Notes{0, 4, 7, 9}).to_be_true();
		expect(spelling(voiceChord(Scale::Major, 0, 2), Scale::Major) == Notes{0, 4, 11, 14}).to_be_true();
	});

	it("builds a chord on any degree, in steps from the tonic", _ {
		Chord iv = voiceChord(Scale::Minor, 3, 0);
		expect(iv.size).to_equal(3);
		expect(iv.steps[0]).to_equal(3);
		expect(iv.steps[1]).to_equal(5);
		expect(iv.steps[2]).to_equal(7);
	});

	it("raises the Extension level with Complexity, within the genre's floor and ceiling", _ {
		expect(extensionLevel(0, 0, 2)).to_equal(0);
		expect(extensionLevel(kMacroFull, 0, 2)).to_equal(2);
		int32_t previous = 0;
		for (int32_t complexity = 0; complexity <= kMacroFull; complexity += kMacroUnitsPerKnobStep) {
			int32_t level = extensionLevel(complexity, 0, 2);
			expect(level).to_be_between(previous, previous + 1);
			previous = level;
		}
		expect(extensionLevel(0, 1, 2)).to_equal(1);          // House's floor: never a bare triad
		expect(extensionLevel(kMacroFull, 0, 1)).to_equal(1); // a ceiling caps it
		expect(extensionLevel(kMacroFull, 0, 0)).to_equal(0);
	});
});

CPPSPEC_SPEC(pitch)
