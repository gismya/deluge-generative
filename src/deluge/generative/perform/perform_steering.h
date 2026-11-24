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
#include "generative/core/variants.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/glide.h"
#include "generative/perform/keyboard_page.h"
#include "generative/perform/pending_recipe.h"
#include "generative/perform/perform_driver.h"
#include "generative/perform/press_outcome.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace deluge::generative::perform {

/// Where a restart starts the piece from (spec §13.2).
enum class RestartFrom : uint8_t {
	AsItIs,    ///< The Current Recipe as the player left it.
	AsItBegan, ///< The piece's start state, from its header in the steering log (spec §17.2).
};

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
///   uint32_t& seed() const;                                    while stopped changes it at once, and the next start
///                                                              plays it. A transition's handover replaces it
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

	// ---- A transition's handover (spec §12.9): the firmware's Transitions and the harness's adapter call these ----

	/// The pending Recipe becomes the Current Recipe: its Recipe, seed and takes, with the macros its `macros` names
	/// (a LOAD's file's macros also restart the macro steering, which then starts from them), then newRecipe(). Its
	/// sound picks are the adapter's to copy. At a transition's start step, in the tick, the adapter follows it with
	/// its instance start (instanceStarts()), so a FILL or KEYBOARD fader still held reaches the incoming instance; the
	/// song's tempo, swing and key change as that instance plays. Leaving Perform commits with this alone.
	void handOver(const PendingRecipe& pending);
	/// A stop commits a transition: handOver(), then the song jumps to the new Recipe's tempo and swing (the sink), and
	/// its swing interval and key (the adapter's swingIntervalChanged() and keyChanged()). Can run in the tick, when
	/// the piece ended by itself with the transition Queued: the transport has stopped by then, so the adapter's calls
	/// reschedule nothing.
	void takeOverStopped(const PendingRecipe& pending);

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
	/// Each tick, after the driver advanced: the Recipe keeps each part's take and octave shift as they land, so the
	/// next start, a save or a Compose has them (spec §6.6, §7.3, §15).
	void recordLanded(const Generator& playing);

	// ---- The transport's presses (spec §13.2) ----

	/// FILL (SYNC-SCALING, or a FILL pad) went down: whether SHIFT was held, latched for the press the next tick sees,
	/// so it plays a fresh pick (spec §8.7). The press itself is the song's fill mode, which the caller sets.
	void latchFillShift(bool shift) { fillShift_ = shift; }
	/// End, armed by holding its pad: the Outro from the next phrase line, then the transport stops where it ends.
	/// Refused while a transition is pending (spec §12.9); nothing to do while stopped.
	PressOutcome armEnd();
	/// A tap on End: cancels it while it waits. Nothing to do when no End is armed or queued.
	PressOutcome cancelEnd();

	// ---- Restart (spec §13.2) ----

	/// X+PLAY or RESTART ▸ AS IT IS while a piece plays: the piece starts again at the next bar line, from bar 1 with
	/// its Intro, from the Current Recipe as it is then; with `from` AsItBegan (RESTART ▸ AS IT BEGAN, its sounds
	/// loaded), from the piece's start state (restartOnLine()). Pressed again while it waits, it cancels; BACK does
	/// too. Refused while a transition is pending (spec §12.9). Nothing to do while stopped, where the adapter starts
	/// the piece as PLAY does.
	PressOutcome pressRestart(RestartFrom from = RestartFrom::AsItIs);
	/// AS IT BEGAN's sounds preload before its restart is pressed (the adapter's): a restart is ahead meanwhile.
	void restartPreloads(bool preloading) { restartPreloading_ = preloading; }
	/// Whether a restart is ahead: AS IT BEGAN's sounds preloading, pressed and not yet taken from the queue, waiting
	/// in the core's pending list, or landed and waiting for its line, where pieceStarts() empties the queue. A
	/// transition request is refused meanwhile (TransitionLifecycle::request()).
	[[nodiscard]] bool restartAhead() const { return restartPreloading_ || restartQueued(); }
	/// Where the restart ahead starts the piece from: what it was pressed with.
	[[nodiscard]] RestartFrom restartFrom() const { return restartFrom_; }
	/// A piece starts on `generator` from the Current Recipe, in Groove with `groove` (SHIFT+PLAY), and the steering
	/// with it (pieceStarts()): PLAY from stop, and a restart's line. The adapter follows it with its instance start
	/// (instanceStarts()), so a FILL or KEYBOARD fader still held plays from the new piece's start.
	void startFromCurrentRecipe(Generator& generator, bool groove);
	/// The restart's line, from the adapter's NoteSink::restartPiece(): what landed on the line (a take, an octave
	/// shift) goes into the Current Recipe first, as the tick's recordLanded() would after the call, then
	/// startFromCurrentRecipe() (AS IT IS). So a call that comes past the line plays what an on-time one does.
	void restartAsItIs(Generator& generator);
	/// The restart's line, from the adapter's NoteSink::restartPiece(): AS IT BEGAN from `began`, the header of the
	/// piece that played, when it was pressed so and the piece has one (else AS IT IS). The Current Recipe becomes the
	/// header's: its Recipe (macros, octave shifts, Part biases, key, tempo and swing among them), seed and takes, as
	/// the piece began. The adapter restores the sounds and the params (spec §13.2).
	void restartOnLine(Generator& generator, const PieceStart* began);
	/// AS IT BEGAN while stopped, before the start: the Current Recipe becomes `began`'s, as restartOnLine() makes
	/// it, and the song's swing interval and key follow (the adapter's swingIntervalChanged() and keyChanged()).
	void restoreStopped(const PieceStart& began);

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
	/// Parts with a take waiting for its line, in the playing core or still in the steering queue (pressed since the
	/// last step decision). dropWaitingTakes() takes a place in the queue for each.
	[[nodiscard]] int32_t waitingTakes() const;
	/// A transition's confirm (spec §12.9): each take waiting is dropped, as a press of the take that plays drops it.
	/// The caller has checked that the queue has room for waitingTakes() presses.
	void dropWaitingTakes();
	/// FREEZE or CROSS-SCREEN: the part freezes, or thaws, on the next undecided step. Nothing while stopped.
	PressOutcome pressFreeze(uint8_t part);
	/// A KEYBOARD punch-in pad turns `punch` on or off (spec §8.9, §13.5): the core plays it from the next undecided
	/// step. The steering keeps what is on, stopped too, and sends it again to each instance that starts
	/// (instanceStarts()). @return false if Perform isn't in, the command names no punch-in, or the queue was full: the
	/// pad stays as it was.
	bool setPunch(PunchIn punch, Lane lane, bool on);
	/// Stock's stutter started or ended (spec §17.2): logged where it lands, for Capture, and a stutter still on is
	/// logged again at each instance's step 0 (instanceStarts()). @return false only if the queue was full while a
	/// piece plays: the caller tries again. While stopped there is nothing to log.
	bool setStutter(bool on) {
		if (entered_ && !leaving_ && playing() != nullptr && !queue_.push({CommandKind::SetStutter, 0, on ? 1 : 0})) {
			return false;
		}
		stutterOn_ = on;
		return true;
	}
	/// The punch-ins on, as the pads last set them.
	[[nodiscard]] PunchIns punchIns() const { return {.lanes = punchLanes_, .effects = punchEffects_}; }
	/// `part`'s takes as the CLIP page and the SEED + TAKES rows show them: playing, waiting and the newest made.
	[[nodiscard, gnu::noinline]] TakeRow takeRow(int32_t part) const;
	/// A Y turn or a SEED + TAKES turn: the take one back or forward (`direction`) from `row`'s waiting take, or else
	/// its playing one, pressed as a take pad is. `outcome` is what the press did.
	/// @return the take pressed, or -1 if none was: there's none that way, or the press was refused.
	int32_t stepTake(uint8_t part, const TakeRow& row, int32_t direction, PressOutcome* outcome);
	/// The take `part` plays: the playing piece's, or the Recipe's while stopped.
	[[nodiscard]] int32_t take(int32_t part) const;

	// ---- Octave shifts (spec §7.3, §13.4, §13.6) ----

	/// OCTAVES ▸ or SHIFT + Y: `part`'s octave shift `shift` from the next bar line; the shift playing pressed again
	/// cancels the one waiting. Stopped, the shift is the Recipe's at once, and the next start plays it. Nothing to do
	/// for a part with no Register (Drums, FX), a part past the roster, or a shift past the part's octave range.
	[[gnu::noinline]] PressOutcome pressOctave(uint8_t part, int32_t shift);
	/// A SHIFT + Y turn or an OCTAVES ▸ turn: the shift one down or up (`direction`) from octaveAhead(), pressed as
	/// pressOctave() does. `outcome` is what the press did.
	/// @return the shift pressed, or kNoOctaveWaiting if none was: the part's octave range ends that way (the caller's
	/// LOWEST or HIGHEST popup, octaveAtEdge()), or nothing to do.
	[[gnu::noinline]] int32_t stepOctave(uint8_t part, int32_t direction, PressOutcome* outcome);
	/// `part`'s octave shift as it goes on: its newest press still in the steering queue, else the shift waiting in
	/// the core, else the one playing (the Recipe's while stopped). 0 for Drums and FX.
	[[nodiscard, gnu::noinline]] int32_t octaveAhead(int32_t part) const;
	/// The octave shift `part` plays: the playing piece's, or the Recipe's while stopped.
	[[nodiscard]] int32_t octave(int32_t part) const;
	/// Whether `part` of the roster has a Register, so it takes an octave shift.
	[[nodiscard]] bool takesOctave(int32_t part) const;
	/// The shifts `part` of the roster takes: those that keep every note of it a MIDI note (spec §7.3). Only 0 for a
	/// part that takes none.
	[[nodiscard]] OctaveRange octavesOf(int32_t part) const;
	/// Whether a turn `direction` from octaveAhead() would leave `part`'s octave range: the turn is refused, and the
	/// player is told the shift is the lowest or highest the part takes.
	[[nodiscard]] bool octaveAtEdge(int32_t part, int32_t direction) const;

	// ---- Live variant switches (spec §3.6, §13.6) ----

	/// VARIANTS ▸: live set `set` plays `variant` from the next Drop; the variant playing pressed again cancels the one
	/// waiting. Stopped, it is the Recipe's at once (joining the set's allowed variants), and the next start plays it.
	/// Refused while a transition is pending (spec §12.9). Nothing to do for a set that isn't live, one past the
	/// genre's, or a variant past the set's.
	[[gnu::noinline]] PressOutcome pressVariant(int32_t set, int32_t variant);
	/// Sets with a live switch waiting for the Drop, in the playing core or still in the steering queue.
	/// dropWaitingVariants() takes a place in the queue for each.
	[[nodiscard]] int32_t waitingVariants() const;
	/// A transition's confirm (spec §12.9): each switch waiting is dropped, as a press of the variant that plays drops
	/// it. The caller has checked that the queue has room for waitingVariants() presses.
	void dropWaitingVariants();
	/// Set `set`'s variant as it goes on: its newest switch still in the steering queue, else the one waiting in the
	/// core for the Drop, else the one playing. -1 past the roster genre's sets.
	[[nodiscard, gnu::noinline]] int32_t variantAhead(int32_t set) const;
	/// The variant set `set` plays now: the playing piece's (a Drop may have moved it), or the Recipe's while stopped.
	/// -1 past the roster genre's sets.
	[[nodiscard]] int32_t variantPlaying(int32_t set) const;

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

	// ---- Part bias (spec §3.3, §13.5) ----

	/// Steers `part`'s Part bias on `macro` (F8 with the part focused): the Current Recipe's at once, and the core's
	/// from the next undecided step, as a macro is. While a KEYBOARD fader holds that bias, only the Recipe's moves.
	/// Nothing for a part past the roster.
	void setBias(int32_t part, Macro macro, int32_t units);
	/// A KEYBOARD fader held with `part` focused: the core plays the bias `units` while it is held, across a piece or
	/// instance start, and the Current Recipe keeps its own.
	void holdBias(int32_t part, Macro macro, int32_t units);
	/// A fader tap let go: `units` is `part`'s bias in the Current Recipe, and the core's.
	void keepBias(int32_t part, Macro macro, int32_t units);
	/// A fader hold let go: the core goes back to the Current Recipe's bias.
	void springBiasBack(int32_t part, Macro macro);
	/// A KEYBOARD fader row pressed with `part` focused, or -1 for the Recipe level (spec §13.5): holds the macro at
	/// the row's `units`, or the part's bias that puts it there (faderBias()). keepFader() and springFaderBack() are
	/// its tap and hold let go, for the part the press held.
	void holdFader(int32_t part, Macro macro, int32_t units) {
		if (part < 0) {
			holdMacro(macro, units);
			return;
		}
		holdBias(part, macro, faderBias(adapter().recipe().macros[macro], units));
	}
	void keepFader(int32_t part, Macro macro, int32_t units) {
		if (part < 0) {
			keepMacroCentre(macro, units);
			return;
		}
		keepBias(part, macro, faderBias(adapter().recipe().macros[macro], units));
	}
	void springFaderBack(int32_t part, Macro macro) {
		if (part < 0) {
			springMacroBack(macro);
			return;
		}
		springBiasBack(part, macro);
	}
	/// RESET PART BIAS: `part`'s bias on every macro to 0, or every part's for -1.
	void resetBias(int32_t part);
	/// Where `part` plays `macro` at the Recipe's centre: the centre moved by its bias (partBiased()). The KEYBOARD
	/// fader's fill and F8's ring with the part focused.
	[[nodiscard]] int32_t biasedCentre(int32_t part, Macro macro) const {
		return partBiased(adapter().recipe().macros[macro], adapter().recipe().bias.of(part, macro));
	}
	/// The Part bias a KEYBOARD fader holds on `macro` (its popup), whichever part it holds.
	[[nodiscard]] int32_t heldBias(Macro macro) const { return macroSteering_.heldBias(macro); }
	/// Whether `part` is in the roster, so it takes a Part bias.
	[[nodiscard]] bool takesBias(int32_t part) const { return part >= 0 && part < rosterParts(); }

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
	/// Presses of the menus' per-row lists, octave shifts and variant switches, and BACK taps (which may cancel one),
	/// from any control, modulo 256.
	[[nodiscard]] uint32_t listPresses() const { return listPresses_; }

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
	/// Pushes a SetBias while Perform is entered. @return false if the queue was full.
	bool pushBias(int32_t part, Macro macro, int32_t units);
	/// pressTake() past the pending gate.
	PressOutcome pressTakeUngated(uint8_t part, int32_t take);
	/// Calls `visit(part, take)` for each part with a take waiting, in the playing piece or in the queue, with the
	/// take it plays: what waitingTakes() counts is what dropWaitingTakes() drops. A part an earlier confirm already
	/// dropped, in this step's window, is visited again: its drop's SetTake waits in the queue and takeWaiting() stays
	/// set until the queue drains. So each further confirm in the same step window pushes another SetTake for such a
	/// part, the same take again, and its room check asks for one place more per part already dropped. Harmless: each
	/// is the take the part plays already.
	template <typename Visit>
	void forEachWaitingTake(Visit visit) const {
		const Generator* piece = playing();
		for (int32_t p = 0; piece != nullptr && p < kMaxParts; p++) {
			const auto part = static_cast<uint8_t>(p);
			if (piece->takeWaiting(p) >= 0 || queue_.holdsFor(CommandKind::SetTake, part)) {
				visit(part, piece->take(p));
			}
		}
	}
	/// Calls `visit(set, variant)` for each set with a live switch waiting, in the playing piece or in the queue, with
	/// the variant it plays: what waitingVariants() counts is what dropWaitingVariants() drops.
	template <typename Visit>
	void forEachWaitingVariant(Visit visit) const {
		const Generator* piece = playing();
		for (int32_t s = 0; piece != nullptr && s < kMaxVariantSets; s++) {
			const auto set = static_cast<uint8_t>(s);
			if (piece->variantWaiting(s) >= 0 || queue_.holdsFor(CommandKind::SetVariant, set)) {
				visit(set, piece->variants().pick[s]);
			}
		}
	}
	/// `part` plays `take` now: the Current Recipe keeps it, and it counts among the takes made.
	void recordTake(int32_t part, int32_t take);
	/// The highest take `part` has played in this Recipe, the one playing included.
	[[nodiscard]] int32_t newestTake(int32_t part) const;
	/// sampleFill()'s push, when FILL went down or up.
	void sendFill(bool down);
	/// A restart pressed and not yet taken from the queue, waiting in the core, or landed (restartAhead()).
	[[nodiscard]] bool restartQueued() const;
	/// The Current Recipe becomes `began`'s (AS IT BEGAN), and the menus read their values again.
	void beginAgainFrom(const PieceStart& began);

	/// First: the firmware's adapter keeps the queue at offset 20, which is baked into Generator::decideNextStep()'s
	/// code (see PerformAdapter::steering_, and implementation-notes/core.md).
	SteeringQueue queue_;
	/// Perform's song is in, and LEAVE hasn't started its teardown.
	bool entered_ = false;
	bool leaving_ = false;
	/// Stock's stutter is on (setStutter()), in what was padding: see punchEffects_.
	bool stutterOn_ = false;
	/// The restart ahead's (pressRestart()), and AS IT BEGAN's sounds preloading (restartPreloads()): one byte between
	/// them, the last of the padding before fillSent_ (the steering's size is pinned, see punchEffects_).
	RestartFrom restartFrom_ : 1 = RestartFrom::AsItIs;
	bool restartPreloading_ : 1 = false;
	/// The SetFill value last pushed in this instance: kFillReleased while FILL is up.
	int32_t fillSent_ = kFillReleased;
	/// SHIFT was held as FILL went down (latchFillShift()).
	bool fillShift_ = false;
	/// The punch-ins on (PunchIns' fields), in what was padding: the steering's size moves PerformAdapter::sink_,
	/// which the start step's modulo-8 `static_assert` pins (implementation-notes/perform-ui.md).
	uint8_t punchEffects_ = 0;
	uint16_t punchLanes_ = 0;
	/// What the core's macros were steered with, and the KEYBOARD faders held: a start drops the queue.
	MacroSteering macroSteering_;
	uint32_t pieces_ = 0;
	uint32_t keyPresses_ = 0;
	uint32_t takePresses_[kMaxParts] = {};
	/// Each part's highest take played in this Recipe (the CLIP page's earlier takes, spec §13.5).
	uint8_t newestTake_[kMaxParts] = {};
	GlideLength glide_ = kDefaultGlide;
	/// Wraps at 256: readers compare it frame to frame. A byte keeps the steering's size, which the firmware's start
	/// step pins (PerformAdapter::incomingStarts()).
	uint8_t listPresses_ = 0;
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
	punchEffects_ = 0; // an entry starts with no punch-in on
	punchLanes_ = 0;
	stutterOn_ = false;
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
		// A fader still holding a part's bias too: the new instance starts from the Recipe's or carries the last
		// one's, either way not the held one.
		if (const int32_t part = macroSteering_.biasHeldOn(macro); part != MacroSteering::kNoPart) {
			pushBias(part, macro, macroSteering_.heldBias(macro));
		}
	}
	// Each punch-in still on, as FILL is (spec §8.9). The queue has just emptied, so it takes them all.
	for (int32_t p = 0; p < kNumPunchIns; p++) {
		const auto punch = static_cast<PunchIn>(p);
		if (punch != PunchIn::LaneMute && punchIns().on(punch)) {
			queue_.push({CommandKind::SetPunch, static_cast<uint8_t>(p), 1});
		}
	}
	for (int32_t l = 0; l < kNumDrumLanes; l++) {
		if (punchIns().muted(static_cast<Lane>(l))) {
			Command mute{CommandKind::SetPunch, static_cast<uint8_t>(PunchIn::LaneMute), 1};
			mute.lane = static_cast<Lane>(l);
			queue_.push(mute);
		}
	}
	// A stutter still on is logged again in the new instance; one let go since the last one's final decision isn't,
	// so the new piece's log has no stutter (spec §17.2).
	if (stutterOn_) {
		queue_.push({CommandKind::SetStutter, 0, 1});
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::newRecipe() {
	std::fill(std::begin(newestTake_), std::end(newestTake_), 0);
	pieces_++; // an open menu reads its key and takes again
}

template <typename Adapter>
void PerformSteering<Adapter>::handOver(const PendingRecipe& pending) {
	Recipe& recipe = adapter().recipe();
	const Macros live = recipe.macros;
	const PartBiases liveBias = recipe.bias;
	// A Regenerate keeps each part's bias: the same roster. A genre change starts at the new Recipe's (0), a LOAD at
	// its file's, and a fader still holding a bias lets go of it (spec §3.3).
	const bool keepsBias = keepsPartBias(pending.macros, pending.recipe.genre, recipe.genre);
	recipe = pending.recipe;
	if (keepsBias) {
		recipe.bias = liveBias;
	}
	else {
		for (int32_t m = 0; m < kNumMacros; m++) {
			macroSteering_.letGoBias(static_cast<Macro>(m));
		}
	}
	if (pending.macros == HandoverMacros::Live) {
		recipe.macros = live;
	}
	else {
		macroSteering_.startFrom(recipe.macros); // the incoming instance plays the file's (spec §12.9)
	}
	adapter().seed() = pending.seed;
	adapter().takes() = pending.takes;
	// An octave shift or a variant switch still queued names the outgoing roster's part or set: it goes with it, so
	// the incoming instance never lands it on its own (spec §12.9).
	queue_.removeIf([](const Command& command) {
		return command.kind == CommandKind::SetOctave || command.kind == CommandKind::SetVariant;
	});
	newRecipe();
}

template <typename Adapter>
void PerformSteering<Adapter>::takeOverStopped(const PendingRecipe& pending) {
	handOver(pending);
	const Recipe& recipe = adapter().recipe();
	adapter().sink().setTempo(recipe.tempo);
	adapter().sink().setSwing(recipe.swing);
	adapter().swingIntervalChanged(recipe.swingInterval);
	adapter().keyChanged({recipe.keyRoot, recipe.scale});
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
void PerformSteering<Adapter>::recordLanded(const Generator& playing) {
	for (int32_t p = 0; p < kMaxParts; p++) {
		recordTake(p, playing.take(p));
		adapter().recipe().octave[p] = static_cast<int8_t>(playing.octave(p));
	}
	// The variants playing now, as Drops and live switches moved them: SAVE stores them (spec §3.6).
	if (playing.recipe().genre == adapter().recipe().genre) {
		adapter().recipe().variants = playing.variants();
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
PressOutcome PerformSteering<Adapter>::pressRestart(RestartFrom from) {
	if (gated(PressKind::Restart)) {
		return PressOutcome::ChangePending;
	}
	if (const Generator* piece = playing(); piece != nullptr && piece->restartLanded()) {
		return PressOutcome::NothingToDo; // landed: it can't be cancelled, and the line empties the queue
	}
	const bool cancels = restartQueued();
	const PressOutcome outcome = steer({CommandKind::Restart, 0, cancels ? 0 : 1});
	if (!cancels && outcome == PressOutcome::Accepted) {
		restartFrom_ = from;
	}
	return outcome;
}

template <typename Adapter>
bool PerformSteering<Adapter>::restartQueued() const {
	if (const Command* pressed = queue_.newest(CommandKind::Restart, 0)) {
		return pressed->value != 0;
	}
	const Generator* piece = playing();
	return piece != nullptr && (piece->restartWaiting() || piece->restartLanded());
}

template <typename Adapter>
void PerformSteering<Adapter>::startFromCurrentRecipe(Generator& generator, bool groove) {
	generator.start(adapter().recipe(), adapter().seed(), adapter().takes(), groove);
	pieceStarts(generator.recipe().macros);
}

template <typename Adapter>
void PerformSteering<Adapter>::restartAsItIs(Generator& generator) {
	recordLanded(generator);
	startFromCurrentRecipe(generator, false);
}

template <typename Adapter>
void PerformSteering<Adapter>::restartOnLine(Generator& generator, const PieceStart* began) {
	const bool asItBegan = restartFrom_ == RestartFrom::AsItBegan && began != nullptr;
	restartFrom_ = RestartFrom::AsItIs;
	if (!asItBegan) {
		restartAsItIs(generator);
		return;
	}
	// Nothing that landed in the first run goes into the Current Recipe: the header's replaces it. The song's key
	// follows the new piece's step 0 KeyScale event; its swing interval is the adapter's, outside the tick.
	beginAgainFrom(*began);
	startFromCurrentRecipe(generator, false);
}

template <typename Adapter>
void PerformSteering<Adapter>::restoreStopped(const PieceStart& began) {
	beginAgainFrom(began);
	const Recipe& recipe = adapter().recipe();
	adapter().swingIntervalChanged(recipe.swingInterval);
	adapter().keyChanged({recipe.keyRoot, recipe.scale});
}

template <typename Adapter>
void PerformSteering<Adapter>::beginAgainFrom(const PieceStart& began) {
	// The same Recipe: its takes made stay (an earlier take can be played again), and so does every Part bias fader
	// held, which instanceStarts() sends again as AS IT IS does.
	adapter().recipe() = began.recipe;
	adapter().seed() = began.seed;
	adapter().takes() = began.takes;
	pieces_++; // an open menu reads its key, takes and octaves again
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::cancelNewest() {
	// It may cancel a waiting key change, take, octave shift or variant switch: menus showing one read it again.
	keyPresses_++;
	listPresses_++;
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
	return pressTakeUngated(part, take);
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressTakeUngated(uint8_t part, int32_t take) {
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
int32_t PerformSteering<Adapter>::waitingTakes() const {
	int32_t count = 0;
	forEachWaitingTake([&count](uint8_t /*part*/, int32_t /*take*/) { count++; });
	return count;
}

template <typename Adapter>
void PerformSteering<Adapter>::dropWaitingTakes() {
	// Each press of the take playing drains after the take it drops, at the same decision: the core cancels the one
	// waiting (spec §13.5).
	forEachWaitingTake([this](uint8_t part, int32_t take) { pressTakeUngated(part, take); });
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressFreeze(uint8_t part) {
	return steer({CommandKind::SetFreeze, part, kPress});
}

template <typename Adapter>
bool PerformSteering<Adapter>::setPunch(PunchIn punch, Lane lane, bool on) {
	Command command{CommandKind::SetPunch, static_cast<uint8_t>(punch), on ? 1 : 0};
	command.lane = lane;
	PunchIns next = punchIns();
	if (!entered_ || !next.set(command)) {
		return false;
	}
	// While stopped or leaving, the next start sends it (instanceStarts()).
	if (!leaving_ && playing() != nullptr && !queue_.push(command)) {
		return false;
	}
	punchEffects_ = next.effects;
	punchLanes_ = next.lanes;
	return true;
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

// ---- Octave shifts ----

template <typename Adapter>
bool PerformSteering<Adapter>::takesOctave(int32_t part) const {
	const Genre* genre = rosterGenre();
	return entered_ && genre != nullptr && part >= 0 && part < partCount(*genre)
	       && hasRegister(genre->parts[part].role);
}

template <typename Adapter>
OctaveRange PerformSteering<Adapter>::octavesOf(int32_t part) const {
	return takesOctave(part) ? octaveRange(rosterGenre()->parts[part]) : OctaveRange{};
}

template <typename Adapter>
bool PerformSteering<Adapter>::octaveAtEdge(int32_t part, int32_t direction) const {
	return !leaving_ && direction != 0 && takesOctave(part)
	       && !octavesOf(part).contains(octaveAhead(part) + (direction > 0 ? 1 : -1));
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressOctave(uint8_t part, int32_t shift) {
	if (leaving_ || !takesOctave(part) || !octavesOf(part).contains(shift)) {
		return PressOutcome::NothingToDo;
	}
	listPresses_++;
	if (playing() != nullptr) {
		return queued(queue_.push({CommandKind::SetOctave, part, shift}));
	}
	// Stopped: no line to wait for. The shift is the Recipe's, which the next start plays.
	adapter().recipe().octave[part] = static_cast<int8_t>(shift);
	return PressOutcome::Accepted;
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::stepOctave(uint8_t part, int32_t direction, PressOutcome* outcome) {
	*outcome = PressOutcome::NothingToDo;
	if (direction == 0 || !takesOctave(part)) {
		return kNoOctaveWaiting;
	}
	const int32_t shift = octaveAhead(part) + (direction > 0 ? 1 : -1);
	if (!octavesOf(part).contains(shift)) {
		return kNoOctaveWaiting; // the part's octave range ends that way
	}
	*outcome = pressOctave(part, shift);
	return refused(*outcome) || *outcome == PressOutcome::NothingToDo ? kNoOctaveWaiting : shift;
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::octaveAhead(int32_t part) const {
	if (!takesOctave(part)) {
		return 0;
	}
	const Generator* piece = playing();
	if (piece == nullptr) {
		return adapter().recipe().octave[part];
	}
	// A press the core hasn't drained yet goes on as pushed: a shift waits, and the one playing cancels the wait.
	if (const Command* queued = queue_.newest(CommandKind::SetOctave, static_cast<uint8_t>(part))) {
		return queued->value;
	}
	const int32_t waiting = piece->octaveWaiting(part);
	return waiting != kNoOctaveWaiting ? waiting : piece->octave(part);
}

template <typename Adapter>
PressOutcome PerformSteering<Adapter>::pressVariant(int32_t set, int32_t variant) {
	if (gated(PressKind::Variant)) {
		return PressOutcome::ChangePending; // it would wait for a Drop the outgoing piece never plays
	}
	const Genre* genre = rosterGenre();
	if (leaving_ || !entered_ || genre == nullptr || !switchable(*genre, set, variant)) {
		return PressOutcome::NothingToDo;
	}
	listPresses_++;
	if (playing() != nullptr) {
		return queued(queue_.push({CommandKind::SetVariant, static_cast<uint8_t>(set), variant}));
	}
	// Stopped: no Drop to wait for. The variant is the Recipe's, as a live switch landing makes it
	// (Generator::apply()).
	switchVariant(adapter().recipe().variants, set, variant);
	return PressOutcome::Accepted;
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::waitingVariants() const {
	int32_t count = 0;
	forEachWaitingVariant([&count](uint8_t /*set*/, int32_t /*variant*/) { count++; });
	return count;
}

template <typename Adapter>
void PerformSteering<Adapter>::dropWaitingVariants() {
	// Each press of the variant playing drains after the switch it drops, at the same decision: the core cancels the
	// one waiting (spec §13.6).
	forEachWaitingVariant([this](uint8_t set, int32_t variant) {
		listPresses_++;
		queue_.push({CommandKind::SetVariant, set, variant});
	});
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::variantAhead(int32_t set) const {
	const Generator* piece = playing();
	if (piece != nullptr && set >= 0 && set < kMaxVariantSets) {
		// A press the core hasn't drained yet goes on as pushed: a switch waits, and the one playing cancels the wait.
		if (const Command* queued = queue_.newest(CommandKind::SetVariant, static_cast<uint8_t>(set))) {
			return queued->value;
		}
		const int32_t waiting = piece->variantWaiting(set);
		if (waiting >= 0) {
			return waiting;
		}
	}
	return variantPlaying(set);
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::variantPlaying(int32_t set) const {
	const Genre* genre = rosterGenre();
	if (genre == nullptr || set < 0 || set >= static_cast<int32_t>(genre->variantSets.size())) {
		return -1;
	}
	const Generator* piece = playing();
	return (piece != nullptr ? piece->variants() : adapter().recipe().variants).pick[set];
}

template <typename Adapter>
int32_t PerformSteering<Adapter>::octave(int32_t part) const {
	const Generator* piece = playing();
	return piece != nullptr ? piece->octave(part) : adapter().recipe().octave[part];
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
	const int32_t ticks = glideLengthTicks(); // fits 16 bits: genre_data_spec checks each genre's Phrase glide
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
	const int32_t ticks = glideLengthTicks(); // fits 16 bits, as in setTempo()
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
void PerformSteering<Adapter>::setBias(int32_t part, Macro macro, int32_t units) {
	if (!takesBias(part)) {
		return;
	}
	PartBiases& bias = adapter().recipe().bias;
	const int32_t before = bias.of(part, macro);
	bias.set(part, macro, units);
	// A full queue loses the turn: the Recipe keeps what the core plays, as a macro's centre does.
	if (macroSteering_.biasHeldOn(macro) != part && !pushBias(part, macro, bias.of(part, macro))) {
		bias.set(part, macro, before);
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::holdBias(int32_t part, Macro macro, int32_t units) {
	if (!takesBias(part)) {
		return;
	}
	PartBiases held;
	held.set(part, macro, units); // clamped as the Recipe's
	if (pushBias(part, macro, held.of(part, macro))) {
		macroSteering_.holdBias(part, macro, held.of(part, macro));
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::keepBias(int32_t part, Macro macro, int32_t units) {
	macroSteering_.letGoBias(macro);
	setBias(part, macro, units);
}

template <typename Adapter>
void PerformSteering<Adapter>::springBiasBack(int32_t part, Macro macro) {
	macroSteering_.letGoBias(macro);
	if (takesBias(part)) {
		pushBias(part, macro, adapter().recipe().bias.of(part, macro));
	}
}

template <typename Adapter>
void PerformSteering<Adapter>::resetBias(int32_t part) {
	for (int32_t p = 0; p < rosterParts(); p++) {
		if (part >= 0 && p != part) {
			continue;
		}
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			if (adapter().recipe().bias.of(p, macro) != 0) {
				setBias(p, macro, 0);
			}
		}
	}
}

template <typename Adapter>
bool PerformSteering<Adapter>::pushBias(int32_t part, Macro macro, int32_t units) {
	Command command{CommandKind::SetBias, static_cast<uint8_t>(part), units};
	command.length = static_cast<uint16_t>(macro);
	return !entered_ || queue_.push(command);
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
