/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// A sound re-roll's lifecycle in Perform (spec §10.7, §12.9, §12.10, §13.4, §13.5): NEW SOUND, SHIFT for the previous
// sound, or a preset chosen in the preset browser preloads a part's new Output while the old one plays, then queues
// SetSound for the next phrase line, where the core's Sound event swaps it in. Portable, so the harness runs every
// press against the real core and Perform driver. Per part:
//   Idle:       nothing pending
//   Preloading: its preload runs. One runs at a time, across all parts.
//   Waiting:    the preload ended and SetSound, with the part's token, waits in the core for its line. Any number of
//               parts may wait at once.
// A waiting sound swaps in on its Sound event, or is dropped: pressed again, by a BACK tap in the core (the pump sees
// it gone), by a stop, or by a transition's confirm. Stopped, a preload swaps in as it ends.
//
// The adapter (SoundSwapAdapter below) is what needs the firmware (spec §2.5): the preload job and the runner, the
// Outputs (each part's incoming one, the roster, the retirements), the sound picks, the sound controls, popups and
// test events. It derives from SoundSwapLifecycle<Adapter>, so no call is virtual.
//
// Every member here is valid zero-filled: the firmware's sits in SDRAM's BSS.

#pragma once

#include "generative/core/event.h"
#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/perform/press_outcome.h"
#include "generative/perform/sound_retirements.h"
#include "generative/perform/text.h"
#include <concepts>
#include <cstdint>

