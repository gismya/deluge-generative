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

// The Perform driver's steering (spec §2.5, §2.6): every press and edit the player makes in Perform, and what the tick
// hands it as plain data. It owns the steering queue the driver drains, and keeps what the presses need across pieces
// and instances: the FILL latch, the KEYBOARD faders' holds, each part's takes made, and the Glide setting. Portable,
// so the harness presses the same entry points the Perform view does. Its adapter (the firmware's, or the harness's)
// hands it the Current Recipe and does what a press changes outside the core and the driver; the firmware's calls it
// from the tick and from entry, leave and the starts.

#pragma once

#include "generative/core/generator.h"
#include "generative/core/pitch.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/glide.h"
#include "generative/perform/keyboard_page.h"
#include "generative/perform/perform_driver.h"
#include "generative/perform/press_outcome.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace deluge::generative::perform {

/// How a key change acted, for the test port.
enum class KeySet : uint8_t { Stopped, Queued, Refused };

/// The test port's word for `how` (`gen_key_set … stopped`).
constexpr const char* keySetName(KeySet how) {
	switch (how) {
	case KeySet::Stopped:
		return "stopped";
	case KeySet::Queued:
		return "queued";
	case KeySet::Refused:
		break;
	}
	return "refused";
}

/// The steering, as the base of its adapter (`Adapter`, CRTP as TransitionLifecycle is): the firmware's or the
/// harness's. The adapter gives it, with no virtual call:
///
///   PerformDriver& driver() const;   NoteSink& sink() const;   the driver it steers, and the sink tempo goes to
///   Recipe& recipe() const;          Takes& takes() const;     the Current Recipe, handed in as plain data: a press
///                                                              while stopped changes it at once, and the next start
///                                                              plays it
///   bool changePending() const;      a transition is pending (spec §12.9): refusedWhilePending() presses are refused
///   void glideStarted();             a tempo or swing glide started while a piece plays: the tick is asked for on the
///                                    next swung tick, so it starts at the press (spec §12.7)
///   void swingIntervalChanged(int32_t interval);   the song's swing interval follows at once, as stock's does
///   void keyChanged(Key key);        stopped, a key change is the Recipe's at once: the song's key follows (§12.8)
///   void macroCentreMoved(Macro macro);            a MIDI-learned knob gets feedback
///   void takeSetStopped(int32_t part, int32_t take);   void keySet(Key key, KeySet how);
///   void fillPressed(int32_t value);               for the test port: what a press did, and the SetFill sent
///
/// Called from the presses, in task context, and fillPressed() from the tick once per FILL change.
///
/// The functions marked gnu::noinline stay out of line: the view and the menus call them from many places, and LTO
/// inlines them there at a cost of 384 bytes of the image, which sits in internal RAM.
template <typename Adapter>
class PerformSteering {
public:
	// ---- From the adapter: entry and leave, the starts, and the tick ----

	/// Perform's song is in (spec §14.3): presses act from here, on an empty queue and a new Recipe (newRecipe()).
	[[gnu::noinline]] void enter();
	/// LEAVE (spec §13.8): until the teardown starts, presses that need a piece do nothing. undoLeave() takes it back
	/// if the teardown can't start.
	void startLeaving() { leaving_ = true; }
	void undoLeave() { leaving_ = false; }
	/// Perform's song is gone: presses do nothing until the next enter().
	void forget() {
		entered_ = false;
		leaving_ = false;
	}
	[[nodiscard]] bool entered() const { return entered_; }
	[[nodiscard]] bool leaving() const { return leaving_; }

	/// The queue the driver drains at each step decision. The adapter's tick-side writers (homes, the external
	/// clock's tempo) and the transition lifecycle push into it too.
	[[nodiscard]] SteeringQueue& queue() { return queue_; }

	/// A piece starts from `macros`, the Recipe's (spec §13.2): every start is fresh, so the queue empties.
	void pieceStarts(const Macros& macros);
	/// A Generator instance plays from its step 0, at a start or a transition's start step (spec §12.9): what was sent
	/// to the last one and the new one has cleared is sent again. FILL still down (`fillDown`, the song's fill mode)
	/// is sent at the next sampleFill() with its SHIFT, and a KEYBOARD fader still held is pushed now (spec §13.5).
	void instanceStarts(bool fillDown);
	/// A new Recipe plays (a transition's, or a new entry's): every take at 0, none made yet (spec §6.6), and the
	/// menus' values read again.
	[[gnu::noinline]] void newRecipe();

