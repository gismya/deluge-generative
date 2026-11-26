// A stepwise piece with a transition's lifecycle on it (spec §12.9): the real core, Perform driver,
// TransitionLifecycle, sound swaps (SoundSwapLifecycle) and the steering's handover (PerformSteering::handOver()), with
// adapters that keep plain records where the firmware has jobs, Outputs, sound picks and a palette. For
// transitions_spec, transition_lifecycle_spec and sound_swap_spec.
#pragma once

#include "generative/perform/transition_lifecycle.h"
#include "harness.h"
#include "sound_swap_piece.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
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
	HarnessSoundSwaps* sounds = nullptr;
	std::vector<Played> played;
	int32_t starts = 0;
	int32_t drained = 0;
	int32_t throwsCut = 0;    ///< outgoingThrowsCut() calls: a stop or jump ended the outgoing parts' fall.
	int32_t base = 0;         ///< Where the playing instance's tick 0 is on the timeline.
	int32_t outgoingBase = 0; ///< The outgoing one's, once it drains.
	int32_t startAt = -1;     ///< The start step's tick on the timeline.
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< {driver clock on the timeline, tempo}
	std::vector<std::pair<int32_t, int32_t>> swings;
	std::function<void(SteeringLog&, int32_t, uint8_t, uint8_t)> onLogSoundPaths;

	void attach(const perform::PerformDriver& playing, const Hook& /*hook*/) { driver = &playing; }
	/// As the firmware's: a Sound event swaps the part's waiting sound in, after its note-offs on the line.
	void play(const Event& event) override {
		played.push_back({event, starts > 0, false, event.tick + base});
		if (event.kind == EventKind::Sound && sounds != nullptr) {
			sounds->swap(event);
		}
	}
	void logSoundPaths(SteeringLog& log, int32_t step, uint8_t part, uint8_t token) override {
		if (onLogSoundPaths) {
			onLogSoundPaths(log, step, part, token);
		}
	}
	void playOutgoing(const Event& event) override {
		played.push_back({event, false, true, event.tick + outgoingBase});
	}
	void transitionStarts(int32_t startTick) override;
	void transitionLanded() override;
	void transitionDropped() override;
	void outgoingDrained() override;
	void outgoingThrowsCut() override { throwsCut++; }
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
	/// The firmware's Retirements::kTransitionRetirements: two rosters' Outputs.
	static constexpr int32_t kTransitionShare = 2 * kMaxParts;

	/// A roster handed to be freed.
	struct Retired {
		int32_t roster = 0;
		bool played = false; ///< It rings out first, `cap` at most. False: it never played, and goes at once.
		uint32_t cap = 0;
		bool operator==(const Retired&) const = default;
	};

	TransitionAdapter(StepwisePiece<TransitionSink>& piece, HarnessSoundSwaps& sounds, const Recipe& recipe)
	    : piece_(piece), sounds_(sounds), current_(&piece.generator) {
		pendingRecipe.recipe = recipe;
		pendingRecipe.seed = 7;
		piece.sink.adapter = this;
	}

	// ---- What a spec sets ----

	/// What the incoming instance starts with, and what the handover makes the Current Recipe. Its sound picks and
	/// stagedPalette are the firmware's: the harness leaves them alone. Its `macros` (Pending for a LOAD) is read.
	perform::PendingRecipe pendingRecipe;
	int32_t glide = kBarTicks;      ///< The Perform Glide setting, in Generator ticks.
	bool entered = true;            ///< Perform is entered with a genre.
	bool left = false;              ///< Perform is being left.
	bool transport = true;          ///< Playback runs.
	bool preloadStarts = true;      ///< Whether the runner takes a preload.
	bool preloadEndsAtOnce = false; ///< The runner takes it but its task can't start: it ends inside the submit.
	uint32_t phrase = 0;            ///< A phrase of the Current Recipe's genre, in song ticks: the unit of now().
	uint32_t pendingPhrase = 0; ///< The pending Recipe's genre's: `phrase` once that Recipe is Current. 0: the same.
	int32_t freeing = 0;        ///< Retired rosters not freed yet: freeRetired() frees them.
	/// Preload jobs in the runner: the one running and one waiting in the next slot, which a later request replaces.
	/// The newest ends last: its load() or failLoad() leaves the runner idle, an older one's leaves the newest.
	/// cancelPreload() empties the next slot.
	int32_t jobs = 0;
	/// A sound's preload or the free job runs, and the transition's preload waits behind it in the next slot. The
	/// firmware reserves a roster only for a TransitionJob that runs (Transitions::retirementRoom()): one waiting has
	/// built nothing. A spec sets it; a preload's end clears it, since the job ahead ended before that one ran.
	bool jobAhead = false;
	/// The newest preload rolled its pending Recipe, as the firmware's Transitions::rolled_: load() sets it, a submit
	/// clears it, and a spec sets it for a preload the leave cancels after its roll.
	bool rolled = false;

	// ---- What it records ----

	std::vector<std::string> events;         ///< Each `gen_transition` test event's text after that prefix.
	std::vector<uint32_t> preloads;          ///< The request of each preload started.
	std::vector<const Genre*> preloadGenres; ///< The genre each preload started rolls from.
	int32_t cancelledPreloads = 0;
	int32_t endedPreloads = 0; ///< Preloads that ended with no roster and none following.
	int32_t soundDrops = 0;    ///< Confirms that dropped the sound actions (SoundSwapLifecycle::cancelAll()).
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
		const uint32_t newest = newestPreload();
		rolled = request == 0 || request == newest;
		jobEnded(request);
		if (!loaded(request != 0 ? request : newest)) {
			discarded.push_back(built);
			freeing++; // as the firmware's preloaded(): it never played, and is retired to be freed
		}
		return built;
	}
	/// The preload for `request` (the newest by default) was cancelled or failed.
	void failLoad(uint32_t request = 0) {
		const uint32_t newest = newestPreload();
		jobEnded(request);
		loadEnded(request != 0 ? request : newest);
	}
	/// The free job has freed every retired roster.
	void freeRetired() { freeing = 0; }

	/// The instance the adapter plays, or would start: the incoming one from its start step.
	[[nodiscard]] Generator& current() { return *current_; }
	[[nodiscard]] bool has(const std::string& event) const {
		return std::find(events.begin(), events.end(), event) != events.end();
	}
	[[nodiscard]] SteeringQueue& queue() { return piece_.queue; }

	/// A request as the firmware's Transitions::change() makes one, with the genre the preload rolls from: the
	/// pending Recipe's unless given. The harness records it and loads nothing.
	Request request(TransitionStyle style, const Genre* genre = nullptr) {
		return TransitionLifecycle::request(style, genre != nullptr ? *genre : *pendingRecipe.recipe.genre);
	}

	// ---- The lifecycle's adapter ----

	[[nodiscard]] perform::PerformDriver& driver() { return piece_.driver; }
	[[nodiscard]] HarnessSteering& steering() { return piece_.steering; }
	[[nodiscard]] perform::NoteSink& sink() { return piece_.sink; }
	[[nodiscard]] bool ready() const { return entered && !left; }
	[[nodiscard]] bool leaving() const { return left; }
	[[nodiscard]] bool transportRunning() const { return transport; }
	/// PLAY from stop: as the firmware's tick, a piece starts from the Current Recipe on the instance that played last.
	void startPlayback(bool groove) {
		plays.push_back(groove);
		transport = true;
		piece_.playFromRecipe(groove, current_);
	}
	[[nodiscard]] int32_t cutGlideTicks() const { return glide; }
	bool submitPreload(uint32_t request, const Genre& genre) {
		if (preloadStarts) {
			rolled = false;
			preloads.push_back(request);
			preloadGenres.push_back(&genre);
			jobs = std::min(jobs + 1, 2);
		}
		if (preloadStarts && preloadEndsAtOnce) {
			jobs--;
			loadEnded(request); // as the firmware's runner, when startTask() fails: the job ends Failed at once
		}
		return preloadStarts;
	}
	void cancelPreload() {
		cancelledPreloads++;
		jobs =
		    std::min(jobs, 1); // the one running unwinds, and ends with failLoad() or, past its last boundary, load()
	}
	/// As the firmware's: leaving, a pending Recipe the preload rolled commits, with the handover alone.
	void preloadEnded() {
		endedPreloads++;
		if (left && rolled) {
			piece_.steering.handOver(pendingRecipe);
			adoptPending();
		}
	}
	[[nodiscard]] int32_t waitingSounds() const { return sounds_.waitingSounds(); }
	void dropWaitingSounds() {
		soundDrops++;
		sounds_.cancelAll();
	}
	void adoptIncoming() { incomingRoster = built; }
	Generator& startIncoming(TransitionStyle style) {
		idle().startIncoming(pendingRecipe.recipe, pendingRecipe.seed, pendingRecipe.takes, style);
		return idle();
	}
	[[nodiscard]] perform::HandoverMacros incomingMacros() const { return pendingRecipe.macros; }
	void releaseIncoming() {
		retire({incomingRoster, false, 0});
		incomingRoster = 0;
	}
	void showQueueFull() { queueFullPopups++; }
	void rosterStarts(TransitionStyle /*style*/) {
		outgoingRoster = playingRoster;
		current_ = &idle();
		incomingPlays();
		// As the firmware's PerformAdapter::incomingStarts(): the handover, then the instance starts as at a start.
		piece_.steering.handOver(pendingRecipe);
		adoptPending();
		piece_.instanceStarts();
	}
	void retireOutgoing(uint32_t cap) {
		retire({outgoingRoster, true, cap});
		outgoingRoster = 0;
	}
	void commitStopped(uint32_t cap) {
		retire({playingRoster, true, cap});
		incomingPlays();
		piece_.steering.takeOverStopped(pendingRecipe);
		adoptPending();
	}
	[[nodiscard]] uint32_t now() const { return static_cast<uint32_t>(piece_.hook.now()); }
	[[nodiscard]] uint32_t phraseLength() const { return phrase; }
	[[nodiscard]] bool outgoingFreed() const { return freeing == 0; }
	/// As the firmware's: the share less a roster for each retired one, and one for the preload running.
	[[nodiscard]] int32_t retirementRoom() const {
		return kTransitionShare - kMaxParts * (freeing + (jobs > 0 && !jobAhead ? 1 : 0));
	}
	void phaseChanged(Phase phase) { events.push_back(std::string("phase=") + perform::phaseName(phase)); }
	void traced(Trace what, TransitionStyle style) {
		events.emplace_back(perform::traceText(what, style, phase()).text);
	}

