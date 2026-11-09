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

#include "generative/firmware/sound_controls.h"
#include "definitions.h"
#include "generative/firmware/perform_song.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection_summary.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SoundControls soundControls{};

namespace params = deluge::modulation::params;
using perform::Engine;
using perform::Needs;
using perform::Target;
using perform::TargetRange;

namespace {

constexpr uint8_t kUnpatched = 0;
constexpr uint8_t kPatched = 1;
constexpr uint8_t kCables = 2;

/// The column of the spec §11.2 table a Sound plays from.
Engine engineOf(const Sound& sound, bool drum) {
	if (sound.synthMode == SynthMode::FM) {
		return Engine::Fm;
	}
	if (sound.synthMode == SynthMode::RINGMOD) {
		return Engine::Ringmod;
	}
	if (sound.sources[0].oscType == OscType::DX7) {
		return Engine::Dx7;
	}
	if (drum && sound.sources[0].oscType == OscType::SAMPLE) {
		return Engine::KitRow;
	}
	return Engine::Subtractive;
}

bool isLfo(PatchSource source) {
	return source == PatchSource::LFO_GLOBAL_1 || source == PatchSource::LFO_GLOBAL_2
	       || source == PatchSource::LFO_LOCAL_1 || source == PatchSource::LFO_LOCAL_2;
}

constexpr int32_t kMenuZero = INT32_MIN; ///< A standard param at menu value 0.
constexpr int32_t kMenuFull = INT32_MAX; ///< A standard param at menu value 50.

} // namespace

void SoundControls::begin(const Roster& roster, const Genre& genre) {
	channelCount_ = 0;
	boundCount_ = 0;
	// The channels in the order the core lays them out (spec §11.4): a part's own, kit-wide on Drums, or each FX lane.
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& spec = genre.parts[p];
		const PartOutput& part = roster.parts[p];
		if (part.output == nullptr || part.clip == nullptr) {
			continue;
		}
		const int32_t lanes = spec.role == Role::FX ? static_cast<int32_t>(spec.lanes.size()) : 1;
		for (int32_t l = 0; l < lanes && channelCount_ < kMaxControlChannels; l++) {
			Channel channel;
			channel.part = static_cast<uint8_t>(p);
			channel.clip = part.clip;
			if (spec.role == Role::FX) {
				channel.lane = spec.lanes[l].lane;
				Drum* drum = part.drums[laneRow(channel.lane)];
				int32_t row = -1;
				NoteRow* noteRow = drum != nullptr ? part.clip->getNoteRowForDrum(drum, &row) : nullptr;
				if (noteRow == nullptr || drum->type != DrumType::SOUND) {
					continue; // a lane with nothing loaded has no params to move
				}
				auto* sound = static_cast<SoundDrum*>(drum);
				channel.engine = engineOf(*sound, true);
				channel.row = row;
				channel.noteRow = noteRow;
				channel.audio = sound;
				channel.sound = sound;
				channel.paramManager = &noteRow->paramManager;
			}
			else if (part.output->type == OutputType::KIT) {
				auto* kit = static_cast<Kit*>(part.output);
				channel.engine = Engine::KitLevel;
				channel.audio = kit;
				channel.paramManager = &part.clip->paramManager;
			}
			else {
				auto* synth = static_cast<SoundInstrument*>(part.output);
				channel.engine = engineOf(*synth, false);
				channel.audio = synth;
				channel.sound = synth;
				channel.paramManager = &part.clip->paramManager;
			}
			for (int32_t c = 0; c < kNumSoundControls; c++) {
				channel.first[c] = static_cast<uint16_t>(boundCount_);
				for (const TargetRange& range : perform::controlTargets(static_cast<SoundControl>(c), channel.engine)) {
					if (uses(channel, range.needs)) {
						bind(channel, range);
					}
				}
			}
			channel.first[kNumSoundControls] = static_cast<uint16_t>(boundCount_);
			channels_[channelCount_++] = channel;
		}
	}
	TEST_EVENT("gen_controls channels=%ld params=%ld", static_cast<long>(channelCount_),
	           static_cast<long>(boundCount_));
}

bool SoundControls::uses(const Channel& channel, Needs needs) const {
	ParamManager& manager = *channel.paramManager;
	auto patched = [&](int32_t p) { return manager.getPatchedParamSet()->getValue(p); };
	const ModControllableAudio* audio = channel.audio;
	switch (needs) {
	case Needs::Always:
		return true;
	case Needs::LpfInUse:
		return audio->lpfMode != FilterMode::OFF;
	case Needs::HpfInUse:
		return audio->hpfMode != FilterMode::OFF && patched(params::LOCAL_HPF_FREQ) > kMenuZero;
	case Needs::SustainBelowFull:
		return patched(params::LOCAL_ENV_0_SUSTAIN) < kMenuFull;
	case Needs::Env2InUse:
		return manager.getPatchCableSet()->isSourcePatchedToSomething(PatchSource::ENVELOPE_1);
	case Needs::DelayInUse:
		return patched(params::GLOBAL_DELAY_FEEDBACK) > kMenuZero;
	case Needs::ModFxInUse:
		return audio->modFXType_ != ModFXType::NONE;
	case Needs::Modulator1InUse:
		return patched(params::LOCAL_MODULATOR_0_VOLUME) > kMenuZero;
	case Needs::Modulator2InUse:
		return patched(params::LOCAL_MODULATOR_1_VOLUME) > kMenuZero;
	}
	return false;
}

