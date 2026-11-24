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

// Steering into the core (spec §2.6, ADR 0005). UI handlers push commands into a fixed queue; the core drains it at
// each step decision, applies each command where it lands and logs it there. The queue and the log live outside the
// Generator instance blocks, because a log spans pieces and transitions; the driver owns both.
//
// One producer (task context) and one consumer (the tick). The tick and the UI never pre-empt each other: the audio
// routine is a cooperative task that runs only where UI work yields, and nothing here yields. So plain fields are
// enough, and a coalescing push may rewrite a queued entry in place.

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

/// What a command does. Append new kinds at the end: logs store the value.
enum class CommandKind : uint8_t {
	SetMacro, ///< target: Macro. value: macro units. Continuous.
	SetKey,   ///< target: key root, 0 = C. value: Scale. Lands on the next phrase line or an Arc Drop (spec §9.7).
	SetTake,  ///< target: part. value: take, 0 to kMaxTakes - 1. Lands at the next phrase line (spec §6.6).
	/// target: SectionKind (Groove, Break or Build). Lands at the next phrase line; a Build at the bar line that lets
	/// it end on one (spec §9.6). value: the Build's length in steps, drawn by the core when it queues the force, so
	/// the log holds it. The same force again while it waits cancels it.
	ForceSection,
	/// value: 1 holds the Arc, 0 releases it, as logged. Lands on the next undecided step (spec §9.6). The pad sends
	/// kPress, which the core makes into the other state.
	SetHold,
	/// value: 1 arms End, 0 cancels it while it waits. End plays the genre's Outro from the next phrase line, and the
	/// piece ends where the Outro does (spec §13.2). It shares the force's pending slot: the later of the two wins.
	End,
	/// Driver-side steering (spec §2.6): the Perform driver acts on these at once, and they reach the core only to be
	/// logged where they land, on the next undecided step. The core keeps them in its Recipe and nothing else.
	SetTempo,         ///< value: the Recipe tempo, hundredths of a BPM. length: its glide. Continuous.
	SetSwing,         ///< value: the Recipe swing amount, -kMaxSwing to kMaxSwing. length: its glide. Continuous.
	SetSwingInterval, ///< value: the Recipe swing interval, a note value (0 the 16th). Continuous.
	/// target: part. value: 1 mutes, 0 unmutes, as logged. A pad press comes as kPress (the next bar line) or
	/// kPressNow (SHIFT: the next undecided step); the core resolves it into the part's other state, or cancels the
	/// press still waiting for its line (spec §13.4, §13.5).
	SetMute,
	/// target: part. value: 1 solos, 0 unsolos, as logged. Pressed like SetMute. While any part is soloed, only the
	/// soloed parts sound (spec §12.5).
	SetSolo,
	/// BACK tap: cancels the newest action waiting in the pending list, if any (spec §13.5). Never logged.
	CancelNewest,
	/// Driver-side steering (spec §11.6): a player edit moved a sound control's home. target: part. lane: the Drums or
	/// FX lane whose row it is on, or Lane::None. length: the param, keyed by the driver. value: the new home.
	/// Continuous, by part, lane and param.
	SetHome,
	/// target: part. value: 1 freezes, 0 unfreezes, as logged. Lands on the next undecided step (spec §6.7, §13.4).
	/// The CROSS-SCREEN press sends kPress, which the core makes into the other state.
	SetFreeze,
	/// target: part. value: the driver's token (1–255) for the sound it preloaded, or kNoSound to cancel the one
	/// waiting. Lands at the next phrase line: the part's notes end there and a Sound event tells the driver to swap
	/// the part's Output (spec §10.7, §13.4).
	SetSound,
	/// FILL (spec §8.7): value kFillHeldEntry or kFillFreshPick while FILL is held, kFillReleased when it is let go.
	/// Lands on the next undecided step. The driver pushes it when the song's fill mode changes, whether a panel
	/// button or the MIDI-learnable FILL command moved it. Logged with its value, so Capture knows a fresh pick.
	SetFill,
	/// A transition's start in the playing instance (spec §9.7, §12.9). target: TransitionStyle. value: the Build's
	/// length in steps, drawn by the core when it queues, or kCancelTransition to cancel the one waiting. Lands as a
	/// forced Build would (Drop), or on the next phrase line (Fade, Cut). From there the instance plays its Build or
	/// Outro up to the start step, where that ends, or stops on the cut line itself, and decides nothing from the start
	/// step on. It has a pending slot of its own, and once landed it can't be cancelled.
	Transition,
	/// target: part, a pitched one. value: its octave shift, -kMaxOctaveShift to kMaxOctaveShift. Lands at the next
	/// bar line, as a mute does; pressing the shift that plays cancels the one waiting (spec §7.3, §13.4).
	SetOctave,
	/// target: part. length: the Macro. value: the part's Part bias on that macro, in macro units, -kMacroFull to
	/// kMacroFull (spec §3.3). Continuous, by part and macro, as SetMacro is: it lands on the next undecided step.
	SetBias,
	/// X+PLAY or RESTART ▸ AS IT IS (spec §13.2): value 1 queues a restart, 0 cancels the one waiting. Lands at the
	/// next bar line, as a mute does: every note still sounding ends there, as on a Section line, and the instance
	/// decides nothing more, as at a hard cut's line. The driver then starts the piece again from bar 1 with its Intro
	/// (NoteSink::restartPiece()). It has a pending slot of its own, and once landed it can't be cancelled. Never
	/// logged: it ends the piece.
	Restart,
	/// Driver-side steering (spec §17.2): a player edit to a param no sound control moves, from a knob, a fader, the
	/// sound editor or MIDI. target: the part whose Output holds it, or kSongParams. lane: the Drums or FX lane whose
	/// row holds it, or Lane::None. length: the param, keyed by the driver. value: the param's value. Continuous, by
	/// part, lane and param. Capture makes it automation; the core keeps nothing.
	SetParam,
	/// Logged only, by the Perform driver (spec §17.2): the tempo glide a transition starts by itself, into the
	/// incoming Recipe's tempo across the Build, the Outro or a cut's Glide window, or back to the playing Recipe's on
	/// a cancel. value: the target, hundredths of a BPM. length: its glide, in Generator ticks from the entry's step.
	/// Never pushed; apply() keeps nothing, so the outgoing Recipe's tempo stays as it played.
	TransitionTempo,
	/// A tier 2 punch-in pad (spec §8.9, §13.5). target: PunchIn. lane: the Drums lane a LaneMute silences, else
	/// Lane::None. value: 1 on, 0 off, as the KEYBOARD page's hold or latch left it: the core resolves no press. Lands
	/// on the next undecided step.
	SetPunch,
	/// Driver-side steering, logged only (spec §17.2): stock's stutter starts (value 1) or ends (0), from stock
	/// Performance view's stutter column or a gold knob's push. Stutter isn't a param edit, so Capture needs it apart
	/// from the stutter rate's SetParam. The core keeps nothing. Lands on the next undecided step.
	SetStutter,
	/// Logged only, by the Perform driver (spec §17.2): a player edit to a non-param sound-editor setting (oscillator
	/// type, filter mode, arp mode, polyphony, synth mode, etc.). target: the part whose Output holds it, or
	/// kSongParams. lane: the Drums or FX lane whose row holds it, or Lane::None. length: OutputSetting (settingId()).
	/// value: the setting's value. Never pushed into SteeringQueue: written by NoteSink::logSettings() at the step
	/// being decided, so a sample/wavetable path can follow as a kSettingPathTag (0x85) record.
	SetSetting,
	/// A live variant set's switch (spec §3.6, §13.6): target: the set, by index in the genre's list. value: the
	/// variants it allows from then on, a bit each (Perform's VARIANTS ▸ ticks). Waits for the next Drop and lands
	/// there, with the Drop's own changes, where the instance re-applies its genre copy (landMask()). Only a live set
	/// takes one. The mask the set allows now cancels the one waiting.
	SetVariantMask,
};