private:
	/// The newest preload's request. A spec that ends a preload when none has started fails here, not at random.
	[[nodiscard]] uint32_t newestPreload() const {
		if (preloads.empty()) {
			throw std::logic_error("load() or failLoad() with no preload started");
		}
		return preloads.back();
	}
	void jobEnded(uint32_t request) {
		jobs = request == 0 || request == newestPreload() ? 0 : std::max(jobs - 1, 0);
		jobAhead = false;
	}
	[[nodiscard]] Generator& idle() { return current_ == &piece_.generator ? *other_ : piece_.generator; }
	/// After each handover, as the firmware's Transitions::adoptPending(): the new roster has no previous sound.
	void adoptPending() { sounds_.forgetPrevious(); }
	/// The incoming roster plays, and its genre's phrase is the Current Recipe's.
	void incomingPlays() {
		playingRoster = incomingRoster;
		incomingRoster = 0;
		currentRecipe = playingRoster;
		phrase = pendingPhrase != 0 ? pendingPhrase : phrase;
	}
	void retire(const Retired& roster) {
		retired.push_back(roster);
		freeing++;
	}

	StepwisePiece<TransitionSink>& piece_;
	HarnessSoundSwaps& sounds_;
	std::unique_ptr<harness::Instance> other_ =
	    std::make_unique<harness::Instance>(); ///< The second block (spec §2.1).
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

