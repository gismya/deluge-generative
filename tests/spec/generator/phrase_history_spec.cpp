// The SONG page's played columns (spec §13.5): the Perform driver's per-phrase history of what it sent. A part sounded
// in a phrase when a note of it rang there, one held on from an earlier phrase included: a Drone or Pad holds its
// voicing over phrase lines with no new note-on.

#include "generative/perform/phrase_history.h"
#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace harness;
using deluge::generative::perform::PhraseHistory;

namespace {

constexpr int32_t kSteps = 16; // the hand-fed history's phrase
constexpr int32_t kLine = stepTick(kSteps);

/// The parts phrase `phrase` of `history` sounded, or -1 if it holds no such phrase.
int32_t sounds(const PhraseHistory& history, int32_t phrase) {
	ArcPhrase heard;
	return history.at(phrase, heard) ? heard.sounds : -1;
}

/// The roster index of `genre`'s Pad, or -1 if it has none.
int32_t padOf(const Genre& genre) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (genre.parts[p].role == Role::Pad) {
			return p;
		}
	}
	return -1;
}

/// What the history should hold, worked out from the events the driver sent: bit p of phrase i is set when a note of
/// part p started in it or rang into it from before its line. A note-off ends the note of its key that rings, if one
/// does: a muted note's note-on was never sent, so its note-off ends nothing. Notes still ringing at the end ring on.
struct Reference {
	std::vector<uint8_t> sounds;
	/// Bit p of phrase i: part p sounded in it with no note-on there, by a note held over its line.
	std::vector<uint8_t> heldOnly;

	Reference(const std::vector<Event>& events, int32_t phraseSteps, int32_t phrases)
	    : sounds(phrases), heldOnly(phrases) {
		const int32_t phraseTicks = stepTick(phraseSteps);
		std::vector<uint8_t> started(phrases);
		std::map<std::tuple<uint8_t, Lane, uint8_t>, int32_t> ringing; // key -> its note-on's tick
		const auto key = [](const Event& e) {
			return std::tuple{e.part, e.lane, e.lane == Lane::None ? e.pitch : uint8_t{0}};
		};
		const auto mark = [&](uint8_t part, int32_t from, int32_t to) { // the ticks [from, to)
			for (int32_t phrase = from / phraseTicks; phrase < phrases && phrase * phraseTicks < to; phrase++) {
				sounds[phrase] |= partBit(part);
			}
		};
		for (const Event& e : events) {
			if (e.tick < 0) {
				continue;
			}
			if (e.kind == EventKind::NoteOn && !e.muted) {
				if (e.tick / phraseTicks < phrases) {
					started[e.tick / phraseTicks] |= partBit(e.part);
				}
				mark(e.part, e.tick, e.tick + 1);
				ringing[key(e)] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff) {
				if (auto note = ringing.find(key(e)); note != ringing.end()) {
					mark(e.part, note->second, e.tick);
					ringing.erase(note);
				}
			}
		}
		for (const auto& [k, from] : ringing) {
			mark(std::get<0>(k), from, INT32_MAX);
		}
		for (int32_t phrase = 0; phrase < phrases; phrase++) {
			heldOnly[phrase] = sounds[phrase] & ~started[phrase];
		}
	}
};

} // namespace