namespace deluge::generative::perform {

/// What a sound action did.
enum class SoundPress : uint8_t {
	Preloading, ///< the preload started
	Cancelled,  ///< the part's sound was preloading or waiting for its line: pressed again, it's cancelled
	Busy,       ///< the runner is busy, or the sound share of the Outputs to free is full: refused (spec §12.10)
	NoPrevious, ///< no previous sound to go back to
	Ignored,    ///< nothing to act on (no Perform song, leaving, or the swap already decided on its line)
	Pending,    ///< a transition is pending: refused (spec §12.9, the caller shows the popup)
	QueueFull,  ///< the cancel of a sound waiting for its line had no room in the steering queue: it swaps anyway
};

/// A sound action's outcome as a press's, for its popup. NoPrevious has a popup of its own, which the caller shows.
[[nodiscard]] constexpr PressOutcome pressOutcome(SoundPress press) {
	switch (press) {
	case SoundPress::Preloading:
	case SoundPress::Cancelled:
		return PressOutcome::Accepted;
	case SoundPress::Busy:
		return PressOutcome::Busy;
	case SoundPress::Pending:
		return PressOutcome::ChangePending;
	case SoundPress::QueueFull:
		return PressOutcome::QueueFull;
	case SoundPress::NoPrevious:
	case SoundPress::Ignored:
		return PressOutcome::NothingToDo;
	}
	return PressOutcome::NothingToDo;
}

/// Where a preload's picks come from: rolled from the palette, the part's previous ones, or chosen in the preset
/// browser. A new way to pick a sound is a source, not a second swap path.
enum class SoundSource : uint8_t { Roll, Previous, Chosen };

/// A source as the test events name it (`source=roll`).
[[nodiscard]] constexpr const char* sourceName(SoundSource source) {
	switch (source) {
	case SoundSource::Roll:
		return "roll";
	case SoundSource::Previous:
		return "previous";
	case SoundSource::Chosen:
		return "chosen";
	}
	return "";
}

/// What the lifecycle did, for the test port's `gen_sound_*` events.
enum class SoundTrace : uint8_t {
	Press,                   ///< a preload started
	CancelPreload,           ///< pressed again while it preloads
	CancelTooLate,           ///< pressed again once its token landed: refused, it swaps on the line
	CancelQueueFull,         ///< pressed again while waiting, with no room for the cancel
	CancelWaiting,           ///< pressed again while waiting
	CancelDropped,           ///< the core dropped it (a BACK tap, a full pending list)
	CancelStopped,           ///< the transport stopped
	CancelRestarted,         ///< a restart reached its line (spec §13.2)
	CancelPreloadTransition, ///< a transition's confirm cancelled the preload running
	CancelTransition,        ///< a transition's confirm dropped it
	CancelTransitionDone,    ///< its preload ended Done after a transition's confirm, past its last boundary (#316)
	SwapStopped,             ///< stopped: it swapped in as its preload ended
	Swap,                    ///< its Sound event swapped it in
};

/// One trace, with what its event says.
struct SoundTraced {
	SoundTrace trace = SoundTrace::Press;
	int32_t part = 0;
	uint8_t token = kNoSound;
	SoundSource source = SoundSource::Roll; ///< Press
	bool line = false;                      ///< Swap: its tick is on a phrase line
	int32_t tick = 0;                       ///< Swap: the Sound event's
};

/// A `gen_sound_*` event's whole text.
struct SoundTraceText {
	char text[64];
};

/// A trace's text as its test event. Both adapters send this text, so the specs read what the test port sends.
[[nodiscard]] inline SoundTraceText traceText(const SoundTraced& traced) {
	SoundTraceText buffer;
	Text out(buffer.text);
	const auto cancel = [&](const char* why) { out.add("gen_sound_cancel part=").add(traced.part).add(why); };
	switch (traced.trace) {
	case SoundTrace::Press:
		out.add("gen_sound_press part=").add(traced.part).add(" source=").add(sourceName(traced.source));
		out.add(" token=").add(traced.token);
		break;
	case SoundTrace::CancelPreload:
		cancel(" preload");
		break;
	case SoundTrace::CancelTooLate:
		cancel(" too_late");
		break;
	case SoundTrace::CancelQueueFull:
		cancel(" queue_full");
		break;
	case SoundTrace::CancelWaiting:
		cancel(" waiting");
		break;
	case SoundTrace::CancelDropped:
		cancel(" dropped");
		break;
	case SoundTrace::CancelStopped:
		cancel(" stopped");
		break;
	case SoundTrace::CancelRestarted:
		cancel(" restarted");
		break;
	case SoundTrace::CancelPreloadTransition:
		out.add("gen_sound_cancel preload transition");
		break;
	case SoundTrace::CancelTransition:
		cancel(" transition");
		break;
	case SoundTrace::CancelTransitionDone:
		cancel(" transition_done");
		break;
	case SoundTrace::SwapStopped:
		out.add("gen_sound_swap part=").add(traced.part).add(" stopped token=").add(traced.token);
		break;
	case SoundTrace::Swap:
		out.add("gen_sound_swap part=").add(traced.part).add(" line=").add(traced.line ? 1 : 0);
		out.add(" token=").add(traced.token).add(" tick=").add(traced.tick);
		break;
	}
	return buffer;
}

/// What the lifecycle calls on its adapter: the firmware's (generative/firmware/sound_swaps.h) and the harness's
/// (tests/spec/generator/sound_swap_piece.h). Public members, so the concept can see them.
template <typename Adapter>
concept SoundSwapAdapter = requires(Adapter& adapter, const Adapter& reader, int32_t part, SoundSource source,
                                    uint8_t token, const SoundChoice* chosen, const SoundTraced& traced) {
	// Perform's side.
	/// The Current Recipe's genre: nullptr while Perform has none.
	{ reader.genre() } -> std::same_as<const Genre*>;
	/// Perform is entered and isn't being left.
	{ reader.ready() } -> std::same_as<bool>;
	/// A transition is pending (spec §12.9): sound actions are refused.
	{ reader.changePending() } -> std::same_as<bool>;
	/// The instance whose piece plays: nullptr while stopped.
	{ reader.playingGenerator() } -> std::same_as<const Generator*>;
	/// The steering queue, for SetSound.
	{ adapter.queue() } -> std::same_as<SteeringQueue&>;
	/// Test hook: a cancel press finds the steering queue full. False outside test builds.
	{ reader.queueHeldFull() } -> std::same_as<bool>;
	// The preload.
	/// The runner has a job running or waiting: a preload is refused.
	{ reader.runnerBusy() } -> std::same_as<bool>;
	/// The sound preload runs (its working animation is up).
	{ reader.preloadRunning() } -> std::same_as<bool>;
	/// Starts `part`'s preload from `source`, with `token`. A Chosen source's choice is `chosen`, kept for the job.
	/// It ends Done with preloaded(part, token, park), whose `park` hands over the Output it built, or ends Cancelled
	/// or Failed with no call here. @return false if the runner didn't take it.
	{ adapter.submitPreload(part, source, token, chosen) } -> std::same_as<bool>;
	/// The preload running is cancelled. It may still end Done, past its last boundary.
	{ adapter.cancelJob() } -> std::same_as<void>;
	// The Outputs.
	/// Outputs the sound share of the retirements may still take (spec §12.9).
	{ reader.retirementRoom() } -> std::same_as<int32_t>;
	/// In the tick, or as a preload ends while stopped: `part`'s incoming Output, the one its preload built, takes
	/// its place in the roster, the old one retires after its tail, and the new picks are the Current Recipe's and the
	/// old ones the part's previous. Frees, shows and allocates nothing.
	{ adapter.swapIn(part) } -> std::same_as<void>;
	/// In the tick, after a swap while playing: `part`'s sound controls bind to its new Output. Stopped, the next
	/// start binds every part.
	{ adapter.rebind(part) } -> std::same_as<void>;
	/// `part`'s incoming Output never plays: it is retired to be freed at once.
	{ adapter.dropIncoming(part) } -> std::same_as<void>;
	/// In the tick: `part`'s sound landed, but a confirm dropped it before its Sound event, so the part plays on with
	/// its old one. The core has logged the SetSound: the steering log says it never played
	/// (PerformDriver::logSoundDropped(), spec §17.2).
	{ adapter.swapDropped(part) } -> std::same_as<void>;
	/// The steering queue had no room for a preloaded sound's SetSound: the QUEUE FULL popup.
	{ adapter.showQueueFull() } -> std::same_as<void>;
	/// A test event (compiled out of release firmware).
	{ adapter.traced(traced) } -> std::same_as<void>;
};

/// The sound swaps, as the base of their adapter (CRTP, as TransitionLifecycle is).
template <typename Adapter>
class SoundSwapLifecycle {
public:
	/// The part's NEW SOUND, or with `previous` its previous sound (SHIFT, spec §13.5). Task context. The outcomes, in
	/// order: Ignored (not ready, or a part the genre hasn't), Pending, Cancelled for a part that preloads, then for
	/// one waiting Ignored once its token has landed (too late), QueueFull without room for the cancel, else Cancelled;
	/// then NoPrevious, Busy (the runner, or room to retire), Busy (the submit), Preloading.
	SoundPress press(int32_t part, bool previous) {
		const SoundPress gated = gate(part);
		if (gated != SoundPress::Preloading) {
			return gated;
		}
		// Pressed again while its sound preloads or waits for the line: that one is cancelled (spec §13.5).
		if (preloading(part)) {
			adapter().cancelJob();
			trace({.trace = SoundTrace::CancelPreload, .part = part});
			return SoundPress::Cancelled;
		}
		if (waiting(part)) {
			const Generator* piece = adapter().playingGenerator();
			if (piece != nullptr && piece->soundLanded(part) == waiting_[part].token) {
				// Too late: the line's step is decided, with the part's notes ended for the swap, which plays on the
				// line.
				trace({.trace = SoundTrace::CancelTooLate, .part = part});
				return SoundPress::Ignored;
			}
			// Without the cancel the core would still play the swap on its line: refused, and the sound stays waiting.
			if (adapter().queueHeldFull() || !adapter().queue().push(setSound(part, kNoSound))) {
				trace({.trace = SoundTrace::CancelQueueFull, .part = part});
				return SoundPress::QueueFull;
			}
			drop(part);
			trace({.trace = SoundTrace::CancelWaiting, .part = part});
			return SoundPress::Cancelled;
		}
		if (previous && !hasPrevious_[part]) {
			return SoundPress::NoPrevious;
		}
		return submit(part, previous ? SoundSource::Previous : SoundSource::Roll, nullptr);
	}
	/// A sound chosen in a Perform browser (spec §10.1, §13.5): a synth part's preset, or one lane's file on a Drums or
	/// FX part. It becomes the pick and the part's whole Output preloads as a re-roll's does, refused as canChoose()
	/// says. Task context.
	SoundPress choose(int32_t part, const SoundChoice& choice) {
		const SoundPress check = canChoose(part, choice);
		if (check != SoundPress::Preloading) {
			return check;
		}
		return submit(part, SoundSource::Chosen, &choice);
	}
	/// Whether choose(part, choice) would start preloading now: Preloading if it would, else what it would be refused
	/// with: Pending or Busy, or Ignored for a choice that doesn't fit the part (choiceFits(): an empty path, a preset
	/// for a Kit part, a lane it doesn't play) or a part whose sound is pending() (the browser opens only when it
	/// isn't). The browser closes on a choice only when it would.
	[[nodiscard]] SoundPress canChoose(int32_t part, const SoundChoice& choice) const {
		const SoundPress start = canStart(part);
		if (start == SoundPress::Preloading && !choiceFits(*adapter().genre(), part, choice)) {
			return SoundPress::Ignored;
		}
		return start;
	}
	/// canChoose() before there is a choice: the preset browser and the lane list open only when it is Preloading.
	/// The caller first lets a press cancel the part's pending() sound.
	[[nodiscard]] SoundPress canStart(int32_t part) const {
		const SoundPress gated = gate(part);
		if (gated != SoundPress::Preloading) {
			return gated;
		}
		return !pending(part) ? room() : SoundPress::Ignored;
	}