/// Acid with `phraseBars`-bar phrases and `buildBars`-bar Builds. No shipped genre has another phrase length yet; M2's
/// do (House's 4-bar Builds, Dub's 1-bar ones), and genre changes cross them.
inline Genre acidWith(const char* name, uint8_t phraseBars, std::span<const WeightedBars> buildBars) {
	Genre genre = genres::technoAcid;
	genre.name = name;
	genre.phraseBars = phraseBars;
	genre.arc.buildBars = buildBars;
	return genre;
}
inline constexpr WeightedBars kFourBars[] = {{4, 1}};
inline constexpr WeightedBars kOneBar[] = {{1, 1}};
inline const Genre kFourBarPhrases = acidWith("acid 4", 4, kFourBars);
inline const Genre kOneBarPhrases = acidWith("acid 1", 1, kOneBar);

/// Whether a glide's write of `tempo`, after `last`, moves from `from` towards `to` without passing it. A repeat of
/// `last` moves nothing, so the caller skips it.
inline bool glidesTowards(int32_t last, int32_t tempo, int32_t from, int32_t to) {
	return to > from ? tempo > last && tempo <= to : tempo < last && tempo >= to;
}

/// The ticks a cut's first tempo write lands between (spec §13.3): its glide starts the Glide setting before the cut
/// line, or at the confirm if that is later, and the first write is where it has moved the tempo by one unit, under a
/// tick per unit of change after that.
inline std::pair<int32_t, int32_t> cutGlideFirstWrite(int32_t cutAt, int32_t glide, int32_t confirmTick, int32_t from,
                                                      int32_t to) {
	const int32_t starts = std::max(cutAt - glide, confirmTick);
	return {starts, starts + glide / std::abs(to - from) + 1};
}

constexpr int32_t kInTempo = 13400;
constexpr int32_t kInSwing = 12;

