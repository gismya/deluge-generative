// A stepwise piece with a transition's lifecycle on it (spec §12.9): the real core, Perform driver and
// TransitionLifecycle, with an adapter that keeps plain records where the firmware has a job, Outputs and the Current
// Recipe. For transitions_spec and transition_lifecycle_spec.
#pragma once

#include "generative/perform/transition_lifecycle.h"
#include "harness.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace harness {

struct TransitionAdapter;

/// One event as played, on one timeline: Generator ticks since the outgoing piece's step 0.
struct Played {
	Event event;
	bool incoming = false; ///< From the incoming instance, played from the start step on.
	bool outgoing = false; ///< Drained from the outgoing instance after the start step.
	int32_t at = 0;
};

/// Records what the driver plays, with the tempo and swing it writes, and hands the transition's callbacks on to the
/// lifecycle, as the firmware's OutputSink does.
struct TransitionSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	TransitionAdapter* adapter = nullptr;
	std::vector<Played> played;
	int32_t starts = 0;
	int32_t drained = 0;
	int32_t base = 0;                                ///< Where the playing instance's tick 0 is on the timeline.
	int32_t outgoingBase = 0;                        ///< The outgoing one's, once it drains.
	int32_t startAt = -1;                            ///< The start step's tick on the timeline.
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< {driver clock on the timeline, tempo}
	std::vector<std::pair<int32_t, int32_t>> swings;

	void attach(const perform::PerformDriver& playing, const Hook& /*hook*/) { driver = &playing; }
	void play(const Event& event) override { played.push_back({event, starts > 0, false, event.tick + base}); }
	void playOutgoing(const Event& event) override {
		played.push_back({event, false, true, event.tick + outgoingBase});
	}
	void transitionStarts(int32_t startTick) override;
	void transitionLanded() override;
	void transitionDropped() override;
	void outgoingDrained() override;
	void setTempo(int32_t tempo) override { tempos.emplace_back(driver->clock() + base, tempo); }
	void setSwing(int32_t amount) override { swings.emplace_back(driver->clock() + base, amount); }

	/// The first and last of from(): a default event when there are none, so a failing spec fails instead of crashing.
	[[nodiscard]] Played first(bool incoming, EventKind kind) const {
		std::vector<Played> all = from(incoming, kind);
		return all.empty() ? Played{} : all.front();
	}
	[[nodiscard]] Played last(bool incoming, EventKind kind) const {
		std::vector<Played> all = from(incoming, kind);
		return all.empty() ? Played{} : all.back();
	}
	/// The played events of one kind, from the incoming instance or the outgoing one.
	[[nodiscard]] std::vector<Played> from(bool incoming, EventKind kind) const {
		std::vector<Played> out;
		for (const Played& p : played) {
			if (p.event.kind == kind && p.incoming == incoming) {
				out.push_back(p);
			}
		}
		return out;
	}
};

/// The lifecycle's adapter in the harness. Rosters are numbers: 1 plays at the start, and each preload that ends well
/// builds the next. A spec ends a preload itself, with load() or failLoad(), as the runner would.
struct TransitionAdapter : perform::TransitionLifecycle<TransitionAdapter> {
	/// A roster handed to be freed.
	struct Retired {
		int32_t roster = 0;
		bool played = false; ///< It rings out first, `cap` at most. False: it never played, and goes at once.
		uint32_t cap = 0;
		bool operator==(const Retired&) const = default;
	};

	TransitionAdapter(StepwisePiece<TransitionSink>& piece, Recipe pending)
	    : pendingRecipe(std::move(pending)), piece_(piece), current_(&piece.generator) {
		piece.sink.adapter = this;
	}

	// ---- What a spec sets ----

	Recipe pendingRecipe;      ///< What the incoming instance starts with.
	int32_t glide = kBarTicks; ///< The Perform Glide setting, in Generator ticks.
	bool entered = true;       ///< Perform is entered with a genre.
	bool left = false;         ///< Perform is being left.
	bool transport = true;     ///< Playback runs.
	bool preloadStarts = true; ///< Whether the runner takes a preload.
	uint32_t phrase = 0;       ///< A phrase, in song ticks: the unit of now().
	int32_t freeing = 0;       ///< Retired rosters not freed yet: freeRetired() frees them.

	// ---- What it records ----