	/// Whether a preload that ends now is taken: Perform is entered with a genre and isn't being left. Being left, its
	/// Output goes with Perform's song.
	[[nodiscard]] bool takesPreloads() const { return adapter().ready() && adapter().genre() != nullptr; }
	/// A preload ended well, in the job's task. If takesPreloads(), `park()` first hands the adapter the Output the
	/// preload built as `part`'s incoming one, since the swap and the drop name only the part. While a transition is
	/// pending the Output is retired unplayed: the confirm's cancel came after the preload's last boundary, and sound
	/// actions wait for the new roster (spec §12.9). Else it swaps in at once while stopped, or is queued for the line.
	/// Perform being left, `park()` isn't called: the Output goes with Perform's song. @return whether the Output is
	/// kept: swapped in, or waiting for its line.
	template <std::invocable Park>
	bool preloaded(int32_t part, uint8_t token, Park park) {
		if (!takesPreloads()) {
			return false;
		}
		park();
		if (adapter().changePending()) {
			adapter().dropIncoming(part);
			trace({.trace = SoundTrace::CancelTransitionDone, .part = part});
			return false;
		}
		const Generator* piece = adapter().playingGenerator();
		if (piece == nullptr) {
			// Stopped: no line to wait for, so it swaps in at once, and the next start plays it.
			trace({.trace = SoundTrace::SwapStopped, .part = part, .token = token});
			adopt(part);
			return true;
		}
		// The swap waits for the next phrase line in the core (spec §13.4).
		if (!adapter().queue().push(setSound(part, token))) {
			adapter().dropIncoming(part);
			adapter().showQueueFull();
			return false;
		}
		waiting_[part] = {.token = token, .pushedAt = piece->nextStep()};
		return true;
	}
	/// In the tick: the core's Sound event. The part's waiting Output swaps in if the event carries its token.
	void swap(const Event& event) {
		const Genre* genre = adapter().genre();
		if (event.part >= kMaxParts || genre == nullptr) {
			return;
		}
		Waiting& in = waiting_[event.part];
		if (in.token == kNoSound || in.token != event.soundToken()) {
			adapter().swapDropped(event.part); // cancelled since it landed: the part plays on with its old sound
			return;
		}
		trace({.trace = SoundTrace::Swap,
		       .part = event.part,
		       .token = in.token,
		       .line = event.tick % (genre->phraseSteps() * kTicksPerStep) == 0,
		       .tick = event.tick});
		adopt(event.part);
		in = {};
		if (adapter().playingGenerator() != nullptr) {
			adapter().rebind(event.part);
		}
	}
	/// The pump's call: drops a waiting sound the core no longer has and never landed, which the core dropped (a BACK
	/// tap, a full pending list). The core has taken a SetSound by the decision of the step that was next when it was
	/// pushed, so it is looked for only after that.
	void service() {
		if (!adapter().ready()) {
			return;
		}
		const Generator* piece = adapter().playingGenerator();
		for (int32_t p = 0; p < kMaxParts && piece != nullptr; p++) {
			const Waiting& in = waiting_[p];
			if (in.token == kNoSound || piece->nextStep() <= in.pushedAt || piece->soundWaiting(p) == in.token
			    || piece->soundLanded(p) == in.token) {
				continue;
			}
			trace({.trace = SoundTrace::CancelDropped, .part = p});
			drop(p);
		}
	}
	/// The transport stopped: the core's pending list is gone, and any Sound event still buffered (spec §2.6), so
	/// every waiting sound is dropped.
	void playbackEnded() {
		forEachWaitingSound([this](int32_t part) {
			trace({.trace = SoundTrace::CancelStopped, .part = part});
			drop(part);
		});
	}
	/// A restart reached its line (spec §13.2): as at a stop, the core's pending list is gone, and any Sound event
	/// still buffered, so every waiting sound is dropped. A sound still preloading goes on, as after a stop: once
	/// loaded it waits for the new piece's next phrase line. In the tick: drop() frees nothing there.
	void pieceRestarts() {
		forEachWaitingSound([this](int32_t part) {
			trace({.trace = SoundTrace::CancelRestarted, .part = part});
			drop(part);
		});
	}
	/// A transition's confirm (spec §12.9): a sound still preloading is cancelled, and every sound waiting for its line
	/// is dropped, with a cancel pressed for each. The caller has checked that the steering queue has room for
	/// waitingSounds() presses.
	void cancelAll() {
		if (adapter().preloadRunning()) {
			adapter().cancelJob();
			trace({.trace = SoundTrace::CancelPreloadTransition});
		}
		forEachWaitingSound([this](int32_t part) {
			adapter().queue().push(setSound(part, kNoSound));
			drop(part);
			trace({.trace = SoundTrace::CancelTransition, .part = part});
		});
	}
	/// A transition's new roster starts clean: no part has a previous sound (spec §12.9).
	void forgetPrevious() {
		for (bool& previous : hasPrevious_) {
			previous = false;
		}
	}
	/// Perform's song is going, with every Output: nothing waits, and no part has a previous sound.
	void forget() {
		for (Waiting& in : waiting_) {
			in = {};
		}
		forgetPrevious();
	}

