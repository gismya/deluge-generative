// Tempo and swing glides (spec §12.7, §13.3): the Perform driver writes song tempo once per tick along a line, and
// swing only on step boundaries.

#include "generative/perform/glide.h"
#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

/// Keeps every tempo and swing write with the driver clock it was made at.
struct TransportSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< (clock, hundredths of a BPM)
	std::vector<std::pair<int32_t, int32_t>> swings; ///< (clock, amount)
	void play(const Event&) override {}
	void setTempo(int32_t tempo) override { tempos.push_back({driver ? driver->clock() : -1, tempo}); }
	void setSwing(int32_t amount) override { swings.push_back({driver ? driver->clock() : -1, amount}); }
};

struct Playing {
	Generator generator;
	SteeringQueue queue;
	TransportSink sink;
	perform::PerformDriver driver;
	Hook hook{driver, queue, sink};

	Playing() {
		sink.driver = &driver;
		generator.start(technoRecipe(), 3, {}, true);
		driver.begin(generator);
		driver.setTempo(12800, sink);
		driver.setSwing(0, sink);
		sink.tempos.clear();
		sink.swings.clear();
		hook.run(0);
	}
	/// A tempo or swing edit, as the adapter makes it: the glide starts, and the hook is woken for the next tick.
	void glideTempo(int32_t tempo, int32_t ticks) {
		driver.glideTempo(tempo, ticks, sink);
		hook.wake();
	}
	void glideSwing(int32_t amount, int32_t ticks) {
		driver.glideSwing(amount, ticks, sink);
		hook.wake();
	}
};

constexpr int32_t kBar = kStepsPerBar * kTicksPerStep;

} // namespace

