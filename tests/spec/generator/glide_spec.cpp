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
	int32_t lastWait = 0;

	Playing() {
		sink.driver = &driver;
		generator.start(technoRecipe(), 3, {}, true);
		driver.begin(generator);
		driver.setTempo(12800, sink);
		driver.setSwing(0, sink);
		sink.tempos.clear();
		sink.swings.clear();
		lastWait = driver.advance(0, kGeneratorTickMagnitude, queue, nullptr, sink);
	}
	void feed(int32_t ticks) {
		for (int32_t t = 0; t < ticks; t++) {
			lastWait = driver.advance(1, kGeneratorTickMagnitude, queue, nullptr, sink);
		}
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
		p.feed(10);
		p.driver.glideTempo(13184, kBar, p.sink); // +3.84 BPM over a bar: 1 hundredth a tick
		expect(p.driver.advance(0, kGeneratorTickMagnitude, p.queue, nullptr, p.sink)).to_equal(1);
		p.feed(kBar + 50);
		expect(p.sink.tempos.size()).to_equal(size_t{kBar});
		for (size_t i = 0; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].first).to_equal(11 + static_cast<int32_t>(i));
			expect(p.sink.tempos[i].second).to_equal(12801 + static_cast<int32_t>(i));
		}
		expect(p.sink.tempos.back().second).to_equal(13184);
		expect(p.lastWait).to_be_greater_than(1); // asks for every tick only while it glides
	});

	it("glides a new tempo from wherever the last glide had got to, writing only changes", _ {
		Playing p;
		p.driver.glideTempo(13184, kBar, p.sink);
		p.feed(kBar / 2);
		const size_t firstGlide = p.sink.tempos.size();
		p.driver.glideTempo(12800, kBar, p.sink); // from 12992, down 192 over a bar: a hundredth every 2 ticks
		p.feed(kBar);
		auto tempoAt = [&](int32_t clock) {
			int32_t tempo = -1;
			for (auto [when, value] : p.sink.tempos) {
				tempo = when <= clock ? value : tempo;
			}
			return tempo;
		};
		expect(tempoAt(kBar / 2)).to_equal(12992);
		expect(tempoAt(kBar)).to_equal(12896);
		expect(p.sink.tempos.back().second).to_equal(12800);
		expect(p.sink.tempos.size() - firstGlide).to_equal(size_t{192}); // once per change, not per tick
		for (size_t i = 1; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].second != p.sink.tempos[i - 1].second).to_be_true();
		}
	});

	it("changes swing only on step boundaries, stepping along its glide", _ {
		Playing p;
		p.feed(5);
		p.driver.glideSwing(16, kBar, p.sink); // about 1 a step
		p.feed(2 * kBar);
		// Written on each step line from the next one until the glide is done: a bar and one line more, since it
		// started 5 ticks past a line.
		expect(p.sink.swings.size()).to_equal(size_t{17});
		for (size_t i = 0; i < p.sink.swings.size(); i++) {
			expect(p.sink.swings[i].first % kTicksPerStep).to_equal(0);
		}
		expect(p.sink.swings.front().first).to_equal(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(16);

		// Instant still waits for the next step boundary.
		p.feed(3);
		size_t before = p.sink.swings.size();
		p.driver.glideSwing(-10, 0, p.sink);
		expect(p.sink.swings.size()).to_equal(before);
		p.feed(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(-10);
		expect(p.sink.swings.back().first % kTicksPerStep).to_equal(0);
	});

	it("writes tempo and swing at once while stopped, and a stop lands a running glide on its target", _ {
		Playing p;
		p.driver.glideTempo(14000, 4 * kBar, p.sink);
		p.driver.glideSwing(30, 4 * kBar, p.sink);
		p.feed(kBar);
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
		p.feed(100);
		p.driver.glideTempo(13184, kBar, p.sink);
		p.feed(kBar / 2);
		p.driver.jump(p.sink);
		p.feed(kBar);
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
		queue.push({CommandKind::SetTempo, 0, 13000});
		queue.push({CommandKind::SetTempo, 0, 13100}); // continuous: coalesces
		queue.push({CommandKind::SetSwing, 0, 12});
		queue.push({CommandKind::SetSwingInterval, 0, 1});
		const int32_t landing = generator.nextStep();
		driver.advance(kTicksPerStep, kGeneratorTickMagnitude, queue, &log, sink);
		expect(log.size()).to_equal(3);
		for (int32_t i = 0; i < log.size(); i++) {
			expect(log.at(i).step).to_equal(landing);
		}
		expect(log.at(0).command.value).to_equal(13100);
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