	[[nodiscard]] bool preloading(int32_t part) const { return adapter().preloadRunning() && preloadPart_ == part; }
	[[nodiscard]] bool waiting(int32_t part) const { return waiting_[part].token != kNoSound; }
	/// Whether `part`'s sound preloads or waits for its line: a press of its sound action cancels it (spec §10.7).
	[[nodiscard]] bool pending(int32_t part) const { return preloading(part) || waiting(part); }
	/// Parts whose sound waits for its line: cancelAll() presses a cancel for each.
	[[nodiscard]] int32_t waitingSounds() const {
		int32_t count = 0;
		forEachWaitingSound([&count](int32_t /*part*/) { count++; });
		return count;
	}
	[[nodiscard]] bool hasPrevious(int32_t part) const { return hasPrevious_[part]; }
	/// `part`'s Output was swapped in by a restart AS IT BEGAN (spec §13.2), not a re-roll: it has a previous sound
	/// from here, as after a swap.
	void swappedByRestart(int32_t part) { hasPrevious_[part] = true; }

private:
	/// A part's sound waiting for its line.
	struct Waiting {
		uint8_t token = kNoSound;
		int32_t pushedAt = 0; ///< The core's next step when SetSound was queued: taken once it has moved on.
	};

	[[nodiscard]] Adapter& adapter() {
		static_assert(SoundSwapAdapter<Adapter>, "the adapter must have SoundSwapAdapter's members");
		return static_cast<Adapter&>(*this);
	}
	[[nodiscard]] const Adapter& adapter() const { return static_cast<const Adapter&>(*this); }
	void trace(const SoundTraced& traced) { adapter().traced(traced); }