	/// Each tick, before the driver advances: FILL's state (the song's fill mode) goes to the core as SetFill when it
	/// changed (spec §8.7, §2.6). A full queue tries again on the next tick.
	void sampleFill(bool down) {
		if (!down) {
			fillShift_ = false; // a SHIFT latched for a press the core never saw (FILL let go while stopped) goes too
		}
		if (down != (fillSent_ != kFillReleased)) {
			sendFill(down);
		}
	}
	/// Each tick, after the driver advanced: the Recipe keeps each part's take as it lands, so the next start, a save
	/// or a Compose has it (spec §6.6, §15).
	void recordTakes(const Generator& playing);

	// ---- The transport's presses (spec §13.2) ----

	/// FILL (SYNC-SCALING, or a FILL pad) went down: whether SHIFT was held, latched for the press the next tick sees,
	/// so it plays a fresh pick (spec §8.7). The press itself is the song's fill mode, which the caller sets.
	void latchFillShift(bool shift) { fillShift_ = shift; }
	/// End, armed by holding its pad: the Outro from the next phrase line, then the transport stops where it ends.
	/// Refused while a transition is pending (spec §12.9); nothing to do while stopped.
	PressOutcome armEnd();
	/// A tap on End: cancels it while it waits. Nothing to do when no End is armed or queued.
	PressOutcome cancelEnd();

	// ---- The sidebar and BACK (spec §13.4, §13.5) ----
	// Each acts while a piece plays and does nothing when stopped (mutes, solos, holds and forces are lost on stop).

	/// A MUTE pad: the part's mute toggles at the next bar line, or with `now` (SHIFT) the next undecided step.
	/// Pressed again while it waits, it cancels.
	PressOutcome pressMute(uint8_t part, bool now);
	/// X held + a MUTE pad: the part's solo, as pressMute().
	PressOutcome pressSolo(uint8_t part, bool now);
	/// The hold pad: the Arc's hold toggles from the next undecided step.
	PressOutcome pressHold();
	/// A force pad: `kind` from the next phrase line (a Build where it lets the Drop land on one). Again cancels.
	PressOutcome forceSection(SectionKind kind);
	/// BACK tap: cancels the newest queued action, if any.
	PressOutcome cancelNewest();

	// ---- The CLIP page (spec §6.6, §6.7, §13.5) ----

	/// A take pad or a Y turn: `take` for `part` from the next phrase line; the waiting take or the one playing
	/// pressed again cancels the one waiting. Stopped, the take is the Recipe's at once, and the next start plays it.
	PressOutcome pressTake(uint8_t part, int32_t take);
	/// FREEZE or CROSS-SCREEN: the part freezes, or thaws, on the next undecided step. Nothing while stopped.
	PressOutcome pressFreeze(uint8_t part);
	/// `part`'s takes as the CLIP page and the SEED + TAKES rows show them: playing, waiting and the newest made.
	[[nodiscard, gnu::noinline]] TakeRow takeRow(int32_t part) const;
	/// A Y turn or a SEED + TAKES turn: the take one back or forward (`direction`) from `row`'s waiting take, or else
	/// its playing one, pressed as a take pad is. `outcome` is what the press did.
	/// @return the take pressed, or -1 if none was: there's none that way, or the press was refused.
	int32_t stepTake(uint8_t part, const TakeRow& row, int32_t direction, PressOutcome* outcome);
	/// The take `part` plays: the playing piece's, or the Recipe's while stopped.
	[[nodiscard]] int32_t take(int32_t part) const;

	// ---- Tempo and swing (spec §12.7, §13.7) ----

