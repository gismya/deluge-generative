// Part mute and solo (spec §12.5, §13.4, §13.5): the core's muted flag, landing on the next bar line or, with SHIFT,
// the next undecided step. The driver drops flagged note-ons, so the stream specs read here is what Perform plays.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;

Command solo(uint8_t part, bool now = false) {
	return {CommandKind::SetSolo, part, now ? kPressNow : kPress};
}

/// A part's note-ons in [from, to) steps.
std::vector<Event> noteOns(const Result& result, uint8_t part, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == part && e.tick >= stepTick(from) && e.tick < stepTick(to)) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe mute_solo("Part mute and solo", $ {
	it("mutes a part from the next bar line, and leaves every other part as it was", _ {
		Result plain = playSteered(4, {});
		Result muted = playSteered(4, {{5, mute(kAcid)}});
		expect(noteOns(plain, kAcid, 0, kStepsPerBar).empty()).to_be_false();
		expect(noteOns(muted, kAcid, 0, kStepsPerBar) == noteOns(plain, kAcid, 0, kStepsPerBar)).to_be_true();
		expect(noteOns(muted, kAcid, kStepsPerBar, 4 * kStepsPerBar).empty()).to_be_true();
		expect(noteOns(muted, kDrums, 0, 4 * kStepsPerBar) == noteOns(plain, kDrums, 0, 4 * kStepsPerBar)).to_be_true();
	});

	it("with SHIFT, mutes from the next undecided step", _ {
		Result plain = playSteered(4, {});
		Result muted = playSteered(4, {{1, mute(kDrums, true)}});
		expect(noteOns(muted, kDrums, 0, 1) == noteOns(plain, kDrums, 0, 1)).to_be_true();
		expect(noteOns(plain, kDrums, 1, kStepsPerBar).empty()).to_be_false();
		expect(noteOns(muted, kDrums, 1, 4 * kStepsPerBar).empty()).to_be_true();
	});

	it("cancels a press still waiting for its line when the pad is pressed again", _ {
		Result plain = playSteered(4, {});
		Result twice = playSteered(4, {{5, mute(kAcid)}, {9, mute(kAcid)}});
		expect(noteOns(twice, kAcid, 0, 4 * kStepsPerBar) == noteOns(plain, kAcid, 0, 4 * kStepsPerBar)).to_be_true();
	});

	it("keeps a muted part generating, so unmuting brings back the notes it would have played", _ {
		Result plain = playSteered(4, {});
		Result back = playSteered(4, {{5, mute(kAcid)}, {2 * kStepsPerBar + 3, mute(kAcid)}});
		expect(noteOns(back, kAcid, kStepsPerBar, 3 * kStepsPerBar).empty()).to_be_true();
		expect(noteOns(plain, kAcid, 3 * kStepsPerBar, 4 * kStepsPerBar).empty()).to_be_false();
		expect(noteOns(back, kAcid, 3 * kStepsPerBar, 4 * kStepsPerBar)
		       == noteOns(plain, kAcid, 3 * kStepsPerBar, 4 * kStepsPerBar)).to_be_true();
	});

	it("solos: while any part is soloed only soloed parts sound, whatever their mute", _ {
		Result plain = playSteered(4, {});
		Result soloed = playSteered(4, {{1, mute(kAcid, true)}, {2, solo(kAcid)}, {kStepsPerBar + 2, solo(kAcid)}});
		// Bar 2: only the soloed (and muted) Acid sounds. Bar 3: the solo is off, so the mute holds again.
		expect(noteOns(soloed, kDrums, kStepsPerBar, 2 * kStepsPerBar).empty()).to_be_true();
		expect(noteOns(soloed, kAcid, kStepsPerBar, 2 * kStepsPerBar)
		       == noteOns(plain, kAcid, kStepsPerBar, 2 * kStepsPerBar)).to_be_true();
		expect(noteOns(soloed, kAcid, 2 * kStepsPerBar, 4 * kStepsPerBar).empty()).to_be_true();
		expect(noteOns(soloed, kDrums, 2 * kStepsPerBar, 4 * kStepsPerBar)
		       == noteOns(plain, kDrums, 2 * kStepsPerBar, 4 * kStepsPerBar)).to_be_true();
	});

	it("logs a mute or solo where it lands, as the state it set, and never a cancelled press", _ {
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		playSteered(4, {{5, mute(kAcid)}, {9, mute(kAcid)}, {10, solo(kDrums)}, {20, mute(kDrums, true)}}, &log);
		expect(log.size()).to_equal(2);
		expect(log.at(0).step).to_equal(kStepsPerBar);
		expect(log.at(0).command == Command{CommandKind::SetSolo, kDrums, 1}).to_be_true();
		expect(log.at(1).step).to_equal(20);
		expect(log.at(1).command == Command{CommandKind::SetMute, kDrums, 1}).to_be_true();
	});

	it("is lost on stop: every start is fresh", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
		piece.decideBefore(1); // step 0, a bar line: a press before it would land there
		piece.push(mute(kAcid, true));
		piece.push(solo(kDrums));
		piece.decideBefore(2);
		expect(piece.generator.partMuted(kAcid)).to_be_true();
		expect(piece.generator.soloPending(kDrums)).to_be_true();
		piece.restart({.seed = 3, .skipIntro = true});
		expect(piece.generator.partMuted(kAcid)).to_be_false();
		expect(piece.generator.soloPending(kDrums)).to_be_false();
	});

	it("drops a mute or solo for a part past the roster: it never waits in the queue", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
		piece.decideBefore(1);
		const auto pastRoster = static_cast<uint8_t>(partCount(genres::techno));
		piece.push(mute(pastRoster));
		piece.push(solo(pastRoster));
		piece.decideBefore(2);
		expect(piece.generator.queuedCount()).to_equal(0);
		expect(piece.generator.refusedSteering()).to_equal(0U);
	});
});

CPPSPEC_SPEC(mute_solo)