/// A piece plays from its first Groove, with a transition's lifecycle beside it: `lifecycle` is the firmware's
/// `transitions`. Built from genres, it plays Techno (or the genre given) at kOutTempo, and the pending Recipe is
/// Acid's with the incoming genre, kInTempo, kInSwing and D, so it differs in tempo, swing and key. Built from two
/// Recipes, each plays as given. The members below do what the Perform adapter does around the lifecycle.
struct TransitionPiece : StepwisePiece<TransitionSink> {
	Generator* const out = &generator; ///< The instance that plays first.
	/// The firmware's `soundSwaps`. A transition being pending refuses its presses.
	HarnessSoundSwaps sounds;
	TransitionAdapter lifecycle;

	/// `incoming`: the genre the transition changes to; the playing one, a Regenerate, by default.
	explicit TransitionPiece(const Genre* genre = &genres::technoAcid, const Genre* incoming = nullptr)
	    : StepwisePiece(startOf(genre)), sounds(driver, queue, currentRecipe),
	      lifecycle(*this, sounds, pendingOf(incoming != nullptr ? incoming : genre)) {
		wire(*genre, incoming);
	}
	/// A piece of `playing` from its first Groove at seed 3, whose transition changes to `pending`: each Recipe at its
	/// own genre, tempo, swing and key. For a genre change between two genres' own Recipes.
	TransitionPiece(const Recipe& playing, const Recipe& pending)
	    : StepwisePiece(Start{.recipe = playing, .seed = 3, .skipIntro = true}), sounds(driver, queue, currentRecipe),
	      lifecycle(*this, sounds, pending) {
		wire(*playing.genre, pending.genre != playing.genre ? pending.genre : nullptr);
	}

	/// A phrase of `genre` in now()'s unit, as the firmware's phraseLength() reads it.
	[[nodiscard]] uint32_t phraseOf(const Genre& genre) const {
		return static_cast<uint32_t>(songTicksFor(stepTick(genre.phraseSteps()), hook.magnitude));
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
	/// Whether the incoming roster's first Section is a Groove and its first phrase is in the history with `drop`: on
	/// a Drop (BUILD INTO A DROP, drop) or cut in plain (HARD CUT AT PHRASE, !drop).
	[[nodiscard]] bool startsInGroove(bool drop) const {
		ArcPhrase first;
		return sink.first(true, EventKind::Section).event.section == SectionKind::Groove
		       && driver.history().at(0, first) && first.drop == drop;
	}

	/// The playing core takes what is queued: one more step is decided.
	void decideOne() { decideBefore(driver.playing()->nextStep() + 1); }
	/// Fills the steering queue with presses that undo each other, leaving `room` places.
	void fillQueue(int32_t room = 0) {
		while (queue.room() > room && push({CommandKind::SetHold, 0, kPress})) {}
	}
	/// The view's frame: the pump's calls, in the firmware's order (Pump::service()).
	void pump() {
		sounds.service();
		lifecycle.follow();
	}
	/// The transport stops (PLAY, or a MIDI Stop): the piece stops, then the lifecycle hears of it.
	void stop() {
		if (driver.running()) {
			driver.stop(sink);
		}
		lifecycle.transport = false;
		sounds.playbackEnded(); // as the firmware's PerformAdapter::playbackEnded(), before the lifecycle's
		lifecycle.playbackEnded();
	}
	/// After a call in which the piece ended by itself, the adapter stops the transport from inside the tick.
	/// @return whether it had ended.
	bool stopIfEnded() {
		if (!driver.ended() || !lifecycle.transport) {
			return false;
		}
		lifecycle.transport = false;
		sounds.playbackEnded();
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
	/// What the Perform adapter wires around the lifecycle, and the tempo the piece plays at.
	void wire(const Genre& genre, const Genre* incoming) {
		steering.pending = [this] { return lifecycle.pending(); };
		sounds.held = [this] { return lifecycle.pending(); };
		sink.sounds = &sounds;
		driver.setTempo(currentRecipe.tempo, sink);
		driver.setSwing(currentRecipe.swing, sink);
		sink.tempos.clear();
		sink.swings.clear();
		lifecycle.phrase = phraseOf(genre);
		lifecycle.pendingPhrase = incoming != nullptr ? phraseOf(*incoming) : 0;
	}
	static Start startOf(const Genre* genre) {
		Start start{.seed = 3, .skipIntro = true};
		start.recipe.genre = genre;
		start.recipe.tempo = kOutTempo;
		return start;
	}
	static Recipe pendingOf(const Genre* genre) {
		Recipe pending = acidRecipe();
		pending.genre = genre;
		pending.tempo = kInTempo;
		pending.swing = kInSwing;
		pending.keyRoot = 2; // D
		return pending;
	}
};

} // namespace harness
