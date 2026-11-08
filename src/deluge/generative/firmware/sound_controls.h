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
// (perform/sound_mapping.h), and ramps them there with the AutoParam live ramp (F2, spec §11.7). A stop puts every
// param back at its home.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/event.h"
#include "generative/core/genre.h"
#include "generative/perform/sound_mapping.h"
#include <cstdint>

class AutoParam;
class InstrumentClip;
class ModControllableAudio;
class ModelStackWithThreeMainThings;
class NoteRow;
class ParamManager;
class Sound;
struct ParamCollectionSummary;

namespace deluge::generative::firmware {

struct Roster;

class SoundControls {
public:
	/// As a piece starts: works out each control channel's engine and the params its controls move on the loaded
	/// presets (spec §11.1: only targets a preset already uses), and reads their homes (spec §11.6: homes are read from
	/// the part's Output; player edits to them come with the homes slice).
	void begin(const Roster& roster, const Genre& genre);
	/// A Control event: each param the control moves ramps to home + the offset over `rampTicks` internal ticks, or
	/// jumps there on a snap (spec §11.7).
	void apply(const Event& event, int32_t rampTicks);
	/// Stop, the piece's end or leaving: every param the controls moved goes back to its home at once.
	void restoreHomes();
	/// Perform's song is going: forgets every bound param without touching it.
	void forget() {
		channelCount_ = 0;
		boundCount_ = 0;
	}

	/// For H2's worst case (spec §20), test-port builds: ramps every bound param halfway up its range from home, or
	/// halfway down, over `rampTicks`, as if every control were rewritten each step.
	void stress(bool up, int32_t rampTicks);

	/// For H2 (spec §20): params armed or snapped since the last call.
	uint32_t takeParamsMoved() {
		uint32_t moved = paramsMoved_;
		paramsMoved_ = 0;
		return moved;
	}

	/// Params one channel's controls can move: per control, the most params any engine expands its targets into
	/// (each oscillator, modulator, carrier or LFO, and every patch cable from an LFO).
	static constexpr int32_t kMaxParamsPerChannel = 3 + 3 + 5 + 2 + (1 + 4 + kMaxNumPatchCables) + 1;

private:
	/// Where a param sits and the range its control gives it.
	struct Bound {
		AutoParam* param = nullptr;
		int32_t home = 0;
		perform::TargetRange range;
		uint8_t collection = 0; ///< 0 unpatched, 1 patched, 2 patch cables (ParamManager summary order).
		/// The param's id in its collection, which is also its bit in the collection's interpolating mask; for a patch
		/// cable, the cable's index.
		uint8_t index = 0;
	};
	/// One set of controls: a synth part's, a Kit's affect-entire params, or an FX lane's row.
	struct Channel {
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
	};

	/// Binds each param `range` names on `channel`'s preset, if the preset uses it.
	void bind(Channel& channel, const perform::TargetRange& range);
	void bindParam(Channel& channel, const perform::TargetRange& range, uint8_t collection, int32_t index);
	[[nodiscard]] bool uses(const Channel& channel, perform::Needs needs) const;
	/// The model stack that addresses `channel`'s params, built in `memory`.
	ModelStackWithThreeMainThings* modelStack(const Channel& channel, char* memory) const;
	/// Sets a bound param at once, cancelling its ramp, and lets its Sound know.
	void snap(const Channel& channel, Bound& bound, int32_t value);

	Channel channels_[kMaxControlChannels];
	int32_t channelCount_ = 0;
	Bound bound_[kMaxControlChannels * kMaxParamsPerChannel];
	int32_t boundCount_ = 0;
	uint32_t paramsMoved_ = 0;
};

/// One instance, in SDRAM (its bound params take about 7 KB): Perform plays one piece at a time.
extern SoundControls soundControls;

} // namespace deluge::generative::firmware
