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
#include "gui/l10n/l10n.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection_summary.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include <algorithm>
#include <cctype>
#include <iterator>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SoundControls soundControls{};

namespace params = deluge::modulation::params;
using perform::Engine;
using perform::Needs;
using perform::Target;
using perform::TargetRange;

namespace {

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

/// Level and pan, which the Generator never moves (spec §11.8), so Wobble leaves an LFO's cables to them alone.
bool isLevelOrPan(int32_t param) {
	return param == params::LOCAL_OSC_A_VOLUME || param == params::LOCAL_OSC_B_VOLUME || param == params::LOCAL_VOLUME
	       || param == params::LOCAL_NOISE_VOLUME || param == params::LOCAL_PAN
	       || param == params::GLOBAL_VOLUME_POST_FX || param == params::GLOBAL_VOLUME_POST_REVERB_SEND;
}

ParamDescriptor justParam(int32_t param) {
	ParamDescriptor descriptor;
	descriptor.setToHaveParamOnly(param);
	return descriptor;
}

constexpr int32_t kMenuZero = INT32_MIN; ///< A standard param at menu value 0.
constexpr int32_t kMenuFull = INT32_MAX; ///< A standard param at menu value 50.

#if ENABLE_SYSEX_TEST
/// A control's name in capitals, for test events.
struct UpperName {
	char text[16] = {};
	explicit UpperName(SoundControl control) { upperControlName(control, text); }
};
#endif

} // namespace

void upperControlName(SoundControl control, std::span<char> out) {
	const char* name = l10n::get(controlName(control));
	size_t i = 0;
	for (; i + 1 < out.size() && name[i] != '\0'; i++) {
		out[i] = static_cast<char>(toupper(static_cast<unsigned char>(name[i])));
	}
	out[i] = '\0';
}

l10n::String controlName(SoundControl control) {
	// Indexed by SoundControl.
	constexpr l10n::String kNames[] = {
	    l10n::String::STRING_FOR_BRIGHTNESS, l10n::String::STRING_FOR_DECAY,  l10n::String::STRING_FOR_DRIVE,
	    l10n::String::STRING_FOR_SPACE,      l10n::String::STRING_FOR_WOBBLE, l10n::String::STRING_FOR_THIN,
	};
	static_assert(std::size(kNames) == kNumSoundControls);
	return kNames[static_cast<int32_t>(control)];
}

bool SoundControls::makeSet(const Roster& roster, const Genre& genre, int32_t p, int32_t l, Set& set) {
	const PartSpec& spec = genre.parts[p];
	const PartOutput& part = roster.parts[p];
	if (part.output == nullptr || part.clip == nullptr) {
		return false;
	}
	set = Set{};
	set.part = static_cast<uint8_t>(p);
	set.clip = part.clip;
	set.lane = controlSetLane(spec, l);
	if (set.lane != Lane::None) {
		// A Drums or FX lane's row controls.
		Drum* drum = part.drums[laneRow(set.lane)];
		int32_t row = -1;
		NoteRow* noteRow = drum != nullptr ? part.clip->getNoteRowForDrum(drum, &row) : nullptr;
		if (noteRow == nullptr || drum->type != DrumType::SOUND) {
			return false; // a lane with nothing loaded has no params to move
		}
		auto* sound = static_cast<SoundDrum*>(drum);
		set.engine = engineOf(*sound, true);
		set.row = row;
		set.noteRow = noteRow;
		set.audio = sound;
		set.sound = sound;
		set.paramManager = &noteRow->paramManager;
	}
	else if (part.output->type == OutputType::KIT) {
		set.engine = Engine::KitLevel;
		set.audio = static_cast<Kit*>(part.output);
		set.paramManager = &part.clip->paramManager;
	}
	else {
		auto* synth = static_cast<SoundInstrument*>(part.output);
		set.engine = engineOf(*synth, false);
		set.audio = synth;
		set.sound = synth;
		set.paramManager = &part.clip->paramManager;
	}
	return true;
}