	/// Sets the Recipe tempo (hundredths of a BPM), gliding to it at the Glide setting while playing, and logs it.
	/// Nothing to do under an external clock, which owns the tempo. Tempo and swing edits are refused while a
	/// transition is pending (spec §12.9).
	[[gnu::noinline]] PressOutcome setTempo(int32_t tempo);
	/// Sets the Recipe swing amount (-kMaxSwing to kMaxSwing), gliding to it on step boundaries while playing.
	[[gnu::noinline]] PressOutcome setSwing(int32_t amount);
	/// Sets the Recipe swing interval (a note value, 0 the 16th), at once as stock does.
	PressOutcome setSwingInterval(int32_t interval);
	/// The Perform Glide setting, kept until power-off (spec §13.3): the Perform menu's GLIDE sets it.
	[[nodiscard]] GlideLength glide() const { return glide_; }
	void setGlide(GlideLength glide) { glide_ = glide; }
	/// The Glide setting's length in Generator ticks for the Recipe's genre (spec §13.3).
	[[nodiscard]] int32_t glideLengthTicks() const { return glideTicks(glide_, phraseSteps()); }

	// ---- Macros (spec §13.5) ----

	/// Steers a macro: sets the Current Recipe's centre at once, and the core's from the next undecided step. While a
	/// KEYBOARD fader holds the macro, only the centre moves: the release springs back to it.
	void setMacro(Macro macro, int32_t units);
	/// A KEYBOARD fader held: the core plays `units` from the next undecided step, and from the start of any piece or
	/// instance that starts while it is held, while the Current Recipe keeps its centre.
	void holdMacro(Macro macro, int32_t units);
	/// A fader tap let go: `units` is the Current Recipe's centre, and the core's. The press steered the core already,
	/// unless a piece started during the tap.
	void keepMacroCentre(Macro macro, int32_t units);
	/// A fader hold let go: the core goes back to the Current Recipe's centre.
	void springMacroBack(Macro macro);

	// ---- Key (spec §7.6, §13.5) ----

	/// A key and scale change. While playing it queues to the next phrase line, or an Arc Drop if one lands first
	/// (spec §9.7), and the Current Recipe and the song key follow when it lands; the key that plays cancels a change
	/// waiting. Stopped, it is the Current Recipe's and the song's key at once, as a macro is, and the next start
	/// plays it. Refused while a transition is pending (spec §12.9); nothing to do for an invalid `key`.
	PressOutcome queueKeyChange(Key key);
	/// Whether a key change has reached the playing piece's core and waits for its line (the SCALE LED blinks). One
	/// still in the steering queue doesn't count until the next step decision drains it. Never while stopped: a stop
	/// drops it (spec §13.2).
	[[nodiscard]] bool keyChangePending() const;
	/// The key the piece goes on in, where the key and scale editor starts: a waiting change's, or else the key that
	/// plays; the Current Recipe's while stopped.
	[[nodiscard]] Key keyAhead() const;

	// ---- Queries for the view and the menus ----

	/// Steps in the Current Recipe's genre's phrase (8 bars before a genre is chosen).
	[[nodiscard, gnu::noinline]] int32_t phraseSteps() const;
	/// The roster's genre: the playing piece's, or the Current Recipe's while stopped.
	[[nodiscard, gnu::noinline]] const Genre* rosterGenre() const;
	/// Parts in the roster.
	[[nodiscard, gnu::noinline]] int32_t rosterParts() const;
	/// Pieces started, and Recipes a transition replaced, since power-on. With the driver's running() and the press
	/// counts, what a menu's held value was read under: a stop drops what was queued, a start or a new roster begins
	/// from the Recipe, and a press elsewhere (a Y turn, a BACK tap) moves what waits.
	[[nodiscard]] uint32_t pieces() const { return pieces_; }
	/// Key changes made, and BACK taps (which may cancel one), since power-on, from any control.
	[[nodiscard]] uint32_t keyPresses() const { return keyPresses_; }
	/// `part`'s take presses, and BACK taps (which may cancel one), since power-on, from any control.
	[[nodiscard]] uint32_t takePresses(int32_t part) const { return takePresses_[part]; }

private:
	[[nodiscard]] Adapter& adapter() { return static_cast<Adapter&>(*this); }
	[[nodiscard]] const Adapter& adapter() const { return static_cast<const Adapter&>(*this); }
	/// The piece playing, or nullptr when stopped.
	[[nodiscard]] const Generator* playing() const { return adapter().driver().playing(); }
	/// Whether a press of `kind` is refused with ChangePending: a transition is pending and §12.9 lists it (gate()).
	[[nodiscard]] bool gated(PressKind kind) const { return refused(gate(kind, adapter().changePending())); }
	/// Pushes a sidebar, BACK or End command while a piece plays: nothing to do while stopped or leaving.
	PressOutcome steer(const Command& command);
	/// Pushes a SetMacro while Perform is entered, and records it in macroSteering_.
	void pushMacro(Macro macro, int32_t units);
	/// `part` plays `take` now: the Current Recipe keeps it, and it counts among the takes made.
	void recordTake(int32_t part, int32_t take);
	/// The highest take `part` has played in this Recipe, the one playing included.
	[[nodiscard]] int32_t newestTake(int32_t part) const;
	/// sampleFill()'s push, when FILL went down or up.
	void sendFill(bool down);