	std::vector<std::string> events; ///< Each `gen_transition` test event's text after that prefix.
	std::vector<uint32_t> preloads;  ///< The request of each preload started.
	int32_t cancelledPreloads = 0;
	int32_t endedPreloads = 0; ///< Preloads that ended with no roster and none following.
	int32_t soundDrops = 0;    ///< Confirms that dropped the queued sound actions.
	int32_t queueFullPopups = 0;
	std::vector<bool> plays; ///< Each startPlayback(), with its `groove`.
	std::vector<Retired> retired;
	std::vector<int32_t> discarded; ///< Rosters of preloads that ended for an older request.
	int32_t built = 1;              ///< The newest roster built.
	int32_t playingRoster = 1;
	int32_t incomingRoster = 0; ///< 0: none.
	int32_t outgoingRoster = 0;
	int32_t currentRecipe = 1; ///< The roster whose Recipe is the Current Recipe.

	// ---- The runner's part ----

	/// The preload for `request` (the newest by default) ended well, with a new roster. @return that roster.
	int32_t load(uint32_t request = 0) {
		built++;
		if (!loaded(request != 0 ? request : preloads.back())) {
			discarded.push_back(built);
		}
		return built;
	}
	/// The preload for `request` (the newest by default) was cancelled or failed.
	void failLoad(uint32_t request = 0) { loadEnded(request != 0 ? request : preloads.back()); }
	/// The free job has freed every retired roster.
	void freeRetired() { freeing = 0; }

	/// The instance the adapter plays, or would start: the incoming one from its start step.
	[[nodiscard]] Generator& current() { return *current_; }
	[[nodiscard]] bool has(const std::string& event) const {
		return std::find(events.begin(), events.end(), event) != events.end();
	}

	// ---- The lifecycle's adapter ----

	[[nodiscard]] perform::PerformDriver& driver() { return piece_.driver; }
	[[nodiscard]] SteeringQueue& queue() { return piece_.queue; }
	[[nodiscard]] perform::NoteSink& sink() { return piece_.sink; }
	[[nodiscard]] bool ready() const { return entered && !left; }
	[[nodiscard]] bool leaving() const { return left; }
	[[nodiscard]] bool transportRunning() const { return transport; }
	void startPlayback(bool groove) {
		plays.push_back(groove);
		transport = true;
	}
	[[nodiscard]] int32_t cutGlideTicks() const { return glide; }
	bool submitPreload(uint32_t request) {
		if (preloadStarts) {
			preloads.push_back(request);
		}
		return preloadStarts;
	}
	void cancelPreload() { cancelledPreloads++; }
	void preloadEnded() { endedPreloads++; }
	void dropQueuedSounds() { soundDrops++; }
	void adoptIncoming() { incomingRoster = built; }
	Generator& startIncoming(TransitionStyle style) {
		idle().startIncoming(pendingRecipe, 7, Takes{}, style);
		return idle();
	}
	void releaseIncoming() {
		retire({incomingRoster, false, 0});
		incomingRoster = 0;
	}
	void showQueueFull() { queueFullPopups++; }
	void rosterStarts(TransitionStyle /*style*/) {
		outgoingRoster = playingRoster;
		takeOver();
		current_ = &idle();
		// As the firmware's: a FILL or a KEYBOARD fader still held reaches the new instance (spec §8.7, §13.5).
		piece_.steering.instanceStarts(piece_.fillDown);
	}
	void retireOutgoing(uint32_t cap) {
		retire({outgoingRoster, true, cap});
		outgoingRoster = 0;
	}
	void commitStopped(uint32_t cap) {
		retire({playingRoster, true, cap});
		takeOver();
	}
	[[nodiscard]] uint32_t now() const { return static_cast<uint32_t>(piece_.hook.now()); }
	[[nodiscard]] uint32_t phraseLength() const { return phrase; }
	[[nodiscard]] bool outgoingFreed() const { return freeing == 0; }
	void phaseChanged(Phase phase) { events.push_back(std::string("phase=") + perform::phaseName(phase)); }
	void traced(Trace what, TransitionStyle style) {
		events.emplace_back(perform::traceText(what, style, phase()).text);
	}

private:
	[[nodiscard]] Generator& idle() { return current_ == &piece_.generator ? *other_ : piece_.generator; }
	/// As the firmware's commitRecipe(): the pending Recipe and its takes are the Current Recipe, with the macros the
	/// player has now.
	void takeOver() {
		playingRoster = incomingRoster;
		incomingRoster = 0;
		currentRecipe = playingRoster;
		const Macros live = piece_.currentRecipe.macros;
		piece_.currentRecipe = pendingRecipe;
		piece_.currentRecipe.macros = live;
		piece_.currentTakes = Takes{};
		piece_.steering.newRecipe();
	}
	void retire(const Retired& roster) {
		retired.push_back(roster);
		freeing++;
	}

