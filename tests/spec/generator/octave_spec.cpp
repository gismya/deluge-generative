// A part's octave shift (spec §3.1, §7.3, §13.4): the Recipe moves a pitched part's Register anchor by 12 semitones
// per octave, so its notes move by exactly that and nothing else in the stream does. Steered, it waits for the next
// bar line in its part's pending slot, and lands logged.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;

Command octaveTo(uint8_t part, int32_t value) {
	return {CommandKind::SetOctave, part, value};
}

/// `events` with `part`'s notes moved by `semitones`, held in MIDI's range as noteFor() holds them.
std::vector<Event> movedBy(std::vector<Event> events, int32_t part, int32_t semitones) {
	for (Event& event : events) {
		if ((event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) && event.part == part) {
			event.pitch = static_cast<uint8_t>(std::clamp<int32_t>(event.pitch + semitones, 0, 127));
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

	it("moves a shifted part's notes by exactly 12 semitones an octave, in every genre, and nothing else", _ {
		for (const Genre* genre : allGenres()) {
			const Result plain = play(shiftedRun(*genre, 0, 0), 48);
			for (int32_t p = 0; p < partCount(*genre); p++) {
				if (!hasRegister(genre->parts[p].role)) {
					continue;
				}
				for (int32_t shift : {-2, -1, 1, 2}) {
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

	it("re-voices a held Pad at the bar line the shift lands on", _ {
		for (const Genre* genre : allGenres()) {
			for (int32_t p = 0; p < partCount(*genre); p++) {
				if (genre->parts[p].role != Role::Pad) {
					continue;
				}
				Run run = shiftedRun(*genre, 0, 0);
				run.skipIntro = true;
				const int32_t landStep = 4 * kStepsPerBar; // the bar line after the press
				run.steer = steerAt({{landStep - 5, octaveTo(static_cast<uint8_t>(p), 1)}});
				const Result result = play(run, 8);
				const Run plainRun = [&] {
					Run r = shiftedRun(*genre, 0, 0);
					r.skipIntro = true;
					return r;
				}();
				const Result plain = play(plainRun, 8);
				const int32_t line = landStep * kTicksPerStep;
				// No note of the part rings over the line in the old Register: the voicing sounding from the line is
				// the plain run's an octave up, struck there, and each note-on after the line is the plain run's an
				// octave up.
				int32_t ringingOver = 0;
				for (const Held& note : heldNotes(result.events, p)) {
					if (note.on < line && note.off > line) {
						ringingOver++;
					}
				}
				expect(ringingOver).to_equal(0);
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
				expect(soundingAt(result.events, 0) == soundingAt(plain.events, 12)).to_be_true();
				std::vector<Event> movedOns;
				for (const Event& event : only(plain.events, EventKind::NoteOn)) {
					if (event.part == p && event.tick > line) {
						Event moved = event;
						moved.pitch = static_cast<uint8_t>(moved.pitch + 12);
						movedOns.push_back(moved);
					}
				}
				std::vector<Event> ons;
				for (const Event& event : only(result.events, EventKind::NoteOn)) {
					if (event.part == p && event.tick > line) {
						ons.push_back(event);
					}
				}
				expect(firstDifference(ons, movedOns)).to_equal(std::string{});
			}
		}
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
