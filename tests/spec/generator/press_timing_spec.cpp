// Where a press lands, swept over every song tick it could come on (spec §12.2, §12.7, §13.2, §13.3), with the driver
// called only when it asks, as the firmware hook calls it. A press can come between any two calls, and the next call
// isn't always on the line it lands on: a per-tick or hand-picked press never shows that (#108).

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;
constexpr int32_t kTempo = 12800;

/// A Techno piece from SHIFT+PLAY (Groove from step 0), at 128 BPM with no swing, called as the firmware hook calls
/// it. At magnitude 2 a song tick is a Generator tick, so song ticks and the driver's clock agree.
struct Piece : StepwisePiece<TimedSink> {
	Piece() : StepwisePiece({.seed = 3, .skipIntro = true, .logEntries = 8}) {
		driver.setTempo(kTempo, sink);
		driver.setSwing(0, sink);
		sink.tempos.clear();
		sink.swings.clear();
		runTicks(0);
	}
};

/// The step line at or after song tick `tick`.
int64_t lineAtOrAfter(int64_t tick) {
	return (tick + kTicksPerStep - 1) / kTicksPerStep * kTicksPerStep;
}

/// The first step a press at song tick `at` reaches: the first whose decision comes after it.
int32_t nextUndecided(int64_t at) {
	int32_t step = 0;
	while (decisionTick(step) <= at) {
		step++;
	}
	return step;
}

/// The first line every `lineSteps` steps that a press at song tick `at` reaches (spec §12.2): a press 13 or more
/// ticks ahead of a line lands on it, one 12 ahead or later on the next.
int32_t nextLine(int64_t at, int32_t lineSteps) {
	const int32_t step = nextUndecided(at);
	return (step + lineSteps - 1) / lineSteps * lineSteps;
}

/// Presses at every song tick across the two bars either side of the first phrase line, checking each with `check`
/// (which returns what's wrong with a press at that tick, or ""). @return The first few failures, or "".
std::string sweep(const std::function<std::string(int64_t)>& check) {
	std::string failures;
	int32_t failed = 0;
	for (int64_t at = stepTick(kPhraseSteps - 2 * kStepsPerBar); at < stepTick(kPhraseSteps + 2 * kStepsPerBar); at++) {
		std::string wrong = check(at);
		if (!wrong.empty() && failed++ < 4) {
			failures += "pressed at " + std::to_string(at) + ": " + wrong + "; ";
		}
	}
	return failed == 0 ? "" : std::to_string(failed) + " presses wrong: " + failures;
}

/// A tempo or swing edit as the adapter makes it (spec §12.7): the glide starts, and the hook is woken for the next
/// tick (expectEvent()).
Action tempoEdit(Piece& d, int64_t at, int32_t tempo, int32_t ticks) {
	return {at, [&d, tempo, ticks] { d.driver.glideTempo(tempo, ticks, d.sink); }, true};
}
Action swingEdit(Piece& d, int64_t at, int32_t amount, int32_t ticks) {
	return {at, [&d, amount, ticks] { d.driver.glideSwing(amount, ticks, d.sink); }, true};
}

/// Checks a glide of swing from 0 to `amount` over `ticks`, pressed at `at`: it counts from the call the wake brings
/// on the next tick, and writes on every step line from the first after the press until it lands, and nowhere else.
std::string swingGlide(int64_t at, int32_t amount, int32_t ticks) {
	Piece d;
	d.runTicks(at + ticks + 2 * kTicksPerStep, {swingEdit(d, at, amount, ticks)});
	const int64_t start = at + 1;
	int64_t line = lineAtOrAfter(start);
	const int64_t last = lineAtOrAfter(start + ticks);
	size_t i = 0;
	for (; line <= last; line += kTicksPerStep, i++) {
		const int64_t elapsed = std::min<int64_t>(line - start, ticks);
		const int32_t want = ticks == 0 ? amount : static_cast<int32_t>(amount * elapsed / ticks);
		if (i >= d.sink.swings.size()) {
			return "no swing written on line " + std::to_string(line);
		}
		auto [when, value] = d.sink.swings[i];
		if (when != line || value != want) {
			return "swing " + std::to_string(value) + " written at " + std::to_string(when) + ", wanted "
			       + std::to_string(want) + " at " + std::to_string(line);
		}
	}
	return i == d.sink.swings.size() ? "" : "swing written after it landed";
}

