// A part's octave shift (spec §3.1, §7.3, §13.4): the Recipe moves a pitched part's Register anchor by 12 semitones
// per octave, so its notes move by exactly that and nothing else in the stream does. Steered, it waits for the next
// bar line in its part's pending slot, and lands logged.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;

Command octaveTo(uint8_t part, int32_t value) {
	return {CommandKind::SetOctave, part, value};
}

/// `events` with `part`'s notes moved by `semitones`.
std::vector<Event> movedBy(std::vector<Event> events, int32_t part, int32_t semitones) {
	for (Event& event : events) {
		if ((event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) && event.part == part) {
			event.pitch = static_cast<uint8_t>(event.pitch + semitones);
		}
	}
	return events;
}

/// The first event where `a` and `b` differ, as text, or "" when they match.
std::string firstDifference(const std::vector<Event>& a, const std::vector<Event>& b) {
	for (size_t i = 0; i < std::min(a.size(), b.size()); i++) {
		if (!(a[i] == b[i])) {
			return "event " + std::to_string(i) + " tick " + std::to_string(a[i].tick) + " part "
			       + std::to_string(a[i].part) + " pitch " + std::to_string(a[i].pitch) + " vs "
			       + std::to_string(b[i].pitch);
		}
	}
	return a.size() == b.size() ? "" : "sizes " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
}

Run shiftedRun(const Genre& genre, int32_t part, int32_t shift) {
	Run run;
	run.recipe = movingRecipe(genre, 7);
	run.recipe.octave[part] = static_cast<int8_t>(shift);
	run.seed = 11;
	return run;
}

/// The index of the part named `name` in `genre`'s roster.
int32_t partNamed(const Genre& genre, std::string_view name) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (genre.parts[p].name == name) {
			return p;
		}
	}
	return -1;
}

/// The re-voice case (spec §7.3): `part` of `genre` steered `shift` octaves a few steps before bar 4's line. No note
/// of it rings over the line in the old Register, the voicing sounding from the line is the plain run's moved by the
/// shift, struck there, and each note-on after the line is the plain run's moved by it. Each mismatch goes to
/// `problems`.
/// @return the note-ons the part played after the line, so a caller knows the case ran.
size_t revoiceCase(const Genre& genre, int32_t p, int32_t shift, std::vector<std::string>& problems) {
	const auto runFrom = [&genre] {
		Run run = shiftedRun(genre, 0, 0);
		run.skipIntro = true;
		return run;
	};
	Run run = runFrom();
	const int32_t landStep = 4 * kStepsPerBar; // the bar line after the press
	run.steer = steerAt({{landStep - 5, octaveTo(static_cast<uint8_t>(p), shift)}});
	const Result result = play(run, 8);
	const Result plain = play(runFrom(), 8);
	const int32_t line = landStep * kTicksPerStep;
	const std::string where =
	    std::string{genre.name} + " part " + std::to_string(p) + " shift " + std::to_string(shift) + ": ";
	for (const Held& note : heldNotes(result.events, p)) {
		if (note.on < line && note.off > line) {
			problems.push_back(where + "a note rings over the line");
		}
	}
	const auto soundingAt = [&](const std::vector<Event>& events, int32_t semitones) {
		std::vector<int32_t> pitches;
		for (const Held& note : heldNotes(events, p)) {
			if (note.on <= line && note.off > line) {
				pitches.push_back(note.pitch + semitones);
			}
		}
		std::sort(pitches.begin(), pitches.end());
		return pitches;
	};
	if (soundingAt(result.events, 0) != soundingAt(plain.events, 12 * shift)) {
		problems.push_back(where + "the voicing on the line isn't the plain one moved");
	}
	const auto onsAfter = [&](const std::vector<Event>& events, int32_t semitones) {
		std::vector<Event> ons;
		for (const Event& event : only(events, EventKind::NoteOn)) {
			if (event.part == p && event.tick > line) {
				Event moved = event;
				moved.pitch = static_cast<uint8_t>(moved.pitch + semitones);
				ons.push_back(moved);
			}
		}
		return ons;
	};
	const std::vector<Event> ons = onsAfter(result.events, 0);
	const std::string difference = firstDifference(ons, onsAfter(plain.events, 12 * shift));
	if (!difference.empty()) {
		problems.push_back(where + difference);
	}
	return ons.size();
}

