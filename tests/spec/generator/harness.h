// The harness driver (spec §2.5, §21.2): runs the portable Perform driver with a recording sink, so specs read the
// event stream exactly as the firmware adapter would get it.

#pragma once

#include "generative/core/event.h"
#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/perform/perform_driver.h"

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