/// The last kind: a kind appended after it moves this too. The log packs a kind in 6 bits (steering_log.h), and
/// steering_log_spec round-trips every kind up to here.
constexpr CommandKind kLastCommandKind = CommandKind::SetVariantMask;

/// A SetParam target: a song param, not a part's (song FX F1–F7 among them, spec §17.2).
constexpr uint8_t kSongParams = 0xFF;

/// The tier 2 punch-ins (spec §8.9): each plays a fixed amount while on. All but Pump act on the Drums part.
enum class PunchIn : uint8_t {
	LaneMute,   ///< One Drums lane plays nothing (Command::lane).
	HatRoll,    ///< The Closed hat plays 32nds.
	SnareRoll,  ///< The Snare, or the Clap where there's none, plays 16ths.
	Flam,       ///< Snare, Clap and Rim hits flam, or every hit but the Kick's in a kit with none (flams()).
	DoubleKick, ///< Each Kick hit plays twice, a 32nd apart.
	Pump,       ///< Every other part ducks under the Kick through its sidechain (the driver's, on Event::pump()).
};
constexpr int32_t kNumPunchIns = static_cast<int32_t>(PunchIn::Pump) + 1; ///< Pump is the last

/// How a transition plays (spec §9.7), chosen in the confirm.
enum class TransitionStyle : uint8_t {
	BuildIntoDrop, ///< BUILD INTO A DROP: the outgoing genre's Build, and the new roster from a Drop.
	Fade,          ///< FADE THROUGH: the outgoing genre's Outro, then the new roster's Intro, from the seam.
	Cut,           ///< HARD CUT AT PHRASE: the new roster in Groove from the next phrase line.
};
constexpr int32_t kNumTransitionStyles = 3;