/// Checks a tempo glide from 128 BPM to `tempo` over `ticks`, pressed at `at`. Instant writes it at once; a glide
/// counts from the call the wake brings on the next tick, and writes every tick that changes it.
std::string tempoGlide(int64_t at, int32_t tempo, int32_t ticks) {
	Piece d;
	d.runTicks(at + ticks + kTicksPerStep, {tempoEdit(d, at, tempo, ticks)});
	if (ticks == 0) {
		const bool once = d.sink.tempos.size() == 1 && d.sink.tempos[0] == std::pair<int64_t, int32_t>{at, tempo};
		return once ? "" : "not written once, at the press";
	}
	const int64_t start = at + 1;
	std::vector<std::pair<int64_t, int32_t>> want;
	int32_t last = kTempo;
	for (int64_t t = start + 1; t <= start + ticks; t++) {
		const int32_t value = kTempo + static_cast<int32_t>((tempo - kTempo) * (t - start) / ticks);
		if (value != last) {
			want.push_back({t, value});
			last = value;
		}
	}
	if (d.sink.tempos == want) {
		return "";
	}
	for (size_t i = 0; i < want.size(); i++) {
		if (i >= d.sink.tempos.size() || d.sink.tempos[i] != want[i]) {
			return "write " + std::to_string(i) + " wanted " + std::to_string(want[i].second) + " at "
			       + std::to_string(want[i].first);
		}
	}
	return "written after it landed";
}

/// Checks a press of `command` at `at`: it's logged once (as the core resolved it) on `step`, and the Section `starts`
/// (if any) plays from that step's line, on time.
std::string landsOn(int64_t at, Command command, int32_t step, std::optional<SectionKind> starts = std::nullopt) {
	Piece d;
	// A press that only reaches the steering queue, as End, a force or a mute does.
	d.runToStepLine(step + 1, 0, {d.press(at, command)});
	if (d.log.size() != 1 || d.log.at(0).command.kind != command.kind) {
		return "logged " + std::to_string(d.log.size()) + " times";
	}
	if (d.log.at(0).step != step) {
		return "landed on step " + std::to_string(d.log.at(0).step) + ", wanted " + std::to_string(step);
	}
	if (starts) {
		for (auto& [event, when] : d.sink.played) {
			if (event.kind == EventKind::Section && event.section == *starts) {
				if (event.tick != stepTick(step) || when != event.tick) {
					return "Section start at tick " + std::to_string(event.tick) + " played at " + std::to_string(when);
				}
				return "";
			}
		}
		return "no Section start";
	}
	return "";
}

} // namespace

// clang-format off
describe press_timing("Presses at every tick, with the driver called only when it asks", $ {
	it("lands an Instant swing on the next step line after the press", _ {
		expect(sweep([](int64_t at) { return swingGlide(at, -10, 0); })).to_equal(std::string{});
	});

	it("glides swing from the press, on every step line until it lands", _ {
		expect(sweep([](int64_t at) { return swingGlide(at, 16, kBarTicks); })).to_equal(std::string{});
	});

	it("sets an Instant tempo at the press", _ {
		expect(sweep([](int64_t at) { return tempoGlide(at, 13184, 0); })).to_equal(std::string{});
	});

	it("glides tempo from the tick after the press, every tick that changes it", _ {
		expect(sweep([](int64_t at) { return tempoGlide(at, 13184, kBarTicks); })).to_equal(std::string{});
		expect(sweep([](int64_t at) { return tempoGlide(at, 12608, kBarTicks / 4); })).to_equal(std::string{});
	});

	it("starts the Outro after End on the next phrase line the press reaches", _ {
		const Command end{CommandKind::End, 0, 1};
		expect(sweep([&end](int64_t at) {
			return landsOn(at, end, nextLine(at, kPhraseSteps), SectionKind::Outro);
		})).to_equal(std::string{});
	});

	it("starts a forced Section on the next phrase line the press reaches", _ {
		const Command force{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0};
		expect(sweep([&force](int64_t at) {
			return landsOn(at, force, nextLine(at, kPhraseSteps), SectionKind::Break);
		})).to_equal(std::string{});
	});

	it("lands a mute on the next bar line the press reaches, and SHIFT+mute on the next undecided step", _ {
		const Command mute{CommandKind::SetMute, 1, kPress};
		expect(sweep([&mute](int64_t at) {
			return landsOn(at, mute, nextLine(at, kStepsPerBar));
		})).to_equal(std::string{});
		const Command now{CommandKind::SetMute, 1, kPressNow};
		expect(sweep([&now](int64_t at) { return landsOn(at, now, nextUndecided(at)); })).to_equal(std::string{});
	});
});

CPPSPEC_SPEC(press_timing)
