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

// The Perform adapter's side of the sound controls (spec §2.5, §11): it reads each control's homes from the parts'
// Outputs as a piece starts, turns each Control event into values for the params the control moves on that engine
// (perform/sound_mapping.h), and ramps them there with the AutoParam live ramp (F2, spec §11.7). A player edit to a
// param a control moves moves its home (F3, spec §11.6). A stop puts every param back at its home.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/event.h"
#include "generative/core/genre.h"
#include "generative/perform/sound_mapping.h"
#include "gui/l10n/strings.h"
#include <cstdint>
#include <optional>
#include <span>

class AutoParam;
class InstrumentClip;
class ModControllableAudio;
class ModelStackWithAutoParam;
class ModelStackWithThreeMainThings;
class NoteRow;
class ParamManager;
class Sound;

namespace deluge::generative::firmware {

struct Roster;

/// A sound control's name (spec §11.1), shared by the PART page and the test events.
l10n::String controlName(SoundControl control);
/// The control's name in capitals into `out`, cut to fit: the PART page's three-letter labels take a 4-char buffer.
void upperControlName(SoundControl control, std::span<char> out);

class SoundControls {
public:
	/// As a piece starts: works out each control set's engine and the params its controls move on the loaded presets
	/// (spec §11.1: only targets a preset already uses), and reads their homes from the parts' Outputs (spec §11.7).
	void begin(const Roster& roster, const Genre& genre);
	/// A sound re-roll swapped `part`'s Output (spec §10.7): its params are bound again on the new Output, with homes
	/// read from it, and each control snaps to the offset it had on the old one. Every other part keeps its homes.
	/// Tick-safe: no allocation, no card. Only while a piece plays: stopped, the next start binds every part.
	void rebindPart(const Roster& roster, const Genre& genre, int32_t part);
	/// A Control event: each param the control moves ramps to home + the offset over `rampTicks` internal ticks, or
	/// jumps there on a snap (spec §11.7).
	void apply(const Event& event, int32_t rampTicks);
	/// Stop, the piece's end or leaving: every param the controls moved goes back to its home at once.
	void restoreHomes();
	/// Perform's song is going: forgets every bound param without touching it.
	void forget() {
		setCount_ = 0;
		boundCount_ = 0;
		forgetOutgoing();
	}
	/// A transition's start step, before begin() binds the new roster (spec §12.9): keeps the Space params of the
	/// roster going out, so its parts' Throws can hold and fall after it (spec §11.4).
	void keepOutgoingSpace();
	/// A Control event of the outgoing instance after the start step: a Throw's Space on the outgoing roster.
	void applyOutgoing(const Event& event, int32_t rampTicks);
	/// The outgoing roster drained or went: forgets its Space params without touching them.
	void forgetOutgoing() {
		outgoingSetCount_ = 0;
		outgoingBoundCount_ = 0;
	}

	/// A home a player edit moved (spec §11.6), as the steering log keys it.
	struct HomeEdit {
		uint8_t part = 0;
		Lane lane = Lane::None;
		uint16_t param = 0; ///< The param on its Output, as paramKey() keys it.
		int32_t home = 0;
	};
	/// F3: the player set the param `stack` addresses to `value`. If a control moves it in the piece playing, its home
	/// moves by the amount turned: home = value − the control's current offset (spec §11.6).
	std::optional<HomeEdit> playerEdited(const ModelStackWithAutoParam& stack, int32_t value);
	/// A CLIP fader tap (spec §13.5): sets the home of the param `stack` addresses, which then plays home plus its
	/// control's current offset, written as a player edit so F3 logs it. Stopped, or for a param no control moves in
	/// the piece playing, the param is set to `home`.
	void setHome(const ModelStackWithAutoParam& stack, int32_t home);

	/// The param `control`'s CLIP fader and F7-again knob set on `part` (spec §13.5): the first param the control
	/// moves on the part's own controls (a Kit's kit-wide ones), if the part's preset uses it now. Stopped or playing.
	/// nullptr if there's none, as on an FX part, whose lanes each have their own. Built in `memory`
	/// (MODEL_STACK_MAX_SIZE).
	ModelStackWithAutoParam* leadParam(const Roster& roster, const Genre& genre, int32_t part, SoundControl control,
	                                   char* memory) const;
	/// The home of the param `stack` addresses: where a control moves it from in the piece playing, or else its
	/// value now.
	[[nodiscard]] int32_t homeOf(const ModelStackWithAutoParam& stack);

#if ENABLE_SYSEX_TEST
	/// Whether any control set, the outgoing roster's included, is bound to `clip`'s Output: the free job's check.
	[[nodiscard]] bool boundTo(const InstrumentClip* clip) const;

	/// For H2's worst case (spec §20): ramps every bound param halfway up its range from home, or halfway down, over
	/// `rampTicks`, as if every control were rewritten each step.
	void stress(bool up, int32_t rampTicks);

	/// For H2 (spec §20): params armed or snapped since the last call.
	uint32_t takeParamsMoved() {
		uint32_t moved = paramsMoved_;
		paramsMoved_ = 0;
		return moved;
	}
#endif