/// A Transition value: cancels the transition waiting for its line.
constexpr int32_t kCancelTransition = -1;

/// What a pad press sends as a SetMute, SetSolo, SetHold or SetFreeze value, before the core resolves it into 0 or 1.
constexpr int32_t kPress = 2;    ///< at the action's line
constexpr int32_t kPressNow = 3; ///< SHIFT: on the next undecided step

/// What Generator::octaveWaiting() returns for a part with no octave shift waiting: SetOctave values run negative.
constexpr int32_t kNoOctaveWaiting = -128;

/// SetFill values.
constexpr int32_t kFillReleased = 0;
constexpr int32_t kFillHeldEntry = 1; ///< the genre's held-fill entry: a plain press
constexpr int32_t kFillFreshPick = 2; ///< a weighted draw from the genre's fill entries: SHIFT held as FILL went down

/// A SetSound value: cancels the part's sound waiting for its line. Tokens run from 1 to kMaxSoundToken.
constexpr int32_t kNoSound = 0;
constexpr int32_t kMaxSoundToken = 255;

/// Continuous values coalesce by key, so a control gets at most one value per step. Discrete ones never do.
constexpr bool isContinuous(CommandKind kind) {
	return kind == CommandKind::SetMacro || kind == CommandKind::SetTempo || kind == CommandKind::SetSwing
	       || kind == CommandKind::SetSwingInterval || kind == CommandKind::SetHome || kind == CommandKind::SetBias
	       || kind == CommandKind::SetParam;
}

/// Whether a command waits in the core's pending list for its line (spec §2.6), instead of landing on the next
/// undecided step.
constexpr bool isQuantised(CommandKind kind) {
	return kind == CommandKind::SetKey || kind == CommandKind::SetTake || kind == CommandKind::ForceSection
	       || kind == CommandKind::End || kind == CommandKind::SetMute || kind == CommandKind::SetSolo
	       || kind == CommandKind::SetSound || kind == CommandKind::Transition || kind == CommandKind::SetOctave
	       || kind == CommandKind::Restart || kind == CommandKind::SetVariantMask;
}