void SoundControls::begin(const Roster& roster, const Genre& genre) {
	setCount_ = 0;
	boundCount_ = 0;
	// The sets in the order the core lays them out (spec §11.1): a part's own, kit-wide then each lane on Drums, or
	// each FX lane.
	for (int32_t p = 0; p < partCount(genre); p++) {
		bindPart(roster, genre, p);
	}
	TEST_EVENT("gen_controls sets=%ld params=%ld", static_cast<long>(setCount_), static_cast<long>(boundCount_));
}

void SoundControls::bindPart(const Roster& roster, const Genre& genre, int32_t part) {
	for (int32_t l = 0; l < controlSetsOf(genre.parts[part]) && setCount_ < kMaxControlSets; l++) {
		Set set;
		if (!makeSet(roster, genre, part, l, set)) {
			continue;
		}
		for (int32_t c = 0; c < kNumSoundControls; c++) {
			set.first[c] = static_cast<uint16_t>(boundCount_);
			for (const TargetRange& range : perform::controlTargets(static_cast<SoundControl>(c), set.engine)) {
				if (uses(set, range.needs)) {
					expand(set, range, [&](Collection collection, uint8_t param, PatchSource source) {
						bindParam(set, range, collection, param, source);
					});
				}
			}
		}
		set.first[kNumSoundControls] = static_cast<uint16_t>(boundCount_);
		sets_[setCount_++] = set;
	}
}

void SoundControls::rebindPart(const Roster& roster, const Genre& genre, int32_t part) {
	// The part's old sets go, with their params: they point into the Output it no longer plays. The rest close up,
	// keeping their homes and offsets.
	struct Kept {
		Lane lane;
		int8_t offset[kNumSoundControls];
	};
	Kept old[1 + kMaxLanesPerPart]; // a Drums part's kit-wide set and each lane's
	int32_t oldCount = 0;
	int32_t sets = 0;
	int32_t bound = 0;
	for (int32_t i = 0; i < setCount_; i++) {
		Set set = sets_[i];
		const int32_t from = set.first[0];
		const int32_t to = set.first[kNumSoundControls];
		if (set.part == part) {
			if (oldCount < static_cast<int32_t>(std::size(old))) {
				old[oldCount].lane = set.lane;
				std::copy_n(set.offset, kNumSoundControls, old[oldCount].offset);
				oldCount++;
			}
			continue;
		}
		std::copy(bound_ + from, bound_ + to, bound_ + bound);
		for (uint16_t& first : set.first) {
			first = static_cast<uint16_t>(first - (from - bound));
		}
		bound += to - from;
		sets_[sets++] = set;
	}
	setCount_ = sets;
	boundCount_ = bound;
	// The new sound's homes are its preset's, and each control plays the offset it had on the old one (spec §11.6).
	const int32_t firstNew = setCount_;
	bindPart(roster, genre, part);
	for (int32_t i = firstNew; i < setCount_; i++) {
		for (int32_t o = 0; o < oldCount; o++) {
			if (old[o].lane != sets_[i].lane) {
				continue;
			}
			for (int32_t c = 0; c < kNumSoundControls; c++) {
				if (old[o].offset[c] != 0) {
					apply(Event::controlChange(0, static_cast<uint8_t>(part), sets_[i].lane,
					                           static_cast<SoundControl>(c), old[o].offset[c], true),
					      0);
				}
			}
		}
	}
	TEST_EVENT("gen_controls rebind part=%ld sets=%ld params=%ld", static_cast<long>(part),
	           static_cast<long>(setCount_), static_cast<long>(boundCount_));
}

