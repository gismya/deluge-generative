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
#include <cstdint>
#include <optional>

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

class SoundControls {
public:
	/// As a piece starts: works out each control set's engine and the params its controls move on the loaded presets
	/// (spec §11.1: only targets a preset already uses), and reads their homes from the parts' Outputs (spec §11.7).
	void begin(const Roster& roster, const Genre& genre);
	/// A Control event: each param the control moves ramps to home + the offset over `rampTicks` internal ticks, or
	/// jumps there on a snap (spec §11.7).
	void apply(const Event& event, int32_t rampTicks);
	/// Stop, the piece's end or leaving: every param the controls moved goes back to its home at once.
	void restoreHomes();
	/// Perform's song is going: forgets every bound param without touching it.
	void forget() {
		setCount_ = 0;
		boundCount_ = 0;
	}

	/// A home a player edit moved (spec §11.6), as the steering log keys it.
	struct HomeEdit {
		uint8_t part = 0;
		Lane lane = Lane::None;
		SoundControl control = SoundControl::Brightness;
		uint16_t param = 0; ///< The param on its Output: paramKey().
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
	/// A param's home: where a control moves it from in the piece playing, or else its value now.
	[[nodiscard]] int32_t homeOf(AutoParam& param) const;

#if ENABLE_SYSEX_TEST
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
	/// One control set: a synth part's, a Kit's affect-entire params, or an FX lane's row.
	struct Set {
		uint8_t part = 0;
		Lane lane = Lane::None;
		perform::Engine engine = perform::Engine::Subtractive;
		InstrumentClip* clip = nullptr; ///< The part's clip.
		int32_t row = -1;               ///< The NoteRow index of an FX lane's drum, or -1.
		NoteRow* noteRow = nullptr;
		ModControllableAudio* audio = nullptr; ///< The Sound, or the Kit for its affect-entire params.
		Sound* sound = nullptr;                ///< nullptr for a Kit's affect-entire params.
		ParamManager* paramManager = nullptr;
		uint16_t first[kNumSoundControls + 1] = {}; ///< Each control's params: bound_[first[c]] up to first[c + 1].
		int8_t offset[kNumSoundControls] = {};      ///< Each control's offset as last applied: home (0) at the start.
	};

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
	/// A bound param's id in its collection, and its bit in the collection's interpolating mask.
	static int32_t bitOf(const Set& set, const Bound& bound);
	/// The set and the bound param `stack` addresses in the piece playing, and its control; false if none.
	bool find(const ModelStackWithAutoParam& stack, Set** set, Bound** bound, int32_t* control);
	/// The model stack that addresses `set`'s params, built in `memory`.
	static ModelStackWithThreeMainThings* modelStack(const Set& set, char* memory);
	static ModelStackWithAutoParam* paramStack(const Set& set, const Bound& bound, AutoParam* param, char* memory);
	/// Ramps a bound param to `value` (F2), flagging it as interpolating so the render pre-checks tick it (spec §19).
	void armRamp(const Set& set, const Bound& bound, int32_t value, int32_t rampTicks);
	/// Sets a bound param at once, cancelling its ramp, and lets its Sound know.
	static void snap(const Set& set, const Bound& bound, int32_t value);

	Set sets_[kMaxControlSets];
	int32_t setCount_ = 0;
	Bound bound_[kMaxControlSets * kMaxParamsPerSet];
	int32_t boundCount_ = 0;
	/// setHome() is writing the param: the player edit it makes keeps the home it set.
	bool settingHome_ = false;
#if ENABLE_SYSEX_TEST
	uint32_t paramsMoved_ = 0;
#endif
};

/// One instance, in SDRAM (5.6 K, nearly all of it bound params): Perform plays one piece at a time.
extern SoundControls soundControls;

} // namespace deluge::generative::firmware