struct Command {
	CommandKind kind = CommandKind::SetMacro;
	uint8_t target = 0; ///< A part, a lane or a macro, by kind.
	int32_t value = 0;
	/// SetTempo and SetSwing: the glide's length in Generator ticks, so the log holds `{target, length}` (spec
	/// §17.2). 0 for at once, and for the external-clock tempo sample, which doesn't glide. SetHome: the param, as
	/// the driver keys it (read it as homeParam()); the core never reads it.
	uint16_t length = 0;
	Lane lane = Lane::None; ///< SetHome: the Drums or FX lane whose row holds the param, or Lane::None.

	/// SetHome and SetParam: the param, as the driver keys it on the part's Output.
	[[nodiscard]] constexpr uint16_t homeParam() const { return length; }
	/// SetSetting: the OutputSetting id on the part's Output or the song.
	[[nodiscard]] constexpr uint16_t settingId() const { return length; }
	/// SetBias: the macro whose Part bias it sets.
	[[nodiscard]] constexpr uint16_t biasMacro() const { return length; }

	[[nodiscard]] constexpr bool sameKey(const Command& other) const {
		const bool byParam = kind == CommandKind::SetHome || kind == CommandKind::SetParam;
		return kind == other.kind && target == other.target
		       && (!byParam || (lane == other.lane && homeParam() == other.homeParam()))
		       && (kind != CommandKind::SetBias || biasMacro() == other.biasMacro());
	}
	constexpr bool operator==(const Command&) const = default;
};
static_assert(sizeof(Command) == 12, "lane sits in padding: the queue, pending list and log keep their sizes");

class SteeringQueue {
public:
	static constexpr int32_t kCapacity = 64; ///< Spec §2.6, §20.

	/// Queues a command. A continuous one replaces a queued command with the same key, keeping its place.
	/// @return false if the queue is full and nothing was coalesced: a discrete command is refused (the caller shows
	/// the stock popup).
	bool push(const Command& command) {
		if (isContinuous(command.kind)) {
			for (int32_t i = 0; i < count_; i++) {
				Command& queued = entries_[(head_ + i) % kCapacity];
				if (queued.sameKey(command)) {
					queued = command; // the latest value, and its glide
					return true;
				}
			}
		}
		if (count_ == kCapacity) {
			return false;
		}
		entries_[(head_ + count_) % kCapacity] = command;
		count_++;
		return true;
	}

	/// Takes the oldest command.
	bool pop(Command* out) {
		if (count_ == 0) {
			return false;
		}
		*out = entries_[head_];
		head_ = (head_ + 1) % kCapacity;
		count_--;
		return true;
	}

	[[nodiscard]] int32_t size() const { return count_; }
	/// Whether `command` waits in the queue, as pushed.
	[[nodiscard]] bool holds(const Command& command) const {
		return holdsWhere([&](const Command& queued) { return queued == command; });
	}
	/// Whether a command of `kind` for `target` waits in the queue.
	[[nodiscard]] bool holdsFor(CommandKind kind, uint8_t target) const {
		return holdsWhere([&](const Command& queued) { return queued.kind == kind && queued.target == target; });
	}
	/// The newest command of `kind` for `target` waiting in the queue, or nullptr.
	[[nodiscard]] const Command* newest(CommandKind kind, uint8_t target) const {
		for (int32_t i = count_; i-- > 0;) {
			const Command& queued = entries_[(head_ + i) % kCapacity];
			if (queued.kind == kind && queued.target == target) {
				return &queued;
			}
		}
		return nullptr;
	}
	/// Drops each queued command `drop(command)` holds for, keeping the others in order.
	template <typename Drop>
	void removeIf(Drop drop) {
		int32_t kept = 0;
		for (int32_t i = 0; i < count_; i++) {
			const Command command = entries_[(head_ + i) % kCapacity];
			if (!drop(command)) {
				entries_[(head_ + kept++) % kCapacity] = command;
			}
		}
		count_ = kept;
	}
	/// Discrete commands that would still fit.
	[[nodiscard]] int32_t room() const { return kCapacity - count_; }
	void clear() { head_ = count_ = 0; }

private:
	/// Whether a waiting command matches `match`.
	template <typename Match>
	[[nodiscard]] bool holdsWhere(Match match) const {
		for (int32_t i = 0; i < count_; i++) {
			if (match(entries_[(head_ + i) % kCapacity])) {
				return true;
			}
		}
		return false;
	}