	[[nodiscard]] static Command setSound(int32_t part, uint8_t token) {
		return {CommandKind::SetSound, static_cast<uint8_t>(part), token};
	}
	/// `part`'s next token. Tokens run from 1 to kMaxSoundToken across all parts, and wrap. The core keeps the token
	/// that last landed on a part until the next start, so that one is skipped: a new token equal to it would read as
	/// landed, and a press couldn't cancel it, nor the pump see the core drop it.
	[[nodiscard]] uint8_t nextToken(int32_t part) {
		const auto after = [](uint8_t token) { return static_cast<uint8_t>(token >= kMaxSoundToken ? 1 : token + 1); };
		const Generator* piece = adapter().playingGenerator();
		lastToken_ = after(lastToken_);
		if (piece != nullptr && piece->soundLanded(part) == lastToken_) {
			lastToken_ = after(lastToken_);
		}
		return lastToken_;
	}
	/// The checks every press makes first: Preloading if it may go on, else Ignored or Pending.
	[[nodiscard]] SoundPress gate(int32_t part) const {
		const Genre* genre = adapter().genre();
		if (!adapter().ready() || genre == nullptr || part < 0 || part >= partCount(*genre)) {
			return SoundPress::Ignored;
		}
		if (refused(perform::gate(PressKind::Sound, adapter().changePending()))) {
			return SoundPress::Pending; // re-rolls and back to the previous sound wait for the new roster (spec §12.9)
		}
		return SoundPress::Preloading;
	}
	/// Whether the runner has room for a preload: Preloading if so, else Busy. Only one card load at a time: another
	/// job running, or one waiting, refuses it (spec §12.10). So does a full sound share of the Outputs to free
	/// (spec §12.9), which the free job empties once it can run.
	[[nodiscard]] SoundPress room() const {
		if (adapter().runnerBusy() || retirementsOwed() + 1 > adapter().retirementRoom()) {
			return SoundPress::Busy;
		}
		return SoundPress::Preloading;
	}
	/// How many re-rolls may still end in a retiring Output: one preloading and every part's waiting one.
	[[nodiscard]] int32_t retirementsOwed() const { return (adapter().preloadRunning() ? 1 : 0) + waitingSounds(); }
	/// Submits `part`'s preload from `source` if the runner has room (room()), else Busy.
	SoundPress submit(int32_t part, SoundSource source, const SoundChoice* chosen) {
		if (room() != SoundPress::Preloading) {
			return SoundPress::Busy;
		}
		const uint8_t token = nextToken(part);
		preloadPart_ = static_cast<int8_t>(part);
		if (!adapter().submitPreload(part, source, token, chosen)) {
			return SoundPress::Busy;
		}
		trace({.trace = SoundTrace::Press, .part = part, .token = token, .source = source});
		return SoundPress::Preloading;
	}
	/// Swaps `part`'s incoming Output in: it has a previous sound from here.
	void adopt(int32_t part) {
		adapter().swapIn(part);
		hasPrevious_[part] = true;
	}
	/// Cancels `part`'s sound waiting for its line: its Output is freed.
	void drop(int32_t part) {
		adapter().dropIncoming(part);
		waiting_[part] = {};
	}
	/// Calls `visit(part)` for each part whose sound waits for its line: what waitingSounds() counts is what
	/// cancelAll() drops.
	template <typename Visit>
	void forEachWaitingSound(Visit visit) const {
		for (int32_t p = 0; p < kMaxParts; p++) {
			if (waiting(p)) {
				visit(p);
			}
		}
	}

	Waiting waiting_[kMaxParts];
	bool hasPrevious_[kMaxParts] = {};
	int8_t preloadPart_ = 0; ///< The part whose preload was submitted last: the one running, if one runs.
	uint8_t lastToken_ = 0;
};

} // namespace deluge::generative::perform