	/// Params one set's controls can move, from the spec §11.2 table: per target, every oscillator, modulator, carrier
	/// or LFO it can expand into, and every patch cable from an LFO.
	static constexpr int32_t kMaxParamsPerSet = perform::maxParamsPerSet(kMaxNumPatchCables);

private:
	/// A ParamManager's collections, in its summaries' order.
	enum class Collection : uint8_t { Unpatched, Patched, Cables };
	/// Where a param sits and the range its control gives it.
	struct Bound {
		int32_t home = 0;
		perform::TargetRange range;
		Collection collection = Collection::Unpatched;
		/// The param's id in its collection; for a patch cable, the param it goes to. A cable is found by its source
		/// and destination each time, since stock edits reorder the cables (`PatchCableSet::setupPatching`).
		uint8_t param = 0;
		PatchSource source = PatchSource::NONE; ///< A patch cable's source.
	};
	/// One control set: a synth part's, a Kit's affect-entire params, or a Drums or FX lane's row.
	struct Set {
		uint8_t part = 0;
		Lane lane = Lane::None;
		perform::Engine engine = perform::Engine::Subtractive;
		InstrumentClip* clip = nullptr; ///< The part's clip.
		int32_t row = -1;               ///< The NoteRow index of a lane's drum, or -1.
		NoteRow* noteRow = nullptr;
		ModControllableAudio* audio = nullptr; ///< The Sound, or the Kit for its affect-entire params.
		Sound* sound = nullptr;                ///< nullptr for a Kit's affect-entire params.
		ParamManager* paramManager = nullptr;
		uint16_t first[kNumSoundControls + 1] = {}; ///< Each control's params: bound_[first[c]] up to first[c + 1].
		int8_t offset[kNumSoundControls] = {};      ///< Each control's offset as last applied: home (0) at the start.
	};

	/// Binds every set of `part` after the ones bound already, with homes read from its Output now.
	void bindPart(const Roster& roster, const Genre& genre, int32_t part);
	/// Sets up `part`'s set for its lane `l` (an FX part's lanes each have one). False if it has nothing to move.
	static bool makeSet(const Roster& roster, const Genre& genre, int32_t part, int32_t l, Set& set);
	/// Hands each param `range` names on `set`'s preset to `found(collection, param, source)`.
	template <typename Found>
	static void expand(const Set& set, const perform::TargetRange& range, Found found);
	static bool uses(const Set& set, perform::Needs needs);
	void bindParam(const Set& set, const perform::TargetRange& range, Collection collection, uint8_t param,
	               PatchSource source);
	/// A bound param's AutoParam, or nullptr if it is a patch cable that's gone.
	static AutoParam* paramOf(const Set& set, const Bound& bound);
	/// The same, given slotOf(): its id in its collection, or its cable's index (-1 if gone).
	static AutoParam* paramAt(const Set& set, const Bound& bound, int32_t slot);
	/// A bound param's slot in its collection: its param id, or a cable's index there (-1 if gone), which is also its
	/// bit in the collection's interpolating mask.
	static int32_t slotOf(const Set& set, const Bound& bound);
	/// A bound param a model stack addresses: its set, the param and the control that moves it.
	struct Match {
		Set& set;
		Bound& bound;
		int32_t control;
	};
	/// The bound param `stack` addresses in the piece playing, if any.
	std::optional<Match> find(const ModelStackWithAutoParam& stack);
	/// A bound param as the steering log keys it on its Output (SetHome's `length`): bits 13–14 its collection, 8–12 a
	/// patch cable's source, 0–7 its param id (a cable's destination param).
	static uint16_t paramKey(const Bound& bound);
	/// A bound param's id in its collection, as a model stack carries it: for a patch cable, its source and
	/// destination.
	static int32_t idOf(const Bound& bound);
	/// The model stack that addresses `set`'s params, built in `memory`.
	static ModelStackWithThreeMainThings* modelStack(const Set& set, char* memory);
	static ModelStackWithAutoParam* paramStack(const Set& set, const Bound& bound, AutoParam* param, char* memory);
	/// Ramps a bound param to `value` (F2), flagging it as interpolating so the render pre-checks tick it (spec §19).
	void armRamp(const Set& set, const Bound& bound, int32_t value, int32_t rampTicks);
	/// Sets a bound param at once, cancelling its ramp, and lets its Sound know.
	static void snap(const Set& set, const Bound& bound, int32_t value);

	/// apply() on `count` sets and the params they index in `params`.
	void applyIn(Set* sets, int32_t count, const Bound* params, const Event& event, int32_t rampTicks);

	Set sets_[kMaxControlSets];
	int32_t setCount_ = 0;
	Bound bound_[kMaxControlSets * kMaxParamsPerSet];
	int32_t boundCount_ = 0;
	/// The outgoing roster's sets after a start step, each with only its Space params (keepOutgoingSpace()).
	static constexpr int32_t kMaxSpaceParams = perform::maxParamsFor(SoundControl::Space, kMaxNumPatchCables);
	Set outgoingSets_[kMaxControlSets];
	int32_t outgoingSetCount_ = 0;
	Bound outgoingBound_[kMaxControlSets * kMaxSpaceParams];
	int32_t outgoingBoundCount_ = 0;
	/// setHome() is writing the param: the player edit it makes keeps the home it set.
	bool settingHome_ = false;
#if ENABLE_SYSEX_TEST
	uint32_t paramsMoved_ = 0;
#endif
};

/// One instance, in SDRAM (5.6 K, nearly all of it bound params): Perform plays one piece at a time.
extern SoundControls soundControls;

} // namespace deluge::generative::firmware