	/// First: the firmware's adapter keeps the queue at offset 20, which is baked into Generator::decideNextStep()'s
	/// code (see PerformAdapter::steering_, and the implementation notes).
	SteeringQueue queue_;
	/// Perform's song is in, and LEAVE hasn't started its teardown.
	bool entered_ = false;
	bool leaving_ = false;
	/// The SetFill value last pushed in this instance: kFillReleased while FILL is up.
	int32_t fillSent_ = kFillReleased;
	/// SHIFT was held as FILL went down (latchFillShift()).
	bool fillShift_ = false;
	/// What the core's macros were steered with, and the KEYBOARD faders held: a start drops the queue.
	MacroSteering macroSteering_;
	uint32_t pieces_ = 0;
	uint32_t keyPresses_ = 0;
	uint32_t takePresses_[kMaxParts] = {};
	/// Each part's highest take played in this Recipe (the CLIP page's earlier takes, spec §13.5).
	uint8_t newestTake_[kMaxParts] = {};
	GlideLength glide_ = kDefaultGlide;
};

// ---- Definitions ----

// ---- Entry, the starts and the tick ----

template <typename Adapter>
void PerformSteering<Adapter>::enter() {
	static_assert(offsetof(PerformSteering, queue_) == 0, "the queue comes first: see PerformAdapter::steering_");
	entered_ = true;
	leaving_ = false;
	queue_.clear();
	macroSteering_ = MacroSteering{};
	newRecipe(); // a new Recipe: every take at 0 (spec §6.6)
}

template <typename Adapter>
void PerformSteering<Adapter>::pieceStarts(const Macros& macros) {
	pieces_++;
	queue_.clear();
	macroSteering_.startFrom(macros);
}