bool SoundControls::uses(const Set& set, Needs needs) {
	ParamManager& manager = *set.paramManager;
	auto patched = [&](int32_t p) { return manager.getPatchedParamSet()->getValue(p); };
	const ModControllableAudio* audio = set.audio;
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

template <typename Found>
void SoundControls::expand(const Set& set, const TargetRange& range, Found found) {
	const bool kitLevel = set.engine == Engine::KitLevel;
	const Sound* sound = set.sound;
	auto patched = [&](int32_t param) { found(Collection::Patched, static_cast<uint8_t>(param), PatchSource::NONE); };
	auto unpatched = [&](int32_t param) {
		found(Collection::Unpatched, static_cast<uint8_t>(param), PatchSource::NONE);
	};
	switch (range.target) {
	case Target::LpfFrequency:
		kitLevel ? unpatched(params::UNPATCHED_LPF_FREQ) : patched(params::LOCAL_LPF_FREQ);
		break;
	case Target::LpfMorph:
		patched(params::LOCAL_LPF_MORPH);
		break;
	case Target::HpfFrequency:
		kitLevel ? unpatched(params::UNPATCHED_HPF_FREQ) : patched(params::LOCAL_HPF_FREQ);
		break;
	case Target::WaveIndex:
		if (sound->sources[0].oscType == OscType::WAVETABLE) {
			patched(params::LOCAL_OSC_A_WAVE_INDEX);
		}
		if (sound->sources[1].oscType == OscType::WAVETABLE) {
			patched(params::LOCAL_OSC_B_WAVE_INDEX);
		}
		break;
	case Target::PulseWidth:
		patched(params::LOCAL_OSC_A_PHASE_WIDTH);
		patched(params::LOCAL_OSC_B_PHASE_WIDTH);
		break;
	case Target::Modulator1Level:
		patched(params::LOCAL_MODULATOR_0_VOLUME);
		break;
	case Target::Modulator2Level:
		patched(params::LOCAL_MODULATOR_1_VOLUME);
		break;
	case Target::ModulatorFeedback:
		if (uses(set, Needs::Modulator1InUse)) {
			patched(params::LOCAL_MODULATOR_0_FEEDBACK);
		}
		if (uses(set, Needs::Modulator2InUse)) {
			patched(params::LOCAL_MODULATOR_1_FEEDBACK);
		}
		break;
	case Target::CarrierFeedback:
		patched(params::LOCAL_CARRIER_0_FEEDBACK);
		patched(params::LOCAL_CARRIER_1_FEEDBACK);
		break;
	case Target::Fold:
		patched(params::LOCAL_FOLD);
		break;
	case Target::Env1Release:
		patched(params::LOCAL_ENV_0_RELEASE);
		break;
	case Target::Env1Decay:
		patched(params::LOCAL_ENV_0_DECAY);
		break;
	case Target::Env1Sustain:
		patched(params::LOCAL_ENV_0_SUSTAIN);
		break;
	case Target::Env2Decay:
		patched(params::LOCAL_ENV_1_DECAY);
		break;
	case Target::ReverbSend:
		kitLevel ? unpatched(params::UNPATCHED_REVERB_SEND_AMOUNT) : patched(params::GLOBAL_REVERB_AMOUNT);
		break;
	case Target::DelayFeedback:
		patched(params::GLOBAL_DELAY_FEEDBACK);
		break;
	case Target::ModFxDepth:
		kitLevel ? unpatched(params::UNPATCHED_MOD_FX_DEPTH) : patched(params::GLOBAL_MOD_FX_DEPTH);
		break;
	case Target::LfoRate: {
		// Only LFOs patched to something: an unused LFO's rate is never heard.
		PatchCableSet* cables = set.paramManager->getPatchCableSet();
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
				patched(lfo.param);
			}
		}
		break;
	}
	case Target::LfoCables: {
		// Existing cables from an LFO to a param only: Wobble never adds one (spec §11.2), and never touches level or
		// pan (spec §11.8).
		PatchCableSet* cables = set.paramManager->getPatchCableSet();
		for (int32_t c = 0; c < cables->numPatchCables; c++) {
			const PatchCable& cable = cables->patchCables[c];
			if (isLfo(cable.from) && cable.destinationParamDescriptor.isJustAParam()
			    && !isLevelOrPan(cable.destinationParamDescriptor.getJustTheParam())) {
				found(Collection::Cables, static_cast<uint8_t>(cable.destinationParamDescriptor.getJustTheParam()),
				      cable.from);
			}
		}
		break;
	}
	}
}

