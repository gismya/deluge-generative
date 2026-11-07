// The queue as the OLED shows it (spec §13.5): the status line's tag for the next action to land, and QUEUE's
// lines with their countdowns.

#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <string>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

constexpr uint8_t kDrums = 0;
constexpr uint8_t kAcid = 1;

/// A piece played up to just after a step's decision, with commands pushed before it.
struct Steered {
	Generator generator;
	SteeringQueue queue;
	RecordingSink sink;
	PerformDriver driver;

	Steered() {
		generator.start(technoRecipe(), 3, {}, true);
		driver.begin(generator);
		driver.advance(0, kGeneratorTickMagnitude, queue, nullptr, sink);
	}
	/// Plays until `step` is the next to decide.
	void decideTo(int32_t step) {
		while (generator.nextStep() < step) {
			driver.advance(1, kGeneratorTickMagnitude, queue, nullptr, sink);
		}
	}
	std::string tag() {
		char text[16];
		statusTag(generator, driver.clock(), text);
		return text;
	}
};

std::string labelOf(const Command& command) {
	char text[24];
	queueLabel(command, genres::techno, text);
	return text;
}

std::string countdownOf(int32_t ticks, bool words) {
	char text[16];
	countdown(ticks, words, text);
	return text;
}

} // namespace

// clang-format off
describe queue_text("Queue text", $ {
	it("tags the next action to land with its bars to go, and + when more are queued", _ {
		Steered s;
		s.decideTo(5);
		expect(s.tag()).to_equal(std::string{});
		s.queue.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		s.decideTo(6);
		// The force lands on step 128, 2964 ticks after the clock at step 5's decision: 7.7 bars.
		expect(s.tag()).to_equal(std::string{">BR8"});
		s.queue.push({CommandKind::SetMute, kAcid, kPress});
		s.decideTo(7);
		// The mute lands first, on step 16, under a bar away: counted in beats.
		expect(s.tag()).to_equal(std::string{">MUT3B+"});
	});

	it("lists the queue soonest first, the older first on a tie", _ {
		Steered s;
		s.decideTo(5);
		s.queue.push({CommandKind::SetTake, kDrums, 1});
		s.queue.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0});
		s.queue.push({CommandKind::SetSolo, kAcid, kPress});
		s.decideTo(6);
		QueuedAction actions[4];
		expect(queuedByTime(s.generator, actions)).to_equal(3);
		expect(actions[0].command.kind).to_equal(CommandKind::SetSolo);
		expect(actions[0].landsAt).to_equal(kStepsPerBar);
		expect(actions[1].command.kind).to_equal(CommandKind::SetTake);
		expect(actions[2].command.kind).to_equal(CommandKind::ForceSection);
	});

	it("fills QUEUE's lines soonest first, and with more than fit, the last line says how many more", _ {
		Steered s;
		s.decideTo(5);
		QueueLines lines;
		queueLines(s.generator, s.driver.clock(), genres::techno, lines);
		expect(std::string{lines.label[0]}).to_equal(std::string{"NOTHING QUEUED"});
		s.queue.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		s.queue.push({CommandKind::SetMute, kAcid, kPress});
		s.decideTo(6);
		queueLines(s.generator, s.driver.clock(), genres::techno, lines);
		expect(std::string{lines.label[0]}).to_equal(std::string{"ACID MUTE"});
		expect(std::string{lines.when[0]}).to_equal(std::string{"3 BEATS"});
		expect(std::string{lines.label[1]}).to_equal(std::string{"BREAK"});
		expect(std::string{lines.label[2]}).to_equal(std::string{});
		s.queue.push({CommandKind::SetMute, kDrums, kPress});
		s.queue.push({CommandKind::SetSolo, kDrums, kPress});
		s.decideTo(7);
		queueLines(s.generator, s.driver.clock(), genres::techno, lines);
		expect(std::string{lines.label[1]}).to_equal(std::string{"DRUMS MUTE"});
		expect(std::string{lines.label[QueueLines::kLines - 1]}).to_equal(std::string{"+2 MORE"});
		expect(std::string{lines.when[QueueLines::kLines - 1]}).to_equal(std::string{});
	});

	it("names each action for QUEUE", _ {
		expect(labelOf({CommandKind::SetMute, kAcid, 1})).to_equal(std::string{"ACID MUTE"});
		expect(labelOf({CommandKind::SetMute, kAcid, 0})).to_equal(std::string{"ACID ON"});
		expect(labelOf({CommandKind::SetSolo, kDrums, 1})).to_equal(std::string{"DRUMS SOLO"});
		expect(labelOf({CommandKind::SetSolo, kDrums, 0})).to_equal(std::string{"DRUMS UNSOLO"});
		expect(labelOf({CommandKind::SetTake, kDrums, 1})).to_equal(std::string{"DRUMS TAKE 2"});
		expect(labelOf({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 128}))
		    .to_equal(std::string{"BUILD"});
		expect(labelOf({CommandKind::End, 0, 1})).to_equal(std::string{"OUTRO"});
		expect(labelOf({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Dorian)})).to_equal(std::string{"KEY D DORIAN"});
	});

	it("counts down in whole bars, rounded up, and under a bar in beats", _ {
		constexpr int32_t kBeat = kStepsPerBeat * kTicksPerStep;
		constexpr int32_t kBar = kStepsPerBar * kTicksPerStep;
		expect(countdownOf(3 * kBar, false)).to_equal(std::string{"3"});
		expect(countdownOf(2 * kBar + 1, false)).to_equal(std::string{"3"});
		expect(countdownOf(kBar, false)).to_equal(std::string{"1"});
		expect(countdownOf(kBar - 1, false)).to_equal(std::string{"4B"});
		expect(countdownOf(1, false)).to_equal(std::string{"1B"});
		expect(countdownOf(0, false)).to_equal(std::string{"1B"});
		expect(countdownOf(2 * kBar, true)).to_equal(std::string{"2 BARS"});
		expect(countdownOf(kBar, true)).to_equal(std::string{"1 BAR"});
		expect(countdownOf(kBeat, true)).to_equal(std::string{"1 BEAT"});
		expect(countdownOf(2 * kBeat, true)).to_equal(std::string{"2 BEATS"});
	});
});

CPPSPEC_SPEC(queue_text)