	Command entries_[kCapacity];
	int32_t head_ = 0;
	int32_t count_ = 0;
};

/// The core's pending list (spec §2.6): quantised commands waiting in the instance state for their line. One slot for
/// the key change, one per part's take, one for the force, one for a transition and one for a restart; a later command
/// replaces the one in its slot, so turning the key editor detent by detent queues only where it stops. A part's mute,
/// solo, sound and octave shift have a slot each, and so does a variant set's live switch.
class PendingList {
public:
	/// A key change, a force or End, a transition, a restart, every part's take, mute, solo, sound and octave shift,
	/// and every variant set's switch.
	static constexpr int32_t kCapacity = 42;

	/// Queues a command, replacing the one in its slot. Either way it is the newest. @return false if the list is
	/// full, and nothing changed.
	bool add(const Command& command) {
		int32_t i = indexOf(command);
		if (i >= 0) {
			remove(i);
		}
		else if (count_ == kCapacity) {
			return false;
		}
		entries_[count_++] = command;
		return true;
	}

	/// The waiting commands, oldest first: at(size() - 1) is the newest.
	[[nodiscard]] const Command& at(int32_t i) const { return entries_[i]; }
	/// Drops the newest waiting command (a BACK tap, spec §13.5), if any.
	void cancelNewest() {
		if (count_ > 0) {
			count_--;
		}
	}

	/// The waiting command in `command`'s slot, or nullptr.
	[[nodiscard]] const Command* waiting(const Command& command) const {
		int32_t i = indexOf(command);
		return i < 0 ? nullptr : &entries_[i];
	}
	[[nodiscard]] bool has(CommandKind kind) const {
		for (int32_t i = 0; i < count_; i++) {
			if (entries_[i].kind == kind) {
				return true;
			}
		}
		return false;
	}

	/// Drops the command in `command`'s slot, if any.
	void cancel(const Command& command) {
		land([&](const Command& waiting) { return sameSlot(waiting, command); });
	}

	/// Hands each waiting command to `lands(command)`, oldest first. One for which it returns true leaves the list.
	template <typename Lands>
	void land(Lands lands) {
		for (int32_t i = 0; i < count_;) {
			if (lands(entries_[i])) {
				remove(i);
			}
			else {
				i++;
			}
		}
	}

	[[nodiscard]] int32_t size() const { return count_; }
	void clear() { count_ = 0; }

private:
	/// A key change, a transition and a restart have one slot each whatever their target, and a force or End share one;
	/// a take has one per part.
	static constexpr bool sameSlot(const Command& a, const Command& b) {
		if (movesSection(a.kind) || movesSection(b.kind)) {
			return movesSection(a.kind) && movesSection(b.kind);
		}
		return a.kind == CommandKind::SetKey || a.kind == CommandKind::Transition || a.kind == CommandKind::Restart
		           ? a.kind == b.kind
		           : a.sameKey(b);
	}
	/// Whether a command moves the Arc to another Section at its line: a force, or End's Outro.
	static constexpr bool movesSection(CommandKind kind) {
		return kind == CommandKind::ForceSection || kind == CommandKind::End;
	}
	/// The index of the command in `command`'s slot, or -1.
	[[nodiscard]] int32_t indexOf(const Command& command) const {
		for (int32_t i = 0; i < count_; i++) {
			if (sameSlot(entries_[i], command)) {
				return i;
			}
		}
		return -1;
	}
	void remove(int32_t i) {
		for (int32_t j = i + 1; j < count_; j++) {
			entries_[j - 1] = entries_[j];
		}
		count_--;
	}

	Command entries_[kCapacity];
	int32_t count_ = 0;
};

/// The steering log (steering_log.h): the core appends each command where it lands.
class SteeringLog;
/// How a piece opens, as its log's header holds it (steering_log.h).
struct PieceStart;

} // namespace deluge::generative
