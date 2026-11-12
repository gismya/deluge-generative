// The harness driver (spec §2.5, §21.2): runs the portable Perform driver with a recording sink, so specs read the
// event stream exactly as the firmware adapter would get it.
//
// Two ways to run a piece:
//   play(run, bars)  a whole piece in one go, steered by a callback. For specs that read the finished stream.
//   StepwisePiece    a piece a spec steps through: push a command, run on, look, and again. It owns the Generator,
//                    the queue, the sink, the driver and the Hook that calls it as the firmware does.

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
#include <stdexcept>
#include <utility>
#include <vector>

namespace harness {

using namespace deluge::generative;

/// Generator ticks in a bar.
constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

/// Song ticks at `magnitude` for `ticks` Generator ticks, rounded up: when the driver's clock has reached them.
/// For non-negative `ticks`: below magnitude 2 a negative count rounds towards 0.
constexpr int64_t songTicksFor(int32_t ticks, int32_t magnitude) {
	if (magnitude >= kGeneratorTickMagnitude) {
		return int64_t{ticks} * (int64_t{1} << (magnitude - kGeneratorTickMagnitude));
	}
	const int32_t per = 1 << (kGeneratorTickMagnitude - magnitude);
	return (ticks + per - 1) / per;
}

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
/// only at those calls is a firmware bug, and calling every tick hides it (#108). call() and callAt() are for a call
/// the driver didn't ask for. A spec that means another cadence, one call a song tick say, uses play() with
/// Run::ticksPerCall.
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
	/// Whether the first call has been made.
	[[nodiscard]] bool called() const { return called_; }

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
	/// Back to tick 0 with no call made, for a driver that has begun again.
	void reset() {
		now_ = due_ = lastCall_ = 0;
		called_ = false;
	}

private:
	void advance(int32_t delta) {
		called_ = true;
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
	bool called_ = false;

public:
	int32_t magnitude; ///< The song's tick magnitude, for the calls from here on.
};

/// Keeps what the driver plays and writes with the song tick (the Hook's now()) it did it at, and where each jump was
/// reported: the index of the next event to play. For a StepwisePiece<TimedSink>.
struct TimedSink final : perform::NoteSink {
	const Hook* hook = nullptr;
	std::vector<std::pair<Event, int64_t>> played;
	std::vector<size_t> jumps;
	std::vector<std::pair<int64_t, int32_t>> tempos; ///< (song tick, tempo written)
	std::vector<std::pair<int64_t, int32_t>> swings; ///< (song tick, swing written)
	void attach(const perform::PerformDriver& /*driver*/, const Hook& calls) { hook = &calls; }
	void play(const Event& event) override { played.push_back({event, hook->now()}); }
	void jumped() override { jumps.push_back(played.size()); }
	void setTempo(int32_t tempo) override { tempos.push_back({hook->now(), tempo}); }
	void setSwing(int32_t amount) override { swings.push_back({hook->now(), amount}); }
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

/// Techno with its parts edited, for specs that look at one sound-control layer alone. Lives at a stable address,
/// since a Recipe points at its genre.
struct EditedGenre {
	std::vector<PartSpec> parts;
	Genre genre;
};
inline std::shared_ptr<EditedGenre> technoEdited(const std::function<void(PartSpec&)>& edit) {
	auto edited = std::make_shared<EditedGenre>();
	edited->parts.assign(genres::techno.parts.begin(), genres::techno.parts.end());
	for (PartSpec& part : edited->parts) {
		edit(part);
	}
	edited->genre = genres::techno;
	edited->genre.parts = edited->parts;
	return edited;
}
/// Every gesture switched off on a part.
constexpr uint16_t kAllMoments = 0x1FF;
/// The macro layer alone: no gesture and no drift on any part.
inline void macroLayerOnly(PartSpec& part) {
	part.gestures.off = kAllMoments;
	part.drift.weight = 0;
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

/// What a StepwisePiece starts from, as play()'s Run: a Techno piece from its Intro unless the spec says otherwise.
struct Start {
	Recipe recipe = technoRecipe();
	uint32_t seed = 1;
	Takes takes{};
	/// SHIFT+PLAY: start in Groove, with no Intro (spec §13.2).
	bool skipIntro = false;
	int32_t magnitude = kGeneratorTickMagnitude;
	/// Where the master is at the start, in song ticks: past 0 for a Continue (spec §12.4).
	int32_t songTick = 0;
	/// Room in the piece's SteeringLog. With 0 the piece runs with no log.
	int32_t logEntries = 0;
};

/// A piece a spec steps through (spec §2.5, §12.3): a Generator, the steering queue, a sink, the Perform driver and
/// the Hook that calls it only when it asks, as the firmware does. The spec pushes a command, runs on, and looks.
///
///   StepwisePiece<> piece{{.seed = 3, .skipIntro = true}};
///   piece.decideBefore(5);                 // steps 0-4 are decided
///   piece.push({CommandKind::SetMute, 1, kPress});
///   piece.decideBefore(6);                 // step 5's decision took the mute into the pending list
///   piece.runToStepLine(16);               // the bar line it lands on has played
///
/// Nothing is called until the first run: a command pushed before it is there for step 0's decision. runTicks(0)
/// makes only that first call.
///
/// The two ways to reach a step:
///   decideBefore(step)   "decided up to": `step` is the next to decide. If it had to move, the clock stands at
///                        step - 1's decision tick, 12 ticks before its line: step - 1 is decided, not heard. A push
///                        now is in time for `step`. Use it to say which decision a command reaches.
///   runToStepLine(step)  "the clock is at the line": everything on `step`'s grid line has played, and step + 1 is
///                        not decided yet (with `ticks` under 12). A push now is in time for step + 1. Use it to say
///                        what has been heard.
/// Both count on the instance that plays: after a transition's start step that is the incoming one, whose steps and
/// clock start again from 0.
///
/// `Sink` is what the driver plays into: a RecordingSink, or the spec's own. A sink with attach(driver, hook) gets
/// them before the piece starts, to stamp what it is given with the driver's clock or the Hook's song tick. A spec
/// that needs more at the start (a tempo, a decision timer, a second instance) derives from StepwisePiece and adds it.
template <typename Sink = RecordingSink>
struct StepwisePiece {
	Generator generator;
	SteeringQueue queue;
	Sink sink;
	perform::PerformDriver driver;
	std::vector<LogEntry> logEntries;
	/// What the core logged as steering landed. Empty, and not passed to the driver, unless Start::logEntries is set.
	SteeringLog log;
	/// The calls to the driver. Reach for it for a call the driver didn't ask for (call(), callAt()), a wake(), or
	/// to wait for a kind of call (runUntil()).
	Hook hook;

	explicit StepwisePiece(const Start& start = {})
	    : logEntries(static_cast<size_t>(start.logEntries)), log(logEntries.data(), start.logEntries),
	      hook(driver, queue, sink, start.magnitude, start.logEntries > 0 ? &log : nullptr), start_(start) {
		if constexpr (requires(const perform::PerformDriver& d, const Hook& h) { sink.attach(d, h); }) {
			sink.attach(driver, hook);
		}
		else {
			// A sink whose attach() takes something else would never be attached, and fail far from here.
			static_assert(
			    !requires { &Sink::attach; }, "the sink's attach() must take (const PerformDriver&, const Hook&)");
		}
		begin(start);
	}
	StepwisePiece(const StepwisePiece&) = delete;
	StepwisePiece& operator=(const StepwisePiece&) = delete;

	/// Stops the piece and starts another from `start` on the same Generator, as PLAY after a stop does: the steering
	/// queue and the log are cleared. The sink keeps what it holds. The magnitude and the log's size stay as
	/// constructed.
	void restart(const Start& start) {
		if (driver.running()) {
			driver.stop(sink);
		}
		queue.clear();
		log.clear();
		hook.reset();
		start_ = start;
		begin(start);
	}
	/// As restart(start), from the Start the piece last started from.
	void restart() { restart(Start{start_}); }

	/// A press now: the command goes onto the steering queue, for the next decision. @return false if it was full.
	bool push(const Command& command) { return queue.push(command); }
	/// The last Section event played, as the firmware's sink keeps it for the Perform view's pages; nullptr before
	/// any. For a sink that records its events, as RecordingSink does.
	[[nodiscard]] const Event* lastSection() const {
		const Event* last = nullptr;
		for (const Event& event : sink.events) {
			last = event.kind == EventKind::Section ? &event : last;
		}
		return last;
	}
	/// The phrase length of the genre the piece plays, in steps.
	[[nodiscard]] int32_t phraseSteps() const { return generator.recipe().genre->phraseSteps(); }
	/// A press at song tick `at` during a run, for runTicks() and its kin: after the call at that tick, if any.
	[[nodiscard]] Action press(int64_t at, Command command) {
		return {at, [this, command] { queue.push(command); }};
	}
	/// Pushes a command, then has the instance that plays decide one more step, so the core has taken it into its
	/// pending list. @return as decideBefore(); false at once, with the command pushed, if nothing plays.
	bool pushAndDecide(const Command& command) {
		push(command);
		const Generator* playing = driver.playing();
		return playing != nullptr && decideBefore(playing->nextStep() + 1);
	}

	/// Decides every step before `step` on the instance that plays now: plays until `step` is its next to decide.
	/// Does nothing if it is there already. @return whether it got there. False if that instance stopped deciding
	/// first: the piece ended or was stopped, or a transition's incoming instance took over. Throws if `step` is too
	/// far to reach.
	bool decideBefore(int32_t step) {
		const Generator* playing = driver.playing();
		if (playing == nullptr) {
			return false;
		}
		const bool reached = hook.runUntil([&] { return driver.playing() != playing || playing->nextStep() >= step; });
		if (!reached) {
			throw std::runtime_error("decideBefore: the step was not reached");
		}
		return driver.playing() == playing;
	}
	/// Plays until the clock is at `step`'s grid line, or `ticks` Generator ticks past it (before it, if negative).
	/// If the clock is there already it does nothing, and `actions` are dropped; a fresh piece still gets its first
	/// call. Counts from the last call, so it is for a piece that plays in time: not across a jump or a transition's
	/// start. Exact where the line falls on a whole song tick: at magnitude 2, and above it from a call that left no
	/// part of a Generator tick over.
	void runToStepLine(int32_t step, int32_t ticks = 0, const std::vector<Action>& actions = {}) {
		const int64_t tick = hook.lastCall() + songTicksFor(stepTick(step) + ticks - driver.clock(), hook.magnitude);
		if (tick > hook.now() || !hook.called()) {
			hook.runTo(std::max(tick, hook.now()), actions);
		}
	}

	/// Lets `songTicks` go by, with `actions` between the calls (see Hook::run).
	void runTicks(int64_t songTicks, const std::vector<Action>& actions = {}) { hook.run(songTicks, actions); }
	/// As runTicks(), up to song tick `tick`.
	void runToTick(int64_t tick, const std::vector<Action>& actions = {}) { hook.runTo(tick, actions); }
	/// Lets `steps` 16ths of song time go by.
	void runSteps(int32_t steps) { hook.run(steps * songTicksFor(kTicksPerStep, hook.magnitude)); }
	/// Lets `bars` bars of song time go by.
	void runBars(int32_t bars) { runSteps(bars * kStepsPerBar); }

private:
	void begin(const Start& start) {
		generator.start(start.recipe, start.seed, start.takes, start.skipIntro);
		driver.begin(generator, start.songTick, hook.magnitude);
	}
	Start start_;
};

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