void SoundControls::bind(Channel& channel, const TargetRange& range) {
	const bool kitLevel = channel.engine == Engine::KitLevel;
	const Sound* sound = channel.sound;
	switch (range.target) {
	case Target::LpfFrequency:
		kitLevel ? bindParam(channel, range, kUnpatched, params::UNPATCHED_LPF_FREQ)
		         : bindParam(channel, range, kPatched, params::LOCAL_LPF_FREQ);
		break;
	case Target::LpfMorph:
		bindParam(channel, range, kPatched, params::LOCAL_LPF_MORPH);
		break;
	case Target::HpfFrequency:
		kitLevel ? bindParam(channel, range, kUnpatched, params::UNPATCHED_HPF_FREQ)
		         : bindParam(channel, range, kPatched, params::LOCAL_HPF_FREQ);
		break;
	case Target::WaveIndex:
		if (sound->sources[0].oscType == OscType::WAVETABLE) {
			bindParam(channel, range, kPatched, params::LOCAL_OSC_A_WAVE_INDEX);
		}
		if (sound->sources[1].oscType == OscType::WAVETABLE) {
			bindParam(channel, range, kPatched, params::LOCAL_OSC_B_WAVE_INDEX);
		}
		break;
	case Target::PulseWidth:
		bindParam(channel, range, kPatched, params::LOCAL_OSC_A_PHASE_WIDTH);
		bindParam(channel, range, kPatched, params::LOCAL_OSC_B_PHASE_WIDTH);
		break;
	case Target::Modulator1Level:
		bindParam(channel, range, kPatched, params::LOCAL_MODULATOR_0_VOLUME);
		break;
	case Target::Modulator2Level:
		bindParam(channel, range, kPatched, params::LOCAL_MODULATOR_1_VOLUME);
		break;
	case Target::ModulatorFeedback:
		if (uses(channel, Needs::Modulator1InUse)) {
			bindParam(channel, range, kPatched, params::LOCAL_MODULATOR_0_FEEDBACK);
		}
		if (uses(channel, Needs::Modulator2InUse)) {
			bindParam(channel, range, kPatched, params::LOCAL_MODULATOR_1_FEEDBACK);
		}
		break;
	case Target::CarrierFeedback:
		bindParam(channel, range, kPatched, params::LOCAL_CARRIER_0_FEEDBACK);
		bindParam(channel, range, kPatched, params::LOCAL_CARRIER_1_FEEDBACK);
		break;
	case Target::Fold:
		bindParam(channel, range, kPatched, params::LOCAL_FOLD);
		break;
	case Target::Env1Release:
		bindParam(channel, range, kPatched, params::LOCAL_ENV_0_RELEASE);
		break;
	case Target::Env1Decay:
		bindParam(channel, range, kPatched, params::LOCAL_ENV_0_DECAY);
		break;
	case Target::Env1Sustain:
		bindParam(channel, range, kPatched, params::LOCAL_ENV_0_SUSTAIN);
		break;
	case Target::Env2Decay:
		bindParam(channel, range, kPatched, params::LOCAL_ENV_1_DECAY);
		break;
	case Target::ReverbSend:
		kitLevel ? bindParam(channel, range, kUnpatched, params::UNPATCHED_REVERB_SEND_AMOUNT)
		         : bindParam(channel, range, kPatched, params::GLOBAL_REVERB_AMOUNT);
		break;
	case Target::DelayFeedback:
		bindParam(channel, range, kPatched, params::GLOBAL_DELAY_FEEDBACK);
		break;
	case Target::ModFxDepth:
		kitLevel ? bindParam(channel, range, kUnpatched, params::UNPATCHED_MOD_FX_DEPTH)
		         : bindParam(channel, range, kPatched, params::GLOBAL_MOD_FX_DEPTH);
		break;
	case Target::LfoRate: {
		// Only LFOs patched to something: an unused LFO's rate is never heard.
		PatchCableSet* cables = channel.paramManager->getPatchCableSet();
		constexpr struct {
			PatchSource source;
			int32_t param;
		} kLfos[] = {
		    {PatchSource::LFO_GLOBAL_1, params::GLOBAL_LFO_FREQ_1},
		    {PatchSource::LFO_GLOBAL_2, params::GLOBAL_LFO_FREQ_2},
		    {PatchSource::LFO_LOCAL_1, params::LOCAL_LFO_LOCAL_FREQ_1},
		    {PatchSource::LFO_LOCAL_2, params::LOCAL_LFO_LOCAL_FREQ_2},
		};
		for (const auto& lfo : kLfos) {
			if (cables->isSourcePatchedToSomething(lfo.source)) {
				bindParam(channel, range, kPatched, lfo.param);
			}
		}
		break;
	}
	case Target::LfoCables: {
		// Existing cables only: Wobble never adds one (spec §11.2).
		PatchCableSet* cables = channel.paramManager->getPatchCableSet();
		for (int32_t c = 0; c < cables->numPatchCables; c++) {
			if (isLfo(cables->patchCables[c].from)) {
				bindParam(channel, range, kCables, c);
			}
		}
		break;
	}
	}
}

