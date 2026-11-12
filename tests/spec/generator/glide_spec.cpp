// Tempo and swing glides (spec §12.7, §13.3): the Perform driver writes song tempo once per tick along a line, and
// swing only on step boundaries.

#include "generative/perform/glide.h"
#include "harness.h"

#include "cppspec.hpp"

#include <stdexcept>
#include <vector>

using namespace harness;

namespace {

/// Keeps every tempo and swing write with the driver clock it was made at.
struct TransportSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< (clock, hundredths of a BPM)
	std::vector<std::pair<int32_t, int32_t>> swings; ///< (clock, amount)
	void attach(const perform::PerformDriver& playing, const Hook& /*hook*/) { driver = &playing; }
	void play(const Event&) override {}
	void setTempo(int32_t tempo) override { tempos.push_back({driver ? driver->clock() : -1, tempo}); }
	void setSwing(int32_t amount) override { swings.push_back({driver ? driver->clock() : -1, amount}); }
};

/// A Techno piece from SHIFT+PLAY at 128 BPM with no swing, its first call made.
struct Playing : StepwisePiece<TransportSink> {
	Playing() : StepwisePiece({.seed = 3, .skipIntro = true}) {
		driver.setTempo(12800, sink);
		driver.setSwing(0, sink);
		sink.tempos.clear();
		sink.swings.clear();
		runTicks(0);
	}
	/// A tempo or swing edit with the Glide setting that lasts `ticks`: the steering starts the glide, and wakes the
	/// hook for the next tick.
	void glideTempo(int32_t tempo, int32_t ticks) {
		steering.setGlide(glideLasting(ticks, phraseSteps()));
		if (steering.setTempo(tempo) != perform::PressOutcome::Accepted) {
			throw std::runtime_error("tempo edit not accepted");
		}
	}
	void glideSwing(int32_t amount, int32_t ticks) {
		steering.setGlide(glideLasting(ticks, phraseSteps()));
		if (steering.setSwing(amount) != perform::PressOutcome::Accepted) {
			throw std::runtime_error("swing edit not accepted");
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
		p.runTicks(10);
		// Pressed at tick 10: the glide counts from the call the wake asks for, on 11, and asks for every tick after.
		p.glideTempo(13184, kBar); // +3.84 BPM over a bar: 1 hundredth a tick
		p.runTicks(kBar + 50);
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
		p.runToTick(kFirst + kBar / 2);
		const size_t firstGlide = p.sink.tempos.size();
		constexpr int32_t kSecond = kFirst + kBar / 2 + 1;
		p.glideTempo(12800, kBar); // from 12992, down 192 over a bar: a hundredth every 2 ticks
		p.runToTick(kSecond + kBar + 10);
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
		p.runTicks(5);
		p.glideSwing(16, kBar); // about 1 a step
		p.runTicks(2 * kBar);
		// Written on each step line from the next one until the glide is done: a bar and one line more, since it
		// started 5 ticks past a line.
		expect(p.sink.swings.size()).to_equal(size_t{17});
		for (size_t i = 0; i < p.sink.swings.size(); i++) {
			expect(p.sink.swings[i].first % kTicksPerStep).to_equal(0);
		}
		expect(p.sink.swings.front().first).to_equal(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(16);

		// Instant still waits for the next step boundary.
		p.runTicks(3);
		size_t before = p.sink.swings.size();
		p.glideSwing(-10, 0);
		expect(p.sink.swings.size()).to_equal(before);
		p.runTicks(kTicksPerStep);
		expect(p.sink.swings.back().second).to_equal(-10);
		expect(p.sink.swings.back().first % kTicksPerStep).to_equal(0);
	});

	it("writes tempo and swing at once while stopped, and a stop lands a running glide on its target", _ {
		Playing p;
		p.glideTempo(14000, 4 * kBar);
		p.glideSwing(30, 4 * kBar);
		p.runTicks(kBar);
		p.driver.stop(p.sink);
		expect(p.sink.tempos.back().second).to_equal(14000);
		expect(p.sink.swings.back().second).to_equal(30);

		p.glideTempo(12000, 4 * kBar);
		expect(p.sink.tempos.back().second).to_equal(12000);
		p.glideSwing(-5, 4 * kBar);
		expect(p.sink.swings.back().second).to_equal(-5);
	});

	it("keeps gliding across a jump, by the ticks that played", _ {
		Playing p;
		p.runTicks(100);
		p.glideTempo(13184, kBar);
		p.runTicks(kBar / 2);
		p.driver.jump(p.sink);
		p.runTicks(kBar);
		expect(p.sink.tempos.back().second).to_equal(13184);
		for (size_t i = 1; i < p.sink.tempos.size(); i++) {
			expect(p.sink.tempos[i].second - p.sink.tempos[i - 1].second).to_equal(1);
		}
	});

	it("logs tempo, swing and the swing interval as driver-side steering, on the next undecided step", _ {
		StepwisePiece<> piece{{.seed = 3, .skipIntro = true, .logEntries = 8}};
		piece.runTicks(0);
		// Glides log as {target, length in ticks} (spec §17.2); the later one coalesces over both.
		piece.push({CommandKind::SetTempo, 0, 13000, 96});
		piece.push({CommandKind::SetTempo, 0, 13100, 384}); // continuous: coalesces
		piece.push({CommandKind::SetSwing, 0, 12, 0});
		piece.push({CommandKind::SetSwingInterval, 0, 1});
		const int32_t landing = piece.generator.nextStep();
		piece.runSteps(1);
		expect(piece.log.size()).to_equal(3);
		for (int32_t i = 0; i < piece.log.size(); i++) {
			expect(piece.log.at(i).step).to_equal(landing);
		}
		expect(piece.log.at(0).command.value).to_equal(13100);
		expect(static_cast<int32_t>(piece.log.at(0).command.length)).to_equal(384);
		expect(static_cast<int32_t>(piece.log.at(1).command.length)).to_equal(0);
		expect(piece.generator.recipe().tempo).to_equal(13100);
		expect(static_cast<int32_t>(piece.generator.recipe().swing)).to_equal(12);
		expect(static_cast<int32_t>(piece.generator.recipe().swingInterval)).to_equal(1);
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