	StepwisePiece<TransitionSink>& piece_;
	std::unique_ptr<Generator> other_ = std::make_unique<Generator>(); ///< The second block (spec §2.1).
	Generator* current_;
};

inline void TransitionSink::transitionStarts(int32_t startTick) {
	starts++;
	outgoingBase = base;
	base += startTick;
	startAt = base;
	adapter->started();
}
inline void TransitionSink::transitionLanded() {
	adapter->landed();
}
inline void TransitionSink::transitionDropped() {
	adapter->dropped();
}
inline void TransitionSink::outgoingDrained() {
	drained++;
	adapter->outgoingDrained();
}

constexpr int32_t kOutTempo = 12800;
constexpr int32_t kInTempo = 13400;
constexpr int32_t kInSwing = 12;

/// Techno plays from its first Groove, with a transition's lifecycle beside it: `lifecycle` is the firmware's
/// `transitions`. The pending Recipe differs in tempo, swing and key. The members below do what the Perform adapter
/// does around the lifecycle.
struct TransitionPiece : StepwisePiece<TransitionSink> {
	Generator* const out = &generator; ///< The instance that plays first.
	TransitionAdapter lifecycle;

	/// `incoming`: the genre the transition changes to; the playing one, a Regenerate, by default.
	explicit TransitionPiece(const Genre* genre = &genres::techno, const Genre* incoming = nullptr)
	    : StepwisePiece(startOf(genre)), lifecycle(*this, pendingOf(incoming != nullptr ? incoming : genre)) {
		steering.pending = [this] { return lifecycle.pending(); };
		driver.setTempo(kOutTempo, sink);
		sink.tempos.clear();
		lifecycle.phrase = static_cast<uint32_t>(songTicksFor(stepTick(genre->phraseSteps()), hook.magnitude));
	}

	/// A transition is confirmed and its preload ends at once. Playing, its command reaches the next decision.
	void confirm(TransitionStyle style, int32_t cutGlideTicks = kBarTicks) {
		lifecycle.glide = cutGlideTicks;
		lifecycle.request(style);
		lifecycle.load();
	}
	/// As confirm(), a tick before `step`'s line: the command reaches step + 1's decision.
	void confirmAt(int32_t step, TransitionStyle style, int32_t cutGlideTicks = kBarTicks) {
		runToStepLine(step, -1);
		confirm(style, cutGlideTicks);
	}
	/// The view's frame: the pump's call.
	void pump() { lifecycle.follow(); }
	/// The transport stops (PLAY, or a MIDI Stop): the piece stops, then the lifecycle hears of it.
	void stop() {
		if (driver.running()) {
			driver.stop(sink);
		}
		lifecycle.transport = false;
		lifecycle.playbackEnded();
	}
	/// After a call in which the piece ended by itself, the adapter stops the transport from inside the tick.
	/// @return whether it had ended.
	bool stopIfEnded() {
		if (!driver.ended() || !lifecycle.transport) {
			return false;
		}
		lifecycle.transport = false;
		lifecycle.playbackEnded();
		return true;
	}
	/// PLAY: a stop while playback runs; else a start, unless the lifecycle takes it to wait for a preload.
	void pressPlay(bool groove = false) {
		if (lifecycle.transport) {
			stop();
		}
		else if (!lifecycle.startWhenLoaded(groove)) {
			lifecycle.startPlayback(groove);
		}
	}

private:
	static Start startOf(const Genre* genre) {
		Start start{.seed = 3, .skipIntro = true};
		start.recipe.genre = genre;
		start.recipe.tempo = kOutTempo;
		return start;
	}
	static Recipe pendingOf(const Genre* genre) {
		Recipe pending = technoRecipe();
		pending.genre = genre;
		pending.tempo = kInTempo;
		pending.swing = kInSwing;
		pending.keyRoot = 2; // D
		return pending;
	}
};

} // namespace harness