void SoundControls::bindParam(const Set& set, const TargetRange& range, Collection collection, uint8_t param,
                              PatchSource source) {
	if (boundCount_ >= kMaxControlSets * kMaxParamsPerSet) {
		return; // never: kMaxParamsPerSet counts the most each table entry can expand into
	}
	Bound bound;
	bound.range = range;
	bound.collection = collection;
	bound.param = param;
	bound.source = source;
	AutoParam* autoParam = paramOf(set, bound);
	if (autoParam == nullptr || autoParam->isAutomated()) {
		return; // Perform's clips hold no automation (spec §11.7); a param that has some is left to it
	}
	bound.home = autoParam->getCurrentValue();
	bound_[boundCount_++] = bound;
}

int32_t SoundControls::slotOf(const Set& set, const Bound& bound) {
	if (bound.collection != Collection::Cables) {
		return bound.param;
	}
	const uint8_t c = set.paramManager->getPatchCableSet()->getPatchCableIndex(bound.source, justParam(bound.param));
	return c == 255 ? -1 : c;
}

AutoParam* SoundControls::paramOf(const Set& set, const Bound& bound) {
	return paramAt(set, bound, slotOf(set, bound));
}

AutoParam* SoundControls::paramAt(const Set& set, const Bound& bound, int32_t slot) {
	ParamManager& manager = *set.paramManager;
	if (slot < 0) {
		return nullptr; // a cable the player deleted
	}
	switch (bound.collection) {
	case Collection::Unpatched:
		return &manager.getUnpatchedParamSet()->params[slot];
	case Collection::Patched:
		return &manager.getPatchedParamSet()->params[slot];
	case Collection::Cables:
		return &manager.getPatchCableSet()->patchCables[slot].param;
	}
	return nullptr;
}

ModelStackWithThreeMainThings* SoundControls::modelStack(const Set& set, char* memory) {
	ModelStackWithTimelineCounter* withClip =
	    setupModelStackWithSong(memory, currentSong)->addTimelineCounter(set.clip);
	if (set.noteRow != nullptr) {
		return withClip->addNoteRow(set.row, set.noteRow)->addOtherTwoThings(set.audio, set.paramManager);
	}
	return withClip->addOtherTwoThingsButNoNoteRow(set.audio, set.paramManager);
}

int32_t SoundControls::idOf(const Bound& bound) {
	return bound.collection == Collection::Cables ? PatchCableSet::getParamId(justParam(bound.param), bound.source)
	                                              : bound.param;
}

ModelStackWithAutoParam* SoundControls::paramStack(const Set& set, const Bound& bound, AutoParam* param, char* memory) {
	ParamCollectionSummary* summary = &set.paramManager->summaries[static_cast<int32_t>(bound.collection)];
	return modelStack(set, memory)->addParam(summary->paramCollection, summary, idOf(bound), param);
}

void SoundControls::snap(const Set& set, const Bound& bound, int32_t value) {
	AutoParam* param = paramOf(set, bound);
	if (param == nullptr) {
		return; // a cable the player deleted
	}
	param->resetInterpolationIncrement();
	char memory[MODEL_STACK_MAX_SIZE];
	param->setCurrentValueWithNoReversionOrRecording(paramStack(set, bound, param, memory), value);
}

