// Queued actions (spec §12.2, §13.5): what waits in the core's pending list, where each lands, the 12-tick close, and
// BACK cancelling the newest first. The sidebar, the status line and QUEUE read these queries.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;
constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars

constexpr Command kCancelNewest{CommandKind::CancelNewest};
constexpr Command kHoldPress{CommandKind::SetHold, 0, kPress};

/// A Techno piece from SHIFT+PLAY. A push between runs is a press at the tick the last run reached.
struct Playing : StepwisePiece<> {
	Playing() : StepwisePiece({.seed = 3, .skipIntro = true, .logEntries = 16}) {}
};

} // namespace

// clang-format off
describe queue("Queued actions", $ {
	it("lands a press before the line's decision tick (13+ ticks ahead) on it, and one at 12 on the next bar", _ {
		constexpr int32_t kLine = 2 * kStepsPerBar;
		for (int32_t early : {13, 12}) {
			Playing d;
			d.runToStepLine(kLine, -early);
			d.push(mute(kAcid));
			d.runToStepLine(kLine + 2 * kStepsPerBar);
			expect(d.log.size()).to_equal(1);
			// Pressed 13 ticks before the line it arrives in time for the line's decision, 12 before the line.
			expect(d.log.at(0).step).to_equal(early > 12 ? kLine : kLine + kStepsPerBar);
		}
	});

	it("lists what waits, oldest first, with the step each lands on", _ {
		Playing d;
		d.decideBefore(5);
		d.push(mute(kAcid));
		d.push(force(SectionKind::Break));
		d.push({CommandKind::SetTake, kDrums, 1});
		d.decideBefore(6);
		expect(d.generator.queuedCount()).to_equal(3);
		expect(d.generator.queued(0).kind).to_equal(CommandKind::SetMute);
		expect(d.generator.landsAt(d.generator.queued(0))).to_equal(kStepsPerBar);
		expect(d.generator.queued(1).kind).to_equal(CommandKind::ForceSection);
		expect(d.generator.landsAt(d.generator.queued(1))).to_equal(kPhraseSteps);
		expect(d.generator.landsAt(d.generator.queued(2))).to_equal(kPhraseSteps);
	});

	it("lands each queued action on the step its countdown named", _ {
		Playing d;
		d.decideBefore(37);
		d.push(mute(kAcid));
		d.push({CommandKind::SetSolo, kDrums, kPress});
		d.push(force(SectionKind::Build));
		d.push({CommandKind::SetTake, kDrums, 1});
		d.push({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Dorian)});
		d.decideBefore(38);
		std::vector<std::pair<CommandKind, int32_t>> named;
		for (int32_t i = 0; i < d.generator.queuedCount(); i++) {
			named.push_back({d.generator.queued(i).kind, d.generator.landsAt(d.generator.queued(i))});
		}
		expect(named.size()).to_equal(size_t{5});
		d.runToStepLine(3 * kPhraseSteps);
		expect(d.generator.queuedCount()).to_equal(0);
		expect(d.log.size()).to_equal(5);
		for (auto [kind, at] : named) {
			bool found = false;
			for (int32_t i = 0; i < d.log.size(); i++) {
				found = found || (d.log.at(i).command.kind == kind && d.log.at(i).step == at);
			}
			expect(found).to_be_true();
		}
	});

	it("places a forced Build where it lets the Drop land on a phrase line", _ {
		Playing d;
		d.decideBefore(5);
		d.push(force(SectionKind::Build));
		d.decideBefore(6);
		const Command& build = d.generator.queued(0);
		expect(build.value > 0).to_be_true();
		const int32_t at = d.generator.landsAt(build);
		expect((at + build.value) % kPhraseSteps).to_equal(0);
		expect(at >= 6 && at < 6 + kPhraseSteps).to_be_true();
		d.decideBefore(at + 1);
		expect(d.generator.arc().section()).to_equal(SectionKind::Build);
		expect(d.generator.arc().sectionStartsHere()).to_be_true();
	});

	it("cancels the newest queued action on a BACK tap, newest first, and logs neither", _ {
		Playing d;
		d.decideBefore(5);
		d.push(mute(kAcid));
		d.push(force(SectionKind::Break));
		d.push(kCancelNewest);
		d.decideBefore(6);
		expect(d.generator.queuedCount()).to_equal(1);
		expect(d.generator.mutePending(kAcid)).to_be_true();
		expect(d.generator.sectionForced()).to_be_false();
		d.push(kCancelNewest);
		d.push(kCancelNewest); // nothing left: does nothing
		d.decideBefore(7);
		expect(d.generator.queuedCount()).to_equal(0);
		d.runToStepLine(kPhraseSteps + 1);
		expect(d.log.size()).to_equal(0);
	});

	it("counts a press that replaces a waiting one as the newest", _ {
		Playing d;
		d.decideBefore(5);
		d.push({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Minor)});
		d.push(mute(kAcid));
		d.push({CommandKind::SetKey, 4, static_cast<int32_t>(Scale::Minor)});
		d.push(kCancelNewest);
		d.decideBefore(6);
		expect(d.generator.keyChangePending()).to_be_false();
		expect(d.generator.mutePending(kAcid)).to_be_true();
	});

	it("toggles the hold on each press of its pad, from the next undecided step", _ {
		Playing d;
		d.decideBefore(5);
		d.push(kHoldPress);
		d.decideBefore(6);
		expect(d.generator.arc().held()).to_be_true();
		d.push(kHoldPress);
		d.decideBefore(7);
		expect(d.generator.arc().held()).to_be_false();
		expect(d.log.size()).to_equal(2);
		expect(d.log.at(0).command == Command{CommandKind::SetHold, 0, 1}).to_be_true();
		expect(d.log.at(1).command == Command{CommandKind::SetHold, 0, 0}).to_be_true();
	});

	it("says which parts the Arc silences, for the dim MUTE pads", _ {
		Playing d;
		d.decideBefore(5);
		expect(d.generator.partSilenced(kAcid)).to_be_false();
		d.push(force(SectionKind::Break));
		d.decideBefore(kPhraseSteps + 2);
		expect(d.generator.arc().section()).to_equal(SectionKind::Break);
		// Techno's Break keeps the kick, rim and closed hat: the acid line goes.
		expect(d.generator.partSilenced(kAcid)).to_be_true();
		expect(d.generator.partSilenced(kDrums)).to_be_false();
	});
});

CPPSPEC_SPEC(queue)