void SoundControls::bindParam(Channel& channel, const TargetRange& range, uint8_t collection, int32_t index) {
	if (boundCount_ >= kMaxControlChannels * kMaxParamsPerChannel) {
		return; // never, by kMaxParamsPerChannel's construction
	}
	ParamManager& manager = *channel.paramManager;
	AutoParam* param = collection == kUnpatched ? &manager.getUnpatchedParamSet()->params[index]
	                   : collection == kPatched ? &manager.getPatchedParamSet()->params[index]
	                                            : &manager.getPatchCableSet()->patchCables[index].param;
	if (param->isAutomated()) {
		return; // Perform's clips hold no automation (spec §11.7); a param that has some is left to it
	}
	Bound& bound = bound_[boundCount_++];
	bound.param = param;
	bound.home = param->getCurrentValue();
	bound.range = range;
	bound.collection = collection;
	bound.index = static_cast<uint8_t>(index);
}

ModelStackWithThreeMainThings* SoundControls::modelStack(const Channel& channel, char* memory) const {
	ModelStackWithTimelineCounter* withClip =
	    setupModelStackWithSong(memory, currentSong)->addTimelineCounter(channel.clip);
	if (channel.noteRow != nullptr) {
		return withClip->addNoteRow(channel.row, channel.noteRow)
		    ->addOtherTwoThings(channel.audio, channel.paramManager);
	}
	return withClip->addOtherTwoThingsButNoNoteRow(channel.audio, channel.paramManager);
}

void SoundControls::snap(const Channel& channel, Bound& bound, int32_t value) {
	bound.param->resetInterpolationIncrement();
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* stack = modelStack(channel, memory);
	ParamCollectionSummary* summary = &channel.paramManager->summaries[bound.collection];
	int32_t paramId = bound.index;
	if (bound.collection == kCables) {
		const PatchCable& cable = channel.paramManager->getPatchCableSet()->patchCables[bound.index];
		paramId = PatchCableSet::getParamId(cable.destinationParamDescriptor, cable.from);
	}
	bound.param->setCurrentValueWithNoReversionOrRecording(
	    stack->addParam(summary->paramCollection, summary, paramId, bound.param), value);
}

void SoundControls::apply(const Event& event, int32_t rampTicks) {
	for (int32_t i = 0; i < channelCount_; i++) {
		Channel& channel = channels_[i];
		if (channel.part != event.part || channel.lane != event.lane) {
			continue;
		}
		const auto c = static_cast<int32_t>(event.control);
		for (int32_t b = channel.first[c]; b < channel.first[c + 1]; b++) {
			Bound& bound = bound_[b];
			const int32_t value = perform::targetValue(bound.range, bound.home, event.offset);
			paramsMoved_++;
			if (event.snap) {
				snap(channel, bound, value);
			}
			else if (bound.param->setLiveRamp(value, rampTicks)) {
				// F2: the render pre-checks tick a param flagged as interpolating (spec §19).
				ParamCollectionSummary& summary = channel.paramManager->summaries[bound.collection];
				summary.whichParamsAreInterpolating[bound.index >> 5] |= 1U << (bound.index & 31);
			}
		}
		return;
	}
}

void SoundControls::stress(bool up, int32_t rampTicks) {
	for (int32_t i = 0; i < channelCount_; i++) {
		Channel& channel = channels_[i];
		for (int32_t b = channel.first[0]; b < channel.first[kNumSoundControls]; b++) {
			Bound& bound = bound_[b];
			paramsMoved_++;
			if (bound.param->setLiveRamp(perform::targetValue(bound.range, bound.home, up ? 50 : -50), rampTicks)) {
				ParamCollectionSummary& summary = channel.paramManager->summaries[bound.collection];
				summary.whichParamsAreInterpolating[bound.index >> 5] |= 1U << (bound.index & 31);
			}
		}
	}
}

void SoundControls::restoreHomes() {
	for (int32_t i = 0; i < channelCount_; i++) {
		Channel& channel = channels_[i];
		for (int32_t b = channel.first[0]; b < channel.first[kNumSoundControls]; b++) {
			Bound& bound = bound_[b];
			bound.param->resetInterpolationIncrement(); // a ramp still running mustn't carry on
			if (bound.param->getCurrentValue() != bound.home) {
				snap(channel, bound, bound.home);
			}
		}
	}
	channelCount_ = 0;
	boundCount_ = 0;
}

} // namespace deluge::generative::firmware
