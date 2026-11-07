// The harness driver (spec §2.5, §21.2): runs the portable Perform driver with a recording sink, so specs read the
// event stream exactly as the firmware adapter would get it.

#pragma once

#include "generative/core/event.h"
#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/perform/perform_driver.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace harness {

using namespace deluge::generative;

/// Keeps every event the driver plays, in play order.
struct RecordingSink final : perform::NoteSink {
	std::vector<Event> events;
	void play(const Event& event) override { events.push_back(event); }
};

/// Something that happens between hook calls, such as a press or a tempo turn, at song tick `at` (song ticks since
/// the Hook's first call). It comes after the hook's call at that tick, if there is one, so it reaches the first call
/// after it: a press at a line's decision tick is too late for that line (spec §12.2). With `wake`, it also asks for
/// a call on the next tick, as the adapter's expectEvent() does after a tempo or swing edit.
struct Action {
	int64_t at = 0;
	std::function<void()> act;
	bool wake = false;
};

/// Calls a Perform driver the way the firmware's tick hook does (spec §12.3): only when the wait it last returned is
/// up, with every song tick since its last call. This is how specs drive the driver over time, since a bug that shows
/// only at those calls is a firmware bug, and calling every tick hides it (#108). everyTick() is the opt-in for a spec
/// that means one call a song tick, and call() and callAt() for a call the driver didn't ask for.
class Hook {
public:
	Hook(perform::PerformDriver& driver, SteeringQueue& queue, perform::NoteSink& sink,
	     int32_t magnitude = kGeneratorTickMagnitude, SteeringLog* log = nullptr)
	    : driver_(driver), queue_(queue), sink_(sink), log_(log), magnitude(magnitude) {}

	/// The song tick it is now, between calls: song ticks since the first call.
	[[nodiscard]] int64_t now() const { return now_; }
	/// The song tick of the next call the driver asked for.
	[[nodiscard]] int64_t due() const { return due_; }
	/// Song ticks from now until that call.
	[[nodiscard]] int64_t wait() const { return due_ - now_; }
	/// The song tick of the last call.
	[[nodiscard]] int64_t lastCall() const { return lastCall_; }

	/// Lets `songTicks` go by, calling the driver whenever its wait is up, with `actions` (in order of `at`, each
	/// within the run) between the calls. The first run makes the first call, with no ticks, at tick 0.
	void run(int64_t songTicks, const std::vector<Action>& actions = {}) {
		const int64_t end = now_ + songTicks;
		size_t next = 0;
		while (true) {
			for (; next < actions.size() && actions[next].at < due_ && actions[next].at <= end; next++) {
				now_ = std::max(now_, actions[next].at);
				actions[next].act();
				if (actions[next].wake) {
					wake();
				}
			}
			if (due_ > end) {
				break;
			}
			now_ = due_;
			advance(now_ - lastCall_);
		}
		now_ = end;
	}
	/// As run(), up to song tick `tick`.
	void runTo(int64_t tick, const std::vector<Action>& actions = {}) { run(tick - now_, actions); }
	/// Goes on to the next call the driver asked for, and makes it.
	void next() { runTo(due_); }
	/// Makes the calls the driver asks for until `ready()` holds between two of them. @return false if it didn't
	/// within `calls` calls.
	bool runUntil(const std::function<bool()>& ready, int32_t calls = 100000) {
		for (int32_t i = 0; i < calls; i++) {
			if (ready()) {
				return true;
			}
			next();
		}
		return ready();
	}
	/// Asks for a call on the next song tick, as the adapter's expectEvent() does after a tempo or swing edit.
	void wake() { due_ = std::min(due_, now_ + 1); }

	/// A call now, with the song ticks since the last call plus `moved`: how far the master moved besides, as for a
	/// jump (negative, or more than a bar).
	void call(int32_t moved = 0) { advance(static_cast<int32_t>(now_ - lastCall_) + moved); }
	/// A call at `tick`, whatever the driver asked for: a late one, say.
	void callAt(int64_t tick) {
		now_ = tick;
		call();
	}
	/// One call a song tick for `songTicks`, whatever the driver asked for, as when something else in the song runs
	/// the hook every tick. Only for a spec that means that cadence.
	void everyTick(int64_t songTicks) {
		for (int64_t t = 0; t < songTicks; t++) {
			now_++;
			call();
		}
	}

private:
	void advance(int32_t delta) {
		lastCall_ = now_;
		due_ = now_ + driver_.advance(delta, magnitude, queue_, log_, sink_);
	}