void SoundControls::armRamp(const Set& set, const Bound& bound, int32_t value, int32_t rampTicks) {
	const int32_t slot = slotOf(set, bound); // one cable lookup per ramp
	AutoParam* param = paramAt(set, bound, slot);
	if (param != nullptr && param->setLiveRamp(value, rampTicks)) {
		ParamCollectionSummary& summary = set.paramManager->summaries[static_cast<int32_t>(bound.collection)];
		summary.whichParamsAreInterpolating[slot >> 5] |= 1U << (slot & 31);
	}
}

void SoundControls::apply(const Event& event, int32_t rampTicks) {
	applyIn(sets_, setCount_, bound_, event, rampTicks);
}

void SoundControls::keepOutgoingSpace() {
	forgetOutgoing();
	constexpr auto kSpace = static_cast<int32_t>(SoundControl::Space);
	for (int32_t i = 0; i < setCount_; i++) {
		Set set = sets_[i];
		const int32_t from = set.first[kSpace];
		const int32_t count = std::min<int32_t>(set.first[kSpace + 1] - from, kMaxSpaceParams);
		const auto first = static_cast<uint16_t>(outgoingBoundCount_);
		std::copy_n(bound_ + from, count, outgoingBound_ + first);
		outgoingBoundCount_ += count;
		// Space's params only: every other control's range is empty.
		for (int32_t c = 0; c <= kNumSoundControls; c++) {
			set.first[c] = static_cast<uint16_t>(c <= kSpace ? first : first + count);
		}
		outgoingSets_[outgoingSetCount_++] = set;
	}
}

void SoundControls::applyOutgoing(const Event& event, int32_t rampTicks) {
	if (event.control == SoundControl::Space) {
		applyIn(outgoingSets_, outgoingSetCount_, outgoingBound_, event, rampTicks);
	}
}

void SoundControls::applyIn(Set* sets, int32_t count, const Bound* params, const Event& event, int32_t rampTicks) {
	for (int32_t i = 0; i < count; i++) {
		Set& set = sets[i];
		if (set.part != event.part || set.lane != event.lane) {
			continue;
		}
		const auto c = static_cast<int32_t>(event.control);
		set.offset[c] = event.offset;
		for (int32_t b = set.first[c]; b < set.first[c + 1]; b++) {
			const Bound& bound = params[b];
			const int32_t value = perform::targetValue(bound.range, bound.home, event.offset);
#if ENABLE_SYSEX_TEST
			paramsMoved_++;
#endif
			if (event.snap) {
				snap(set, bound, value);
			}
			else {
				armRamp(set, bound, value, rampTicks);
			}
		}
		return;
	}
}

std::optional<SoundControls::Match> SoundControls::find(const ModelStackWithAutoParam& stack) {
	for (int32_t i = 0; i < setCount_; i++) {
		Set& s = sets_[i];
		if (s.paramManager != stack.paramManager) {
			continue;
		}
		// By collection and id, never by AutoParam: an edit that sets a cable to 0 deletes it before F3 runs, and the
		// delete moves another cable into its slot.
		for (int32_t c = 0; c < kNumSoundControls; c++) {
			for (int32_t b = s.first[c]; b < s.first[c + 1]; b++) {
				const Bound& at = bound_[b];
				if (stack.paramCollection
				        == s.paramManager->summaries[static_cast<int32_t>(at.collection)].paramCollection
				    && stack.paramId == idOf(at)) {
					return Match{s, bound_[b], c};
				}
			}
		}
	}
	return std::nullopt;
}

