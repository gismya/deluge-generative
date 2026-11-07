// The portable Perform driver (spec §2.5, §2.6, §6.3, §12.2, §12.3): determinism, steering, the clock and stop.

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

Run techno() {
	Run run;
	run.recipe = technoRecipe();
	run.seed = 42;
	return run;
}

/// Steers Energy to `points` when the driver is about to decide `atStep`.
void steerEnergyAt(Run& run, int32_t atStep, int32_t points) {
	run.steer = [atStep, points](int32_t nextStep, SteeringQueue& queue) {
		if (nextStep == atStep) {
			queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(points)});
		}
	};
}

} // namespace

// clang-format off
describe perform_driver("The Perform driver", $ {
	it("gives the same stream for the same Recipe, seed and takes", _ {
		expect(play(techno(), 64).events == play(techno(), 64).events).to_be_true();
		Run other = techno();
		other.seed = 43;
		expect(play(other, 64).events == play(techno(), 64).events).to_be_false();
	});

	it("gives the same stream however often it is called and at any song magnitude", _ {
		auto expected = play(techno(), 32).events;
		for (int32_t magnitude : {1, 2, 3, 4}) {
			for (int32_t ticksPerCall : {0, 1, 5}) {
				Run run = techno();
				run.magnitude = magnitude;
				run.ticksPerCall = ticksPerCall;
				expect(play(run, 32).events == expected).to_be_true();
			}
		}
	});

	it("carries a part-tick remainder across a song magnitude change", _ {
		// Magnitude 3 has 2 song ticks per Generator tick, magnitude 4 has 4 (spec §12.3, §12.4).
		Generator generator;
		SteeringQueue queue;
		RecordingSink sink;
		perform::PerformDriver driver;
		generator.start(technoRecipe(), 42, {});
		driver.begin(generator);
		driver.advance(0, 3, queue, nullptr, sink);
		driver.advance(1, 3, queue, nullptr, sink); // half a Generator tick
		expect(driver.clock()).to_equal(0);
		driver.advance(2, 4, queue, nullptr, sink); // another half, at the finer magnitude
		expect(driver.clock()).to_equal(1);
		driver.advance(1, 2, queue, nullptr, sink); // a whole one at Perform's own magnitude
		expect(driver.clock()).to_equal(2);
		driver.advance(3, 4, queue, nullptr, sink); // three quarters: still short of the next
		expect(driver.clock()).to_equal(2);
	});

	it("never plays an event early, and plays a late one at the first call that reaches it", _ {
		struct ClockSink final : perform::NoteSink {
			const perform::PerformDriver* driver = nullptr;
			std::vector<std::pair<Event, int32_t>> played;
			void play(const Event& event) override { played.push_back({event, driver->clock()}); }
		};
		Generator generator;
		SteeringQueue queue;
		ClockSink sink;
		perform::PerformDriver driver;
		sink.driver = &driver;
		generator.start(technoRecipe(), 42, {});
		driver.begin(generator);
		// One call every 7 song ticks at magnitude 3 (48 per 16th): 3.5 Generator ticks a call.
		constexpr int32_t kCall = 7;
		for (int32_t t = 0; t < 8 * kStepsPerBar * 48; t += kCall) {
			driver.advance(t == 0 ? 0 : kCall, 3, queue, nullptr, sink);
		}
		expect(sink.played.size()).to_be_greater_than(size_t{50}); // all Intro: thin while Density rises from the floor
		for (auto [event, clock] : sink.played) {
			expect(clock).to_be_greater_than(event.tick - 1);
			expect(clock).to_be_less_than(event.tick + 4); // within one call
		}
	});

	it("gives the same stream, with nothing dropped, from a call up to a bar late", _ {
		// It plays as it decides, so a late call never holds more than a step's events in the buffer. The Techno
		// roster fits either way; the largest roster needs it (spec §2.7).
		Run busy = techno();
		for (Macro macro : {Macro::Energy, Macro::Density, Macro::Complexity, Macro::Motion}) {
			busy.recipe.macros.set(macro, macroPoints(50));
		}
		busy.skipIntro = true;
		Result onTime = play(busy, 24);
		busy.ticksPerCall = kStepsPerBar * kTicksPerStep; // one call a bar
		Result late = play(busy, 24);
		expect(late.droppedEvents).to_equal(0U);
		// The runs stop at different clocks, so their stop flushes differ: compare what played before the last bars.
		auto before = [](const std::vector<Event>& events) {
			std::vector<Event> out;
			for (const Event& e : events) {
				if (e.tick < stepTick(22 * kStepsPerBar)) {
					out.push_back(e);
				}
			}
			return out;
		};
		expect(before(late.events) == before(onTime.events)).to_be_true();
	});

	it("times its step decisions through the decision timer, only when a decision is due", _ {
		struct CountingTimer final : perform::DecisionTimer {
			int32_t calls = 0;
			int32_t decided = 0;
			bool open = false;
			void decisionsBegin() override { open = true; }
			void decisionsEnd(int32_t steps) override {
				calls += open ? 1 : 0;
				decided += steps;
				open = false;
			}
		};
		Generator generator;
		SteeringQueue queue;
		RecordingSink sink;
		CountingTimer timer;
		perform::PerformDriver driver;
		driver.setDecisionTimer(&timer);
		generator.start(technoRecipe(), 42, {});
		driver.begin(generator);
		Hook hook(driver, queue, sink);
		hook.run(kStepsPerBar * kTicksPerStep);
		expect(timer.decided).to_equal(generator.nextStep());
		expect(timer.calls).to_equal(generator.nextStep()); // one decision per call: on time, each call decides one
	});

	it("applies a macro on the next undecided step and logs it there", _ {
		constexpr int32_t kStep = 40 * kStepsPerBar; // a Groove downbeat
		Run run = techno();
		steerEnergyAt(run, kStep, 50);
		Result steered = play(run, 48);
		Result plain = play(techno(), 48);

		// Before the step, the streams agree; from it on, the kick plays harder.
		for (size_t i = 0; i < plain.events.size() && plain.events[i].tick < stepTick(kStep); i++) {
			expect(steered.events[i] == plain.events[i]).to_be_true();
		}
		auto velocityAt = [](const std::vector<Event>& events, int32_t tick) {
			for (const Event& e : events) {
				if (e.kind == EventKind::NoteOn && e.lane == Lane::Kick && e.tick == tick) {
					return static_cast<int32_t>(e.velocity);
				}
			}
			return 0;
		};
		expect(velocityAt(steered.events, stepTick(kStep)))
		    .to_be_greater_than(velocityAt(plain.events, stepTick(kStep)));
		expect(steered.generator->recipe().macros[Macro::Energy]).to_equal(macroPoints(50));

		// The log stamps the command with the step where it landed (ADR 0005).
		Generator generator;
		SteeringQueue queue;
		LogEntry entries[4];
		SteeringLog log(entries, 4);
		RecordingSink sink;
		perform::PerformDriver driver;
		generator.start(technoRecipe(), 42, {});
		driver.begin(generator);
		driver.advance(0, kGeneratorTickMagnitude, queue, &log, sink);
		queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), macroPoints(10)});
		int32_t landing = generator.nextStep();
		driver.advance(24, kGeneratorTickMagnitude, queue, &log, sink);
		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(landing);
		expect(log.at(0).command.value).to_equal(macroPoints(10));
	});

	it("ends every sounding note on stop, at the stop tick, and plays nothing after", _ {
		// 3 ticks past a downbeat the kick still sounds; 15 ticks past, the next step is decided but hasn't played.
		for (int32_t pastLine : {3, 15}) {
			Generator generator;
			SteeringQueue queue;
			RecordingSink sink;
			perform::PerformDriver driver;
			generator.start(technoRecipe(), 42, {});
			driver.begin(generator);
			Hook hook(driver, queue, sink);
			hook.run(40 * kStepsPerBar * kTicksPerStep + pastLine);
			int32_t stopAt = driver.clock(); // the last call's
			size_t before = sink.events.size();
			driver.stop(sink);
			expect(driver.running()).to_be_false();
			if (pastLine == 3) {
				expect(sink.events.size()).to_be_greater_than(before); // the kick's note-off
			}
			int32_t ons = 0;
			int32_t offs = 0;
			for (const Event& e : sink.events) {
				ons += e.kind == EventKind::NoteOn ? 1 : 0;
				offs += e.kind == EventKind::NoteOff ? 1 : 0;
			}
			expect(offs).to_equal(ons);
			for (size_t i = before; i < sink.events.size(); i++) {
				expect(sink.events[i].kind).to_equal(EventKind::NoteOff);
				expect(sink.events[i].tick).to_equal(stopAt);
			}
		}
	});
});

CPPSPEC_SPEC(perform_driver)