	perform::PerformDriver& driver_;
	SteeringQueue& queue_;
	perform::NoteSink& sink_;
	SteeringLog* log_;
	int64_t now_ = 0;
	int64_t due_ = 0;
	int64_t lastCall_ = 0;

public:
	int32_t magnitude; ///< The song's tick magnitude, for the calls from here on.
};

struct Run {
	Recipe recipe;
	uint32_t seed = 1;
	Takes takes;
	/// Called before each driver call with the step about to be decided next, so a spec can steer.
	std::function<void(int32_t nextStep, SteeringQueue&)> steer;
	/// Song ticks per driver call. 0 calls the driver exactly when its return value says it is next due.
	int32_t ticksPerCall = 0;
	int32_t magnitude = kGeneratorTickMagnitude;
	/// Where the core logs steering as it lands, if a spec wants to read it.
	SteeringLog* log = nullptr;
	/// SHIFT+PLAY: start in Groove, with no Intro (spec §13.2). Specs that want the Recipe's macros to play as set
	/// from the first bar use it, since an Intro's Density rises from the Intro floor (spec §9.4).
	bool skipIntro = false;
};

struct Result {
	std::vector<Event> events;
	uint32_t droppedEvents = 0;
	uint32_t evictedNotes = 0;
	uint32_t refusedSteering = 0;
	/// The piece ended by itself, at the end of its Outro (spec §13.2), at this driver clock.
	bool ended = false;
	int32_t endedAt = 0;
	std::unique_ptr<Generator> generator; ///< Left as it ended, for queries.
};

/// Plays `run` through the Perform driver for `bars` bars of song time, then stops (flushes) the driver.
inline Result play(const Run& run, int32_t bars) {
	Result result;
	result.generator = std::make_unique<Generator>();
	Generator& generator = *result.generator;
	SteeringQueue queue;
	RecordingSink sink;
	perform::PerformDriver driver;

	generator.start(run.recipe, run.seed, run.takes, run.skipIntro);
	driver.begin(generator);

	const int64_t songTicksPerStep = int64_t{6} << run.magnitude;
	const int64_t end = bars * kStepsPerBar * songTicksPerStep;
	int64_t now = 0;
	int32_t delta = 0;
	while (now < end) {
		if (run.steer) {
			run.steer(generator.nextStep(), queue);
		}
		int32_t wait = driver.advance(delta, run.magnitude, queue, run.log, sink);
		if (driver.ended()) {
			result.ended = true;
			result.endedAt = driver.clock();
			break;
		}
		delta = run.ticksPerCall > 0 ? run.ticksPerCall : wait;
		now += delta;
	}
	driver.stop(sink);
	result.events = std::move(sink.events);
	result.droppedEvents = generator.droppedEvents();
	result.evictedNotes = generator.evictedNotes();
	result.refusedSteering = generator.refusedSteering();
	return result;
}

/// The Techno default Recipe with its first palette entries and fixed facts, so a spec's expectations hold.
inline Recipe technoRecipe() {
	Recipe recipe = newRecipe(genres::techno, 0);
	recipe.keyRoot = 9; // A
	recipe.scale = Scale::Minor;
	return recipe;
}

/// A steering callback that pushes each command once, when the driver is about to decide its step.
inline std::function<void(int32_t, SteeringQueue&)> steerAt(std::vector<std::pair<int32_t, Command>> commands) {
	auto pushed = std::make_shared<std::vector<bool>>(commands.size(), false);
	return [=](int32_t nextStep, SteeringQueue& queue) {
		for (size_t i = 0; i < commands.size(); i++) {
			if (commands[i].first == nextStep && !(*pushed)[i]) {
				(*pushed)[i] = queue.push(commands[i].second);
			}
		}
	};
}

/// Steers one command onto the queue when the driver is about to decide `step`.
inline std::function<void(int32_t, SteeringQueue&)> at(int32_t step, Command command) {
	return steerAt({{step, command}});
}

inline std::vector<Event> only(const std::vector<Event>& events, EventKind kind) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.kind == kind) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace harness
