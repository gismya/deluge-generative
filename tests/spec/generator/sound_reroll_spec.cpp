// Sound re-rolls in the core (spec §10.7, §13.4): the driver preloads the part's new sound, then queues SetSound,
// which waits for the next phrase line. There the part's notes end, a Sound event tells the driver to swap the part's
// Output, and its notes go on as they would have: the seed doesn't cover sounds (spec §6.4).

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kAcid = 1;
constexpr int32_t kToken = 7;

Command newSound(uint8_t part, int32_t token) {
	return {CommandKind::SetSound, part, token};
}

/// Techno from its first Groove, so every part plays from bar 1.
Result playSteered(int32_t bars, std::vector<std::pair<int32_t, Command>> commands, SteeringLog* log = nullptr) {
	Run run;
	run.recipe = technoRecipe();
	run.seed = 3;
	run.skipIntro = true;
	run.log = log;
	run.steer = steerAt(std::move(commands));
	return play(run, bars);
}

/// Every event of a kind, without the Sound events, so a steered run compares with a plain one.
std::vector<Event> without(const std::vector<Event>& events, EventKind kind) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.kind != kind) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe sound_reroll("Sound re-rolls", $ {
	const int32_t phrase = genres::techno.phraseSteps();

	it("swaps on the next phrase line, after the part's note-offs there and before its note-ons", _ {
		Result result = playSteered(2 * phrase / kStepsPerBar, {{3, newSound(kAcid, kToken)}});
		std::vector<Event> sounds = only(result.events, EventKind::Sound);
		expect(sounds.size()).to_equal(1U);
		expect(sounds[0].tick).to_equal(stepTick(phrase));
		expect(static_cast<int32_t>(sounds[0].part)).to_equal(static_cast<int32_t>(kAcid));
		expect(static_cast<int32_t>(sounds[0].soundToken())).to_equal(kToken);

		// No ACID note sounds across the line: each note-on before it has its note-off by then.
		int32_t sounding = 0;
		bool swapped = false;
		bool notesAfterSwap = false;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::Sound) {
				expect(sounding).to_equal(0);
				swapped = true;
			}
			if (e.part != kAcid || e.lane != Lane::None) {
				continue;
			}
			if (e.kind == EventKind::NoteOn && !e.muted) {
				sounding++;
				notesAfterSwap = notesAfterSwap || swapped;
				expect(swapped || e.tick < stepTick(phrase)).to_be_true();
			}
			if (e.kind == EventKind::NoteOff && !swapped) {
				sounding--;
			}
		}
		expect(notesAfterSwap).to_be_true();
	});

	it("leaves every note as it was: the seed doesn't cover sounds", _ {
		const int32_t bars = 2 * phrase / kStepsPerBar;
		Result plain = playSteered(bars, {});
		Result swapped = playSteered(bars, {{3, newSound(kAcid, kToken)}});
		expect(only(swapped.events, EventKind::NoteOn) == only(plain.events, EventKind::NoteOn)).to_be_true();
	});

	it("waits in its part's pending slot, logged where it lands", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Result result = playSteered(phrase / kStepsPerBar + 1, {{3, newSound(kAcid, kToken)}}, &log);
		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(phrase);
		expect(log.at(0).command == newSound(kAcid, kToken)).to_be_true();
		expect(result.generator->soundLanded(kAcid)).to_equal(kToken);
		expect(result.generator->soundWaiting(kAcid)).to_equal(-1);
	});

	it("shows the token waiting until its line", _ {
		Result result = playSteered(phrase / kStepsPerBar - 1, {{3, newSound(kAcid, kToken)}});
		expect(result.generator->soundWaiting(kAcid)).to_equal(kToken);
		expect(result.generator->soundLanded(kAcid)).to_equal(-1);
	});

	it("cancels the waiting one on kNoSound, and logs nothing", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		Result result = playSteered(2 * phrase / kStepsPerBar,
		                            {{3, newSound(kAcid, kToken)}, {5, newSound(kAcid, kNoSound)}}, &log);
		expect(only(result.events, EventKind::Sound).empty()).to_be_true();
		expect(log.size()).to_equal(0);
		expect(result.generator->soundWaiting(kAcid)).to_equal(-1);
	});

	it("is dropped by a BACK tap like any queued action", _ {
		Result result = playSteered(2 * phrase / kStepsPerBar,
		                            {{3, newSound(kAcid, kToken)}, {5, Command{CommandKind::CancelNewest}}});
		expect(only(result.events, EventKind::Sound).empty()).to_be_true();
		expect(result.generator->soundLanded(kAcid)).to_equal(-1);
	});

	it("ignores a part outside the roster", _ {
		Result result = playSteered(2 * phrase / kStepsPerBar, {{3, newSound(kMaxParts, kToken)}});
		expect(only(result.events, EventKind::Sound).empty()).to_be_true();
		expect(without(result.events, EventKind::Sound).empty()).to_be_false();
	});
});

CPPSPEC_SPEC(sound_reroll)