std::optional<SoundControls::HomeEdit> SoundControls::playerEdited(const ModelStackWithAutoParam& stack,
                                                                   int32_t value) {
	const std::optional<Match> match = find(stack);
	if (!match) {
		TEST_EVENT("gen_edit control=none");
		return std::nullopt;
	}
	Set& set = match->set;
	Bound& bound = match->bound;
	const int32_t c = match->control;
	if (paramOf(set, bound) == nullptr) {
		// The edit set the cable to 0 and stock deleted it: no home to move, and nothing to restore at the stop.
		TEST_EVENT("gen_edit part=%u control=%s gone", static_cast<unsigned>(set.part),
		           UpperName(static_cast<SoundControl>(c)).text);
		return std::nullopt;
	}
	if (!settingHome_) {
		// The edit stopped any ramp, so the param stays where it was turned until the control's next change.
		bound.home = perform::homeFor(bound.range, value, set.offset[c]);
	}
	TEST_EVENT("gen_edit part=%u lane=%d control=%s", static_cast<unsigned>(set.part),
	           set.lane == Lane::None ? -1 : static_cast<int>(set.lane), UpperName(static_cast<SoundControl>(c)).text);
	return HomeEdit{set.part, set.lane, paramKey(bound), bound.home};
}

uint16_t SoundControls::paramKey(const Bound& bound) {
	static_assert(static_cast<int32_t>(PatchSource::NONE) < 32, "a source fits the key's 5 bits");
	return static_cast<uint16_t>(static_cast<int32_t>(bound.collection) << 13 | static_cast<int32_t>(bound.source) << 8
	                             | bound.param);
}

void SoundControls::setHome(const ModelStackWithAutoParam& stack, int32_t home) {
	int32_t value = home;
	if (const std::optional<Match> match = find(stack)) {
		match->bound.home = home;
		value = perform::targetValue(match->bound.range, home, match->set.offset[match->control]);
	}
	settingHome_ = true;
	stack.autoParam->setCurrentValueInResponseToUserInput(value, &stack);
	settingHome_ = false;
}

ModelStackWithAutoParam* SoundControls::leadParam(const Roster& roster, const Genre& genre, int32_t part,
                                                  SoundControl control, char* memory) const {
	Set set;
	if (part < 0 || part >= partCount(genre) || genre.parts[part].role == Role::FX
	    || !makeSet(roster, genre, part, 0, set)) {
		return nullptr;
	}
	std::optional<Bound> lead;
	for (const TargetRange& range : perform::controlTargets(control, set.engine)) {
		if (!uses(set, range.needs)) {
			continue;
		}
		expand(set, range, [&](Collection collection, uint8_t param, PatchSource source) {
			Bound bound;
			bound.collection = collection;
			bound.param = param;
			bound.source = source;
			AutoParam* autoParam = lead ? nullptr : paramOf(set, bound);
			if (autoParam != nullptr && !autoParam->isAutomated()) {
				lead = bound;
			}
		});
		if (lead) {
			return paramStack(set, *lead, paramOf(set, *lead), memory);
		}
	}
	return nullptr;
}

int32_t SoundControls::homeOf(const ModelStackWithAutoParam& stack) {
	const std::optional<Match> match = find(stack);
	return match ? match->bound.home : stack.autoParam->getCurrentValue();
}

#if ENABLE_SYSEX_TEST
void SoundControls::stress(bool up, int32_t rampTicks) {
	for (int32_t i = 0; i < setCount_; i++) {
		const Set& set = sets_[i];
		for (int32_t b = set.first[0]; b < set.first[kNumSoundControls]; b++) {
			paramsMoved_++;
			armRamp(set, bound_[b], perform::targetValue(bound_[b].range, bound_[b].home, up ? 50 : -50), rampTicks);
		}
	}
}
#endif

void SoundControls::restoreHomes() {
	for (int32_t i = 0; i < setCount_; i++) {
		const Set& set = sets_[i];
		for (int32_t b = set.first[0]; b < set.first[kNumSoundControls]; b++) {
			const Bound& bound = bound_[b];
			AutoParam* param = paramOf(set, bound);
			if (param == nullptr) {
				continue; // a cable the player deleted
			}
			param->resetInterpolationIncrement(); // a ramp still running mustn't carry on
			if (param->getCurrentValue() != bound.home) {
				snap(set, bound, bound.home);
			}
		}
	}
	setCount_ = 0;
	boundCount_ = 0;
}

} // namespace deluge::generative::firmware