template <typename Adapter>
void PerformSteering<Adapter>::instanceStarts(bool fillDown) {
	// The Generator starts with no fill held: a FILL still down is sent again at the next tick, with its SHIFT (a
	// fresh pick sent to the last instance, or one latched and not yet sent). A SHIFT latched for a FILL already let go
	// goes.
	fillShift_ = fillDown && (fillShift_ || fillSent_ == kFillFreshPick);
	fillSent_ = kFillReleased;
	// A KEYBOARD fader still held is sent again, as FILL is, unless the core has it or will (spec §13.5).
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		if (const int32_t held = macroSteering_.resend(macro); held != MacroSteering::kNotHeld) {
			pushMacro(macro, held);
		}
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::newRecipe() {
	std::fill(std::begin(newestTake_), std::end(newestTake_), 0);
	pieces_++; // an open menu reads its key and takes again
}

template <typename Adapter>
void PerformSteering<Adapter>::sendFill(bool down) {
	// Fill state is the song's fill mode, so the MIDI-learnable FILL command works as the button does (spec §8.7). A
	// MIDI FILL has no SHIFT latched: it plays the held-fill entry.
	const int32_t value = !down ? kFillReleased : fillShift_ ? kFillFreshPick : kFillHeldEntry;
	if (!queue_.push({CommandKind::SetFill, 0, value})) {
		return; // a full queue: the next tick tries again
	}
	fillSent_ = value;
	fillShift_ = false;
	adapter().fillPressed(value);
}

template <typename Adapter>
void PerformSteering<Adapter>::recordTakes(const Generator& playing) {
	for (int32_t p = 0; p < kMaxParts; p++) {
		recordTake(p, playing.take(p));
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::recordTake(int32_t part, int32_t take) {
	const auto value = static_cast<uint8_t>(take);
	adapter().takes().take[part] = value;
	newestTake_[part] = std::max(newestTake_[part], value);
}

// ---- Presses ----

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::steer(const Command& command) {
	if (!entered_ || leaving_ || playing() == nullptr) {
		return PressOutcome::NothingToDo; // nothing plays: mutes, solos, holds and forces are lost on stop anyway
	}
	return queued(queue_.push(command));
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::armEnd() {
	if (gated(PressKind::EndArm)) {
		return PressOutcome::ChangePending;
	}
	return steer({CommandKind::End, 0, 1});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::cancelEnd() {
	const Generator* piece = playing();
	if (piece == nullptr || (!piece->endQueued() && !queue_.holds({CommandKind::End, 0, 1}))) {
		return PressOutcome::NothingToDo; // no End to cancel: a tap with a full queue isn't refused for nothing
	}
	return steer({CommandKind::End, 0, 0});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressMute(uint8_t part, bool now) {
	return steer({CommandKind::SetMute, part, now ? kPressNow : kPress});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressSolo(uint8_t part, bool now) {
	return steer({CommandKind::SetSolo, part, now ? kPressNow : kPress});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressHold() {
	return steer({CommandKind::SetHold, 0, kPress});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::forceSection(SectionKind kind) {
	return steer({CommandKind::ForceSection, static_cast<uint8_t>(kind), 0});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::cancelNewest() {
	// It may cancel a waiting key change or take: menus showing one read it again.
	keyPresses_++;
	for (uint32_t& presses : takePresses_) {
		presses++;
	}
	return steer({CommandKind::CancelNewest});
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressTake(uint8_t part, int32_t take) {
	if (gated(PressKind::Take)) {
		return PressOutcome::ChangePending;
	}
	const Genre* genre = adapter().recipe().genre;
	if (!entered_ || leaving_ || genre == nullptr || part >= partCount(*genre) || take < 0 || take >= kMaxTakes) {
		return PressOutcome::NothingToDo;
	}
	takePresses_[part]++;
	if (playing() != nullptr) {
		return queued(queue_.push({CommandKind::SetTake, part, take}));
	}
	// Stopped: no line to wait for. The take is the Recipe's, which the next start plays.
	recordTake(part, take);
	adapter().takeSetStopped(part, take);
	return PressOutcome::Accepted;
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressFreeze(uint8_t part) {
	return steer({CommandKind::SetFreeze, part, kPress});
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::stepTake(uint8_t part, const TakeRow& row, int32_t direction, PressOutcome* outcome) {
	// pressTake() refuses while a transition is pending before it looks at the take, and does nothing with none (-1).
	const int32_t take = steppedTake(row, direction);
	*outcome = pressTake(part, take);
	return refused(*outcome) ? -1 : take;
}

template <typename Adapter>
TakeRow PerformSteering<Adapter>::takeRow(int32_t part) const {
	TakeRow row;
	if (part < 0 || part >= rosterParts() || !entered_) {
		return row;
	}
	const Generator* piece = playing();
	row.playing = take(part);
	row.waiting = piece != nullptr ? piece->takeWaiting(part) : -1;
	row.newest = newestTake(part);
	return row;
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::take(int32_t part) const {
	const Generator* piece = playing();
	return piece != nullptr ? piece->take(part) : adapter().takes().take[part];
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::newestTake(int32_t part) const {
	return std::max<int32_t>(newestTake_[part], take(part));
}

// ---- Tempo and swing ----

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::setTempo(int32_t tempo) {
	if (gated(PressKind::Tempo)) {
		return PressOutcome::ChangePending;
	}
	if (!entered_ || adapter().sink().tempoFollowsMaster()) {
		return PressOutcome::NothingToDo;
	}
	adapter().recipe().tempo = tempo;
	const int32_t ticks = glideLengthTicks();
	adapter().driver().glideTempo(tempo, ticks, adapter().sink());
	queue_.push({CommandKind::SetTempo, 0, tempo, static_cast<uint16_t>(ticks)});
	if (playing() != nullptr) {
		adapter().glideStarted();
	}
	return PressOutcome::Accepted;
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::setSwing(int32_t amount) {
	if (gated(PressKind::Swing)) {
		return PressOutcome::ChangePending;
	}
	if (!entered_) {
		return PressOutcome::NothingToDo;
	}
	adapter().recipe().swing = static_cast<int8_t>(amount);
	const int32_t ticks = glideLengthTicks();
	adapter().driver().glideSwing(amount, ticks, adapter().sink());
	queue_.push({CommandKind::SetSwing, 0, amount, static_cast<uint16_t>(ticks)});
	if (playing() != nullptr) {
		adapter().glideStarted();
	}
	return PressOutcome::Accepted;
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::setSwingInterval(int32_t interval) {
	if (gated(PressKind::SwingInterval)) {
		return PressOutcome::ChangePending;
	}
	if (!entered_) {
		return PressOutcome::NothingToDo;
	}
	adapter().recipe().swingInterval = static_cast<uint8_t>(interval);
	adapter().swingIntervalChanged(interval);
	queue_.push({CommandKind::SetSwingInterval, 0, interval});
	return PressOutcome::Accepted;
}

// ---- Macros ----

template <typename Adapter>
void PerformSteering<Adapter>::setMacro(Macro macro, int32_t units) {
	adapter().recipe().macros.set(macro, units);
	if (macroSteering_.centreSteers(macro)) {
		pushMacro(macro, adapter().recipe().macros[macro]);
	}
	adapter().macroCentreMoved(macro);
}

template <typename Adapter>
void PerformSteering<Adapter>::holdMacro(Macro macro, int32_t units) {
	units = std::clamp<int32_t>(units, 0, kMacroFull);
	macroSteering_.hold(macro, units);
	pushMacro(macro, units);
}

template <typename Adapter>
void PerformSteering<Adapter>::keepMacroCentre(Macro macro, int32_t units) {
	macroSteering_.letGo(macro);
	adapter().recipe().macros.set(macro, units);
	adapter().macroCentreMoved(macro);
	// The press steered the core already, unless a piece started during the tap (a start clears the queue): steer it
	// again then.
	if (macroSteering_.differs(macro, adapter().recipe().macros[macro])) {
		pushMacro(macro, adapter().recipe().macros[macro]);
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::springMacroBack(Macro macro) {
	macroSteering_.letGo(macro);
	pushMacro(macro, adapter().recipe().macros[macro]);
}

template <typename Adapter>
void PerformSteering<Adapter>::pushMacro(Macro macro, int32_t units) {
	if (entered_ && queue_.push({CommandKind::SetMacro, static_cast<uint8_t>(macro), units})) {
		macroSteering_.sent(macro, units);
	}
}

// ---- Key ----

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::queueKeyChange(Key key) {
	if (gated(PressKind::Key)) {
		return PressOutcome::ChangePending;
	}
	if (!entered_ || !validKey(key)) {
		return PressOutcome::NothingToDo;
	}
	keyPresses_++;
	if (playing() == nullptr) {
		// Stopped: the Recipe's key at once, as a macro is, and the next start plays it (spec §13.5).
		adapter().recipe().keyRoot = key.root;
		adapter().recipe().scale = key.scale;
		adapter().keyChanged(key);
		adapter().keySet(key, KeySet::Stopped);
		return PressOutcome::Accepted;
	}
	const bool accepted = queue_.push({CommandKind::SetKey, key.root, static_cast<int32_t>(key.scale)});
	adapter().keySet(key, accepted ? KeySet::Queued : KeySet::Refused);
	return queued(accepted);
}

template <typename Adapter>
bool PerformSteering<Adapter>::keyChangePending() const {
	const Generator* piece = playing();
	return piece != nullptr && piece->keyChangePending();
}

template <typename Adapter>
Key PerformSteering<Adapter>::keyAhead() const {
	const Generator* piece = playing();
	return piece != nullptr ? piece->keyAhead() : Key{adapter().recipe().keyRoot, adapter().recipe().scale};
}

// ---- Queries ----

template <typename Adapter>
int32_t PerformSteering<Adapter>::phraseSteps() const {
	const Genre* genre = adapter().recipe().genre;
	return genre != nullptr ? genre->phraseSteps() : kStepsPerBar * 8;
}

template <typename Adapter>
const Genre* PerformSteering<Adapter>::rosterGenre() const {
	const Generator* piece = playing();
	return piece != nullptr ? piece->recipe().genre : adapter().recipe().genre;
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::rosterParts() const {
	const Genre* genre = rosterGenre();
	return genre != nullptr ? partCount(*genre) : 0;
}

} // namespace deluge::generative::perform