// clang-format off
describe glide("Tempo and swing glides", $ {
	it("maps the Glide setting to ticks: 1 bar by default, up to a phrase", _ {
		constexpr int32_t kPhrase = 8 * kStepsPerBar;
		expect(perform::glideTicks(perform::GlideLength::Instant, kPhrase)).to_equal(0);
		expect(perform::glideTicks(perform::GlideLength::Beat, kPhrase)).to_equal(4 * kTicksPerStep);
		expect(perform::glideTicks(perform::kDefaultGlide, kPhrase)).to_equal(kBar);
		expect(perform::glideTicks(perform::GlideLength::TwoBars, kPhrase)).to_equal(2 * kBar);
		expect(perform::glideTicks(perform::GlideLength::FourBars, kPhrase)).to_equal(4 * kBar);
		expect(perform::glideTicks(perform::GlideLength::Phrase, kPhrase)).to_equal(8 * kBar);
	});

	it("glides tempo along a line, once per tick, and ends on the target", _ {
		Playing p;
		p.hook.run(10);
		// Pressed at tick 10: the glide counts from the call the wake asks for, on 11, and asks for every tick after.
		p.glideTempo(13184, kBar); // +3.84 BPM over a bar: 1 hundredth a tick
		p.hook.run(kBar + 50);
		expect(p.sink.tempos.size()).to_equal(size_t{kBar});
		for (size_t i = 0; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].first).to_equal(12 + static_cast<int32_t>(i));
			expect(p.sink.tempos[i].second).to_equal(12801 + static_cast<int32_t>(i));
		}
		expect(p.sink.tempos.back().second).to_equal(13184);
		expect(p.hook.due() - p.hook.lastCall()).to_be_greater_than(1); // every tick only while it glides
	});

	it("glides a new tempo from wherever the last glide had got to, writing only changes", _ {
		Playing p;
		// Each glide counts from the first hook call after it, on the tick after the press.
		constexpr int32_t kFirst = 1;
		p.glideTempo(13184, kBar);
		p.hook.runTo(kFirst + kBar / 2);
		const size_t firstGlide = p.sink.tempos.size();
		constexpr int32_t kSecond = kFirst + kBar / 2 + 1;
		p.glideTempo(12800, kBar); // from 12992, down 192 over a bar: a hundredth every 2 ticks
		p.hook.runTo(kSecond + kBar + 10);
		auto tempoAt = [&](int32_t clock) {
			int32_t tempo = -1;
			for (auto [when, value] : p.sink.tempos) {
				tempo = when <= clock ? value : tempo;
			}
			return tempo;
		};
		expect(tempoAt(kFirst + kBar / 2)).to_equal(12992);
		expect(tempoAt(kSecond + kBar / 2)).to_equal(12896);
		expect(p.sink.tempos.back().second).to_equal(12800);
		expect(p.sink.tempos.size() - firstGlide).to_equal(size_t{192}); // once per change, not per tick
		for (size_t i = 1; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].second != p.sink.tempos[i - 1].second).to_be_true();
		}
	});

	it("changes swing only on step boundaries, stepping along its glide", _ {
		Playing p;
		p.hook.run(5);
		p.glideSwing(16, kBar); // about 1 a step
		p.hook.run(2 * kBar);
		// Written on each step line from the next one until the glide is done: a bar and one line more, since it
		// started 5 ticks past a line.
		expect(p.sink.swings.size()).to_equal(size_t{17});
		for (size_t i = 0; i < p.sink.swings.size(); i++) {
			expect(p.sink.swings[i].first % kTicksPerStep).to_equal(0);
		}
		expect(p.sink.swings.front().first).to_equal(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(16);

		// Instant still waits for the next step boundary.
		p.hook.run(3);
		size_t before = p.sink.swings.size();
		p.glideSwing(-10, 0);
		expect(p.sink.swings.size()).to_equal(before);
		p.hook.run(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(-10);
		expect(p.sink.swings.back().first % kTicksPerStep).to_equal(0);
	});

	it("writes tempo and swing at once while stopped, and a stop lands a running glide on its target", _ {
		Playing p;
		p.glideTempo(14000, 4 * kBar);
		p.glideSwing(30, 4 * kBar);
		p.hook.run(kBar);
		p.driver.stop(p.sink);
		expect(p.sink.tempos.back().second).to_equal(14000);
		expect(p.sink.swings.back().second).to_equal(30);

		p.driver.glideTempo(12000, 4 * kBar, p.sink);
		expect(p.sink.tempos.back().second).to_equal(12000);
		p.driver.glideSwing(-5, 4 * kBar, p.sink);
		expect(p.sink.swings.back().second).to_equal(-5);
	});

	it("keeps gliding across a jump, by the ticks that played", _ {
		Playing p;
		p.hook.run(100);
		p.glideTempo(13184, kBar);
		p.hook.run(kBar / 2);
		p.driver.jump(p.sink);
		p.hook.run(kBar);
		expect(p.sink.tempos.back().second).to_equal(13184);
		for (size_t i = 1; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].second - p.sink.tempos[i - 1].second).to_equal(1);
		}
	});

	it("logs tempo, swing and the swing interval as driver-side steering, on the next undecided step", _ {
		Generator generator;
		SteeringQueue queue;
		LogEntry entries[8];
		SteeringLog log(entries, 8);
		RecordingSink sink;
		perform::PerformDriver driver;
		generator.start(technoRecipe(), 3, {}, true);
		driver.begin(generator);
		driver.advance(0, kGeneratorTickMagnitude, queue, &log, sink);
		// Glides log as {target, length in ticks} (spec §17.2); the later one coalesces over both.
		queue.push({CommandKind::SetTempo, 0, 13000, 96});
		queue.push({CommandKind::SetTempo, 0, 13100, 384}); // continuous: coalesces
		queue.push({CommandKind::SetSwing, 0, 12, 0});
		queue.push({CommandKind::SetSwingInterval, 0, 1});
		const int32_t landing = generator.nextStep();
		driver.advance(kTicksPerStep, kGeneratorTickMagnitude, queue, &log, sink);
		expect(log.size()).to_equal(3);
		for (int32_t i = 0; i < log.size(); i++) {
			expect(log.at(i).step).to_equal(landing);
		}
		expect(log.at(0).command.value).to_equal(13100);
		expect(static_cast<int32_t>(log.at(0).command.length)).to_equal(384);
		expect(static_cast<int32_t>(log.at(1).command.length)).to_equal(0);
		expect(generator.recipe().tempo).to_equal(13100);
		expect(static_cast<int32_t>(generator.recipe().swing)).to_equal(12);
		expect(static_cast<int32_t>(generator.recipe().swingInterval)).to_equal(1);
		// They pass through: nothing in the stream changes.
		Run plain;
		plain.recipe = technoRecipe();
		plain.seed = 3;
		plain.skipIntro = true;
		Run steered = plain;
		steered.steer = at(40, {CommandKind::SetTempo, 0, 9000});
		expect(play(steered, 8).events == play(plain, 8).events).to_be_true();
	});
});

CPPSPEC_SPEC(glide)