/// Runs revoiceCase() up and down an octave on every part of every genre that plays `flavour`. Up alone doesn't show
/// a voicing left behind in a Stab, whose voicings sit low in its window: voice-led from below, the moved window
/// takes the same notes. Down does. @return how many of the cases played after the line.
int32_t revoiceEvery(Flavour flavour, std::vector<std::string>& problems) {
	int32_t played = 0;
	for (const Genre* genre : allGenres()) {
		for (int32_t p = 0; p < partCount(*genre); p++) {
			if (genre->parts[p].flavour != flavour) {
				continue;
			}
			for (int32_t shift : {1, -1}) {
				played += revoiceCase(*genre, p, shift, problems) > 0 ? 1 : 0;
			}
		}
	}
	return played;
}

} // namespace

// clang-format off
describe octave("Octave shift", $ {
	const int32_t phrase = genres::technoAcid.phraseSteps();
	const int32_t phraseBars = phrase / kStepsPerBar;

	it("is 0 in a new Recipe, for every part", _ {
		Recipe recipe = newRecipe(genres::technoAcid, 5);
		for (int32_t p = 0; p < kMaxParts; p++) {
			expect(static_cast<int32_t>(recipe.octave[p])).to_equal(0);
		}
	});

	it("moves a Register's anchor by 12 semitones an octave, and its span with it", _ {
		const Register reg{40, -5, 12};
		const Register up = shiftedRegister(reg, 2);
		expect(static_cast<int32_t>(up.anchor)).to_equal(64);
		expect(static_cast<int32_t>(up.spanLow)).to_equal(-5);
		expect(static_cast<int32_t>(up.spanHigh)).to_equal(12);
		expect(static_cast<int32_t>(shiftedRegister(reg, -1).anchor)).to_equal(28);
		expect(static_cast<int32_t>(shiftedRegister(reg, 0).anchor)).to_equal(40);
	});

	it("gives Registers only to pitched roles", _ {
		expect(hasRegister(Role::Bass)).to_be_true();
		expect(hasRegister(Role::Chords)).to_be_true();
		expect(hasRegister(Role::Pad)).to_be_true();
		expect(hasRegister(Role::Lead)).to_be_true();
		expect(hasRegister(Role::Drums)).to_be_false();
		expect(hasRegister(Role::FX)).to_be_false();
	});

	it("takes, per part, only the shifts that keep its whole reach a MIDI note", _ {
		expect(octaveRange(genres::technoDub.parts[partNamed(genres::technoDub, "SUB")]).lowest).to_equal(-1);
		expect(octaveRange(genres::technoDub.parts[partNamed(genres::technoDub, "SUB")]).highest).to_equal(2);
		const PartSpec& lead = genres::psytranceClassic.parts[partNamed(genres::psytranceClassic, "LEAD")];
		expect(octaveRange(lead).lowest).to_equal(-2);
		expect(octaveRange(lead).highest).to_equal(1);
		expect(octaveRange(genres::technoAcid.parts[kDrums]).lowest).to_equal(0);
		expect(octaveRange(genres::technoAcid.parts[kDrums]).highest).to_equal(0);
		for (const Genre* genre : allGenres()) {
			for (int32_t p = 0; p < partCount(*genre); p++) {
				const OctaveRange range = octaveRange(genre->parts[p]);
				expect(range.lowest >= -kMaxOctaveShift && range.lowest <= 0).to_be_true();
				expect(range.highest <= kMaxOctaveShift && range.highest >= 0).to_be_true();
			}
		}
	});

	it("keeps every note of a part inside its shifted reach at every shift it takes, in every key", _ {
		// The reach is worked out from the Register alone (pitchReach()): this checks no flavour plays past it, in
		// every key root, so no allowed shift ever clamps a note at 0 or 127.
		for (const Genre* genre : allGenres()) {
			for (int32_t p = 0; p < partCount(*genre); p++) {
				if (!hasRegister(genre->parts[p].role)) {
					continue;
				}
				const PitchReach reach = pitchReach(genre->parts[p]);
				const OctaveRange range = octaveRange(genre->parts[p]);
				for (int32_t shift = range.lowest; shift <= range.highest; shift++) {
					for (uint8_t root = 0; root < 12; root++) {
						Run run = shiftedRun(*genre, p, shift);
						run.recipe.keyRoot = root;
						run.recipe.macros.set(Macro::Complexity, kMacroFull); // Answers, wide Motifs and Arps
						run.skipIntro = true;
						const Result result = play(run, 16);
						for (const Event& event : only(result.events, EventKind::NoteOn)) {
							if (event.part != p) {
								continue;
							}
							const int32_t low = reach.lowest + 12 * shift;
							const int32_t high = reach.highest + 12 * shift;
							if (event.pitch < low || event.pitch > high || low < 0 || high > 127) {
								expect(std::string{genre->name} + " part " + std::to_string(p) + " shift "
								       + std::to_string(shift) + " root " + std::to_string(root) + ": note "
								       + std::to_string(event.pitch) + " outside " + std::to_string(low) + ".."
								       + std::to_string(high))
								    .to_equal(std::string{});
							}
						}
					}
				}
			}
		}
	});

	it("moves a shifted part's notes by exactly 12 semitones an octave, in every genre, and nothing else", _ {
		for (const Genre* genre : allGenres()) {
			const Result plain = play(shiftedRun(*genre, 0, 0), 48);
			for (int32_t p = 0; p < partCount(*genre); p++) {
				const OctaveRange range = octaveRange(genre->parts[p]);
				for (int32_t shift = range.lowest; shift <= range.highest; shift++) {
					if (shift == 0) {
						continue;
					}
					const Result shifted = play(shiftedRun(*genre, p, shift), 48);
					const std::string difference = firstDifference(shifted.events, movedBy(plain.events, p, 12 * shift));
					if (!difference.empty()) {
						std::string what = std::string{genre->name} + " part " + std::to_string(p) + " shift "
						                   + std::to_string(shift) + ": " + difference;
						expect(what).to_equal(std::string{});
					}
				}
			}
		}
	});

	it("waits for the next bar line, then plays the part an octave up and logs the change", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		// Watched mid-bar, after the press and before the line: the driver decides a step ahead of the song.
		int32_t playingMidBar = -1;
		int32_t waitingMidBar = -1;
		Run watched;
		watched.recipe = acidRecipe();
		watched.seed = 3;
		watched.skipIntro = true;
		watched.steer = steerAt({{3, octaveTo(kAcid, 1)}});
		watched.watch = [&](const Generator& g) {
			if (g.nextStep() == 10) {
				playingMidBar = g.octave(kAcid);
				waitingMidBar = g.octaveWaiting(kAcid);
			}
		};
		play(watched, 1);
		expect(playingMidBar).to_equal(0);
		expect(waitingMidBar).to_equal(1);

		Result plain = playSteered(phraseBars + 2, {});
		Result landed = playSteered(phraseBars + 2, {{3, octaveTo(kAcid, 1)}}, &log);
		expect(landed.generator->octave(kAcid)).to_equal(1);
		expect(landed.generator->octaveWaiting(kAcid)).to_equal(kNoOctaveWaiting);
		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(kStepsPerBar);
		expect(log.at(0).command == octaveTo(kAcid, 1)).to_be_true();
		// Before the bar line the stream is the plain one; from it on, the part's notes start an octave up. Every
		// other event, the part's note-offs aside (a slid note rings over the line at its own pitch), is the same.
		const int32_t line = kStepsPerBar * kTicksPerStep;
		const auto withoutOffs = [](const std::vector<Event>& events) {
			std::vector<Event> out;
			for (const Event& event : events) {
				if (event.kind != EventKind::NoteOff || event.part != kAcid) {
					out.push_back(event);
				}
			}
			return out;
		};
		std::vector<Event> expected = withoutOffs(plain.events);
		for (Event& event : expected) {
			if (event.kind == EventKind::NoteOn && event.part == kAcid && event.tick >= line) {
				event.pitch = static_cast<uint8_t>(event.pitch + 12);
			}
		}
		expect(firstDifference(withoutOffs(landed.events), expected)).to_equal(std::string{});
		expect(heldNotes(landed.events, kAcid).size()).to_equal(heldNotes(plain.events, kAcid).size());
	});

	it("re-voices a held Sustain part (a Pad, a Drone) at the bar line the shift lands on", _ {
		std::vector<std::string> problems;
		expect(revoiceEvery(Flavour::Sustain, problems) > 0).to_be_true();
		expect(problems.empty() ? std::string{} : problems.front()).to_equal(std::string{});
	});

	it("re-voices a Stab part's chords at the bar line the shift lands on", _ {
		std::vector<std::string> problems;
		expect(revoiceEvery(Flavour::Stab, problems) > 0).to_be_true();
		expect(problems.empty() ? std::string{} : problems.front()).to_equal(std::string{});
	});

	it("cancels a waiting shift when the shift that plays is pressed, and logs nothing", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Result result = playSteered(2, {{3, octaveTo(kAcid, 1)}, {5, octaveTo(kAcid, 0)}}, &log);
		expect(result.generator->octave(kAcid)).to_equal(0);
		expect(result.generator->octaveWaiting(kAcid)).to_equal(kNoOctaveWaiting);
		expect(log.size()).to_equal(0);
	});

	it("replaces a waiting shift with another", _ {
		Result result = playSteered(2, {{3, octaveTo(kAcid, 1)}, {5, octaveTo(kAcid, -2)}});
		expect(result.generator->octave(kAcid)).to_equal(-2);
	});

	it("refuses a shift past the part's octave range, and starts a Recipe's at the nearest it takes", _ {
		const Genre& genre = genres::technoDub;
		const auto sub = static_cast<uint8_t>(partNamed(genre, "SUB"));
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Run run = shiftedRun(genre, 0, 0);
		run.skipIntro = true;
		run.log = &log;
		run.steer = steerAt({{3, octaveTo(sub, -2)}});
		Result refused = play(run, 2);
		expect(refused.generator->octave(sub)).to_equal(0);
		expect(log.size()).to_equal(0);

		Result started = play(shiftedRun(genre, sub, -2), 1);
		expect(started.generator->octave(sub)).to_equal(-1);
	});

	it("refuses Drums and FX, parts past the roster and shifts past two octaves", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		const uint8_t fx = static_cast<uint8_t>(partCount(genres::technoAcid) - 1);
		Result result = playSteered(2,
		                            {{3, octaveTo(kDrums, 1)},
		                             {4, octaveTo(fx, 1)},
		                             {5, octaveTo(kMaxParts, 1)},
		                             {6, octaveTo(kAcid, 3)},
		                             {7, octaveTo(kAcid, -3)}},
		                            &log);
		expect(result.generator->octave(kDrums)).to_equal(0);
		expect(result.generator->octave(kAcid)).to_equal(0);
		expect(log.size()).to_equal(0);
	});

	it("starts each piece on the Recipe's shifts", _ {
		Run run;
		run.recipe = acidRecipe();
		run.recipe.octave[kAcid] = -1;
		Result result = play(run, 1);
		expect(result.generator->octave(kAcid)).to_equal(-1);
		expect(result.generator->recipe().octave[kAcid]).to_equal(-1);
	});
});

CPPSPEC_SPEC(octave)