// clang-format off
describe phrase_history("Phrase history", $ {
	it("hears a part in every phrase a held note rings through, not only the one it starts in", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(10, 3, Lane::None, 48, 100));
		history.played(Event::noteOff(3 * kLine + 50, 3, Lane::None, 48));
		expect(sounds(history, 0)).to_equal(partBit(3));
		expect(sounds(history, 1)).to_equal(partBit(3));
		expect(sounds(history, 2)).to_equal(partBit(3));
		expect(sounds(history, 3)).to_equal(partBit(3));
		expect(sounds(history, 4)).to_equal(-1);
	});

	it("doesn't hear a note ending on a phrase line in the phrase that line starts", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 3, Lane::None, 48, 100));
		history.played(Event::noteOff(kLine, 3, Lane::None, 48));
		history.played(Event::noteOn(kLine, 1, Lane::None, 36, 100));
		history.played(Event::noteOff(kLine + 6, 1, Lane::None, 36));
		expect(sounds(history, 0)).to_equal(partBit(3));
		expect(sounds(history, 1)).to_equal(partBit(1));
	});

	it("hears a note still ringing in the phrase playing once a step past its line is decided", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 3, Lane::None, 48, 100));
		for (int32_t step = 0; step <= kSteps; step++) {
			history.decided(step, 0);
		}
		// Step 16 is decided 12 ticks before its line, which hasn't passed yet.
		expect(sounds(history, 1)).to_equal(0);
		history.decided(kSteps + 1, 0);
		expect(sounds(history, 1)).to_equal(partBit(3));
	});

	it("ends a note only by its own key's note-off, so a muted note's note-off ends nothing", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 3, Lane::None, 48, 100));
		history.played(Event::noteOn(0, 0, Lane::Kick, 36, 100));
		// The note-offs of a muted note on another pitch and on another lane: their note-ons were never sent.
		history.played(Event::noteOff(30, 3, Lane::None, 55));
		history.played(Event::noteOff(30, 0, Lane::Snare, 38));
		history.played(Event::noteOff(2 * kLine + 5, 3, Lane::None, 48));
		history.played(Event::noteOff(2 * kLine + 5, 0, Lane::Kick, 36));
		expect(sounds(history, 1)).to_equal(partBit(0) | partBit(3));
		expect(sounds(history, 2)).to_equal(partBit(0) | partBit(3));
		// A note-off with no note-on sent sounds nothing.
		history.played(Event::noteOff(2 * kLine + 9, 4, Lane::None, 60));
		expect(sounds(history, 2)).to_equal(partBit(0) | partBit(3));
	});

	it("hears a note cut and struck again on a phrase line ring on, and end at its next note-off", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 3, Lane::None, 48, 100));
		// A Section line cuts the held note and the new Section strikes it again, at the same tick.
		history.played(Event::noteOff(kLine, 3, Lane::None, 48));
		history.played(Event::noteOn(kLine, 3, Lane::None, 48, 100));
		history.played(Event::noteOff(kLine + 30, 3, Lane::None, 48));
		history.decided(2 * kSteps + 1, 0);
		expect(sounds(history, 0)).to_equal(partBit(3));
		expect(sounds(history, 1)).to_equal(partBit(3));
		expect(sounds(history, 2)).to_equal(0);
	});

	it("keeps a note's key within its part's keys, a pitch past 127 included", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 5, Lane::None, 255, 100));
		history.played(Event::noteOff(10, 5, Lane::None, 255));
		history.decided(kSteps + 1, 0);
		expect(sounds(history, 0)).to_equal(partBit(5));
		expect(sounds(history, 1)).to_equal(0);
	});

	it("ends the notes a jump's flush ends, so a held Pad rings on after it only if it plays again", _ {
		const Genre& genre = genres::technoMelodic;
		const int32_t pad = padOf(genre);
		const int32_t phraseSteps = genre.phraseSteps();
		Start start{.seed = 1, .skipIntro = true};
		start.recipe = newRecipe(genre, 1);
		StepwisePiece<> piece(start);
		// Plays into the second phrase until a Pad note rings, then jumps and mutes the Pad.
		const auto padRinging = [&] {
			int32_t ringing = 0;
			for (const Event& e : piece.sink.events) {
				if (e.part == pad && e.kind == EventKind::NoteOn && !e.muted) {
					ringing++;
				}
				else if (e.part == pad && e.kind == EventKind::NoteOff && ringing > 0) {
					ringing--;
				}
			}
			return ringing > 0;
		};
		int32_t step = phraseSteps + 1;
		while (!padRinging()) {
			piece.decideBefore(++step);
		}
		expect(step < 2 * phraseSteps).to_be_true();
		piece.driver.jump(piece.sink);
		expect(padRinging()).to_be_false();
		const int32_t resumed = piece.generator.nextStep();
		piece.pushAndDecide({CommandKind::SetMute, static_cast<uint8_t>(pad), kPress});
		const int32_t phrases = resumed / phraseSteps + 3;
		piece.decideBefore(phrases * phraseSteps + 1);
		const Reference reference(piece.sink.events, phraseSteps, phrases);
		for (int32_t phrase = 0; phrase < phrases; phrase++) {
			const std::string name = "phrase " + std::to_string(phrase) + ": ";
			expect(name + std::to_string(sounds(piece.driver.history(), phrase)))
			    .to_equal(name + std::to_string(reference.sounds[phrase]));
		}
		for (int32_t phrase = resumed / phraseSteps + 1; phrase < phrases; phrase++) {
			expect(sounds(piece.driver.history(), phrase) & partBit(pad)).to_equal(0);
		}
	});

	it("forgets what was ringing when a piece starts", _ {
		PhraseHistory history;
		history.begin(kSteps);
		history.played(Event::noteOn(0, 3, Lane::None, 48, 100));
		history.begin(kSteps);
		history.played(Event::noteOn(kLine + 3, 1, Lane::None, 36, 100));
		expect(sounds(history, 0)).to_equal(-1);
		expect(sounds(history, 1)).to_equal(partBit(1));
	});

	it("hears each part where its notes rang, a held Pad included, for every genre", _ {
		constexpr int32_t kPhrases = PhraseHistory::kPhrases - 2;
		int32_t padHeldOnly = 0;
		for (const Genre* genre : allGenres()) {
			const int32_t pad = padOf(*genre);
			const int32_t phraseSteps = genre->phraseSteps();
			for (uint32_t seed = 1; seed <= 3; seed++) {
				Start start{.seed = seed, .skipIntro = seed == 2};
				start.recipe = newRecipe(*genre, seed);
				StepwisePiece<> piece(start);
				if (seed == 3 && pad >= 0) {
					// A mute and an unmute, so muted notes' note-offs reach the history.
					piece.decideBefore(3 * phraseSteps + 5);
					piece.pushAndDecide({CommandKind::SetMute, static_cast<uint8_t>(pad), kPress});
					piece.decideBefore(5 * phraseSteps + 9);
					piece.pushAndDecide({CommandKind::SetMute, static_cast<uint8_t>(pad), kPress});
				}
				piece.runToStepLine(kPhrases * phraseSteps);
				const Reference reference(piece.sink.events, phraseSteps, kPhrases);
				for (int32_t phrase = 0; phrase < kPhrases; phrase++) {
					const std::string name = std::string(genre->name) + " seed " + std::to_string(seed) + " phrase "
					                         + std::to_string(phrase) + ": ";
					expect(name + std::to_string(sounds(piece.driver.history(), phrase)))
					    .to_equal(name + std::to_string(reference.sounds[phrase]));
					padHeldOnly += pad >= 0 && (reference.heldOnly[phrase] & partBit(pad)) != 0 ? 1 : 0;
				}
			}
		}
		expect(padHeldOnly > 0).to_be_true(); // the sweep holds a Pad over a phrase line with no note-on after it
	});
});

CPPSPEC_SPEC(phrase_history)
