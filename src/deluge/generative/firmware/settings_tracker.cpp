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

#include "generative/firmware/settings_tracker.h"
#include "definitions.h"
#include "dsp/reverb/reverb.hpp"
#include "generative/core/genre.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/arpeggiator.h"
#include "modulation/params/param.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "processing/source.h"
#include "storage/audio/audio_file.h"
#include "storage/audio/audio_file_holder.h"
#include "storage/multi_range/multi_range.h"
#include "storage/multi_range/multisample_range.h"
#include "util/functions.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SettingsTracker settingsTracker;

namespace {

constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;
constexpr double kReverbScale = 2147483648.0; // matches Song::writeToFile() / readFromFile()

uint32_t hashPath(const char* path, uint16_t& outLen) {
	if (path == nullptr || path[0] == 0) {
		outLen = 0;
		return 0;
	}
	uint32_t h = kFnvOffset;
	uint16_t len = 0;
	while (len < kMaxSoundPath - 1 && path[len] != 0) {
		h ^= static_cast<uint8_t>(path[len++]);
		h *= kFnvPrime;
	}
	outLen = len;
	return h == 0 ? 1 : h; // non-zero when non-empty
}

int32_t floatToQ31(float value) {
	const double scaled = std::clamp<double>(value, 0.0, 1.0) * kReverbScale;
	return static_cast<int32_t>(std::min<double>(scaled, 2147483647.0));
}

float q31ToFloat(int32_t value) {
	return static_cast<float>(static_cast<double>(static_cast<uint32_t>(value)) / kReverbScale);
}

int32_t packPitch(int32_t transpose, int8_t cents) {
	return (transpose & 0xFFFF) | (static_cast<int32_t>(static_cast<uint8_t>(cents)) << 16);
}

int16_t unpackTranspose(int32_t packed) {
	return static_cast<int16_t>(packed & 0xFFFF);
}

int8_t unpackCents(int32_t packed) {
	return static_cast<int8_t>((packed >> 16) & 0xFF);
}

int32_t packLfo(const LFOConfig& cfg) {
	return static_cast<int32_t>(cfg.waveType) | (static_cast<int32_t>(cfg.syncLevel) << 8)
	       | (static_cast<int32_t>(cfg.syncType) << 16);
}

void unpackLfo(int32_t packed, LFOConfig& cfg) {
	cfg.waveType = static_cast<LFOType>(packed & 0xFF);
	cfg.syncLevel = static_cast<SyncLevel>((packed >> 8) & 0xFF);
	cfg.syncType = static_cast<SyncType>((packed >> 16) & 0xFF);
}

int32_t packSampleFlags(const Source& source) {
	return static_cast<int32_t>(source.repeatMode) | (source.sampleControls.reversed ? (1 << 4) : 0)
	       | (source.sampleControls.pitchAndSpeedAreIndependent ? (1 << 5) : 0)
	       | (static_cast<int32_t>(source.sampleControls.interpolationMode) << 6)
	       | (static_cast<int32_t>(static_cast<uint8_t>(source.timeStretchAmount)) << 8);
}

void unpackSampleFlags(int32_t packed, Source& source) {
	source.repeatMode = static_cast<SampleRepeatMode>(packed & 0x0F);
	source.setReversed((packed & (1 << 4)) != 0);
	source.sampleControls.pitchAndSpeedAreIndependent = (packed & (1 << 5)) != 0;
	source.sampleControls.interpolationMode = static_cast<InterpolationMode>((packed >> 6) & 0x03);
	source.timeStretchAmount = static_cast<int8_t>((packed >> 8) & 0xFF);
}

const char* sourcePath(Source& source) {
	if ((source.oscType != OscType::SAMPLE && source.oscType != OscType::WAVETABLE)
	    || source.ranges.getNumElements() == 0) {
		return "";
	}
	auto* range = static_cast<MultiRange*>(source.ranges.getElementAddress(0));
	if (range == nullptr) {
		return "";
	}
	AudioFileHolder* holder = range->getAudioFileHolder();
	if (holder == nullptr) {
		return "";
	}
	return holder->audioFile != nullptr ? holder->audioFile->filePath.get() : holder->filePath.get();
}

int32_t readSourcePitch(Source& source) {
	if (source.oscType == OscType::SAMPLE && source.ranges.getNumElements() > 0) {
		auto* range = static_cast<MultisampleRange*>(source.ranges.getElementAddress(0));
		if (range != nullptr) {
			return packPitch(range->sampleHolder.transpose, range->sampleHolder.cents);
		}
	}
	return packPitch(source.transpose, source.cents);
}

/// Resolved pointers for a target Output (Song, Synth, Kit global FX, or Kit row SoundDrum).
struct TargetRefs {
	ModControllableAudio* mod = nullptr;
	Sound* sound = nullptr;
	ArpeggiatorSettings* arp = nullptr;
	InstrumentClip* clip = nullptr;
	NoteRow* row = nullptr;
	int32_t rowIdx = -1;
	ParamManager* manager = nullptr;
	bool isSong = false;
};

TargetRefs resolveTarget(uint8_t output, Lane lane, const Roster& roster, const Genre& genre, Song* song) {
	TargetRefs refs;
	if (output == kSongParams) {
		if (song != nullptr) {
			refs.mod = &song->globalEffectable;
			refs.manager = &song->paramManager;
			refs.isSong = true;
		}
		return refs;
	}
	if (output >= partCount(genre)) {
		return refs;
	}
	const PartOutput& part = roster.parts[output];
	if (part.output == nullptr || part.clip == nullptr) {
		return refs;
	}
	refs.clip = part.clip;
	if (lane == Lane::None) {
		refs.arp = &part.clip->arpSettings;
		refs.manager = &part.clip->paramManager;
		if (part.output->type == OutputType::SYNTH) {
			auto* synth = static_cast<SoundInstrument*>(part.output);
			refs.sound = synth;
			refs.mod = synth;
		}
		else if (part.output->type == OutputType::KIT) {
			refs.mod = static_cast<Kit*>(part.output);
		}
		return refs;
	}
	Drum* drum = part.drums[laneRow(lane)];
	if (drum == nullptr || drum->type != DrumType::SOUND) {
		return refs;
	}
	auto* soundDrum = static_cast<SoundDrum*>(drum);
	refs.sound = soundDrum;
	refs.mod = soundDrum;
	refs.arp = &soundDrum->arpSettings;
	refs.row = part.clip->getNoteRowForDrum(drum, &refs.rowIdx);
	refs.manager = refs.row != nullptr ? &refs.row->paramManager : nullptr;
	return refs;
}

/// Reads the live scalar settings of `refs` into `out` and (for Sound targets) osc 0/1 file paths into `paths`.
void readTarget(const TargetRefs& refs, Song* song, int32_t (&out)[kNumOutputSettings], const char* (&paths)[2]) {
	paths[0] = "";
	paths[1] = "";
	auto set = [&](OutputSetting id, int32_t v) { out[static_cast<uint16_t>(id)] = v; };
	if (refs.mod != nullptr) {
		ModControllableAudio& m = *refs.mod;
		set(OutputSetting::LpfMode, static_cast<int32_t>(m.lpfMode));
		set(OutputSetting::HpfMode, static_cast<int32_t>(m.hpfMode));
		set(OutputSetting::FilterRoute, static_cast<int32_t>(m.filterRoute));
		set(OutputSetting::ModFxType, static_cast<int32_t>(m.modFXType_));
		set(OutputSetting::ClippingAmount, m.clippingAmount);
		set(OutputSetting::DelayFlags, (m.delay.pingPong ? 1 : 0) | (m.delay.analog ? 2 : 0)
		                                   | (static_cast<int32_t>(m.delay.syncLevel) << 8)
		                                   | (static_cast<int32_t>(m.delay.syncType) << 16));
		set(OutputSetting::SidechainAttack, m.sidechain.attack);
		set(OutputSetting::SidechainRelease, m.sidechain.release);
		set(OutputSetting::SidechainSync,
		    static_cast<int32_t>(m.sidechain.syncLevel) | (static_cast<int32_t>(m.sidechain.syncType) << 8));
		set(OutputSetting::CompAttack, m.compressor.getAttack());
		set(OutputSetting::CompRelease, m.compressor.getRelease());
		set(OutputSetting::CompThreshold, m.compressor.getThreshold());
		set(OutputSetting::CompRatio, m.compressor.getRatio());
		set(OutputSetting::CompSidechain, m.compressor.getSidechain());
		set(OutputSetting::CompBlend, m.compressor.getBlend());
		set(OutputSetting::StutterFlags, (m.stutterConfig.useSongStutter ? 1 : 0) | (m.stutterConfig.quantized ? 2 : 0)
		                                     | (m.stutterConfig.reversed ? 4 : 0) | (m.stutterConfig.pingPong ? 8 : 0));
	}
	if (refs.sound != nullptr) {
		Sound& s = *refs.sound;
		set(OutputSetting::SynthMode, static_cast<int32_t>(s.synthMode));
		set(OutputSetting::Osc0Type, static_cast<int32_t>(s.sources[0].oscType));
		set(OutputSetting::Osc1Type, static_cast<int32_t>(s.sources[1].oscType));
		set(OutputSetting::Polyphonic, static_cast<int32_t>(s.polyphonic) | (static_cast<int32_t>(s.maxVoiceCount) << 8)
		                                   | (static_cast<int32_t>(s.voicePriority) << 16));
		set(OutputSetting::TransposeCents, packPitch(s.transpose, s.cents));
		set(OutputSetting::SidechainSend, s.sideChainSendLevel);
		set(OutputSetting::OscSync, s.oscillatorSync ? 1 : 0);
		set(OutputSetting::Osc0RetrigPhase, static_cast<int32_t>(s.oscRetriggerPhase[0]));
		set(OutputSetting::Osc1RetrigPhase, static_cast<int32_t>(s.oscRetriggerPhase[1]));
		set(OutputSetting::Unison, static_cast<int32_t>(static_cast<uint8_t>(s.numUnison))
		                               | (static_cast<int32_t>(static_cast<uint8_t>(s.unisonDetune)) << 8)
		                               | (static_cast<int32_t>(static_cast<uint8_t>(s.unisonStereoSpread)) << 16));
		set(OutputSetting::Lfo0, packLfo(s.lfoConfig[LFO1_ID]));
		set(OutputSetting::Lfo1, packLfo(s.lfoConfig[LFO2_ID]));
		set(OutputSetting::Lfo2, packLfo(s.lfoConfig[LFO3_ID]));
		set(OutputSetting::Lfo3, packLfo(s.lfoConfig[LFO4_ID]));
		set(OutputSetting::Mod0Pitch, packPitch(s.modulatorTranspose[0], s.modulatorCents[0]));
		set(OutputSetting::Mod0RetrigPhase, static_cast<int32_t>(s.modulatorRetriggerPhase[0]));
		set(OutputSetting::Mod1Pitch,
		    packPitch(s.modulatorTranspose[1], s.modulatorCents[1]) | (s.modulator1ToModulator0 ? (1 << 24) : 0));
		set(OutputSetting::Mod1RetrigPhase, static_cast<int32_t>(s.modulatorRetriggerPhase[1]));
		set(OutputSetting::Osc0Pitch, readSourcePitch(s.sources[0]));
		set(OutputSetting::Osc0SampleFlags, packSampleFlags(s.sources[0]));
		set(OutputSetting::Osc1Pitch, readSourcePitch(s.sources[1]));
		set(OutputSetting::Osc1SampleFlags, packSampleFlags(s.sources[1]));
		paths[0] = sourcePath(s.sources[0]);
		paths[1] = sourcePath(s.sources[1]);
	}
	if (refs.arp != nullptr) {
		const ArpeggiatorSettings& a = *refs.arp;
		set(OutputSetting::ArpMain, static_cast<int32_t>(a.mode) | (static_cast<int32_t>(a.preset) << 4)
		                                | (static_cast<int32_t>(a.octaveMode) << 8)
		                                | (static_cast<int32_t>(a.noteMode) << 12) | (a.includeInKitArp ? (1 << 16) : 0)
		                                | (a.randomizerLock ? (1 << 17) : 0)
		                                | (static_cast<int32_t>(a.mpeVelocity) << 18));
		set(OutputSetting::ArpCounts, static_cast<int32_t>(a.numOctaves) | (static_cast<int32_t>(a.numStepRepeats) << 8)
		                                  | (static_cast<int32_t>(a.chordTypeIndex) << 16));
		set(OutputSetting::ArpSync, static_cast<int32_t>(a.syncLevel) | (static_cast<int32_t>(a.syncType) << 8));
	}
	if (refs.isSong && song != nullptr) {
		// In Perform, currentSong == song, so live Reverb values live in AudioEngine::reverb.
		set(OutputSetting::ReverbModel, static_cast<int32_t>(AudioEngine::reverb.getModel()));
		set(OutputSetting::ReverbRoomSize, floatToQ31(AudioEngine::reverb.getRoomSize()));
		set(OutputSetting::ReverbDamping, floatToQ31(AudioEngine::reverb.getDamping()));
		set(OutputSetting::ReverbWidth, floatToQ31(AudioEngine::reverb.getWidth()));
		set(OutputSetting::ReverbHpf, floatToQ31(AudioEngine::reverb.getHPF()));
		set(OutputSetting::ReverbLpf, floatToQ31(AudioEngine::reverb.getLPF()));
		set(OutputSetting::ReverbPan, AudioEngine::reverbPan);
		set(OutputSetting::ReverbSidechainVolume, AudioEngine::reverbSidechainVolume);
		set(OutputSetting::ReverbSidechainShape, AudioEngine::reverbSidechainShape);
		// While reverbSidechainVolume < 0 (AUTO), AudioEngine::updateReverbSidechain() overwrites
		// reverbSidechain.{attack,release,syncLevel} from whichever playing sound sends the most reverb.
		// Track the song's stored fields in AUTO mode so audio playback never logs spurious edits.
		const bool manualSidechain = AudioEngine::reverbSidechainVolume >= 0;
		set(OutputSetting::ReverbSidechainAttack,
		    manualSidechain ? AudioEngine::reverbSidechain.attack : song->reverbSidechainAttack);
		set(OutputSetting::ReverbSidechainRelease,
		    manualSidechain ? AudioEngine::reverbSidechain.release : song->reverbSidechainRelease);
		set(OutputSetting::ReverbSidechainSync,
		    manualSidechain ? (static_cast<int32_t>(AudioEngine::reverbSidechain.syncLevel)
		                       | (static_cast<int32_t>(AudioEngine::reverbSidechain.syncType) << 8))
		                    : static_cast<int32_t>(song->reverbSidechainSync));
	}
}

/// Returns a bitmask of which scalar settings are valid on `refs` (avoids comparing irrelevant fields).
uint64_t validScalarMask(const TargetRefs& refs) {
	uint64_t mask = 0;
	if (refs.mod != nullptr) {
		for (auto i = static_cast<uint16_t>(OutputSetting::LpfMode);
		     i <= static_cast<uint16_t>(OutputSetting::StutterFlags); i++) {
			mask |= uint64_t{1} << i;
		}
	}
	if (refs.sound != nullptr) {
		mask |= settingBit(OutputSetting::SynthMode) | settingBit(OutputSetting::Osc0Type)
		        | settingBit(OutputSetting::Osc1Type);
		for (auto i = static_cast<uint16_t>(OutputSetting::Polyphonic);
		     i <= static_cast<uint16_t>(OutputSetting::Osc1SampleFlags); i++) {
			mask |= uint64_t{1} << i;
		}
	}
	if (refs.arp != nullptr) {
		mask |= settingBit(OutputSetting::ArpMain) | settingBit(OutputSetting::ArpCounts)
		        | settingBit(OutputSetting::ArpSync);
	}
	if (refs.isSong) {
		for (auto i = static_cast<uint16_t>(OutputSetting::ReverbModel);
		     i <= static_cast<uint16_t>(OutputSetting::ReverbSidechainSync); i++) {
			mask |= uint64_t{1} << i;
		}
	}
	return mask;
}

/// Calls Source::setOscType() without clobbering SoundEditor's multi-range pointers.
void setOscTypeSafe(Sound& sound, int32_t osc, OscType newType) {
	const int32_t savedIdx = soundEditor.currentMultiRangeIndex;
	MultiRange* savedRange = soundEditor.currentMultiRange;
	sound.killAllVoices();
	sound.sources[osc].setOscType(newType);
	soundEditor.currentMultiRangeIndex = savedIdx;
	soundEditor.currentMultiRange = savedRange;
}

ModelStackWithSoundFlags* soundModelStack(char (&mem)[MODEL_STACK_MAX_SIZE], const TargetRefs& refs, Song& song) {
	if (refs.sound == nullptr || refs.clip == nullptr || refs.manager == nullptr) {
		return nullptr;
	}
	ModelStackWithTimelineCounter* clipStack = setupModelStackWithSong(mem, &song)->addTimelineCounter(refs.clip);
	if (refs.row != nullptr) {
		return clipStack->addNoteRow(refs.rowIdx, refs.row)
		    ->addOtherTwoThings(refs.sound, refs.manager)
		    ->addSoundFlags();
	}
	return clipStack->addOtherTwoThingsButNoNoteRow(refs.sound, refs.manager)->addSoundFlags();
}

/// Returns the setting bitmask present in `piece`'s kSettingsTag (0x92) blocks for `(output, lane)`.
uint64_t headerMaskFor(const SteeringLog::Piece& piece, uint8_t output, Lane lane) {
	uint64_t mask = 0;
	piece.forEachSettings([&](const void* data, int32_t length) {
		SettingsBlockHeader hdr{};
		uint64_t blockMask = 0;
		if (!readSettingsBlock(
		        data, length, hdr,
		        [&](OutputSetting s, int32_t) {
			        if (static_cast<int32_t>(s) < kNumOutputSettings) {
				        blockMask |= settingBit(s);
			        }
		        },
		        [&](OutputSetting s, const char*, int32_t) {
			        if (static_cast<int32_t>(s) < kNumOutputSettings) {
				        blockMask |= settingBit(s);
			        }
		        })) {
			return;
		}
		if (hdr.output == output && hdr.lane == lane) {
			mask |= blockMask;
		}
	});
	return mask;
}

int32_t slotIndexFor(uint8_t output, Lane lane) {
	if (output == kSongParams) {
		return 0;
	}
	if (lane == Lane::None) {
		return output < kMaxParts ? 1 + output : -1;
	}
	const auto l = static_cast<int32_t>(lane);
	return l >= 0 && l < kNumLanes ? 1 + kMaxParts + l : -1;
}

} // namespace

bool applyOutputSetting(OutputSetting setting, int32_t value, uint8_t target, Lane lane, const Roster& roster,
                        const Genre& genre, Song& song, bool songIsCurrent) {
	const TargetRefs refs = resolveTarget(target, lane, roster, genre, &song);
	if (refs.mod == nullptr) {
		return false;
	}
	switch (setting) {
	case OutputSetting::SynthMode:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->setSynthMode(static_cast<SynthMode>(value), &song);
		return true;
	case OutputSetting::Osc0Type:
	case OutputSetting::Osc1Type: {
		if (refs.sound == nullptr) {
			return false;
		}
		const int32_t osc = setting == OutputSetting::Osc0Type ? 0 : 1;
		setOscTypeSafe(*refs.sound, osc, static_cast<OscType>(value));
		return true;
	}
	case OutputSetting::LpfMode:
		refs.mod->lpfMode = static_cast<FilterMode>(value);
		return true;
	case OutputSetting::HpfMode:
		refs.mod->hpfMode = static_cast<FilterMode>(value);
		return true;
	case OutputSetting::FilterRoute:
		refs.mod->filterRoute = static_cast<FilterRoute>(value);
		return true;
	case OutputSetting::ModFxType:
		refs.mod->setModFXType(static_cast<ModFXType>(value));
		return true;
	case OutputSetting::ClippingAmount:
		refs.mod->clippingAmount = static_cast<uint8_t>(value);
		return true;
	case OutputSetting::DelayFlags:
		refs.mod->delay.pingPong = (value & 1) != 0;
		refs.mod->delay.analog = (value & 2) != 0;
		refs.mod->delay.syncLevel = static_cast<SyncLevel>((value >> 8) & 0xFF);
		refs.mod->delay.syncType = static_cast<SyncType>((value >> 16) & 0xFF);
		return true;
	case OutputSetting::SidechainAttack:
		refs.mod->sidechain.attack = value;
		return true;
	case OutputSetting::SidechainRelease:
		refs.mod->sidechain.release = value;
		return true;
	case OutputSetting::SidechainSync:
		refs.mod->sidechain.syncLevel = static_cast<SyncLevel>(value & 0xFF);
		refs.mod->sidechain.syncType = static_cast<SyncType>((value >> 8) & 0xFF);
		return true;
	case OutputSetting::CompAttack:
		refs.mod->compressor.setAttack(value);
		return true;
	case OutputSetting::CompRelease:
		refs.mod->compressor.setRelease(value);
		return true;
	case OutputSetting::CompThreshold:
		refs.mod->compressor.setThreshold(value);
		return true;
	case OutputSetting::CompRatio:
		refs.mod->compressor.setRatio(value);
		return true;
	case OutputSetting::CompSidechain:
		refs.mod->compressor.setSidechain(value);
		return true;
	case OutputSetting::CompBlend:
		refs.mod->compressor.setBlend(value);
		return true;
	case OutputSetting::StutterFlags:
		refs.mod->stutterConfig.useSongStutter = (value & 1) != 0;
		refs.mod->stutterConfig.quantized = (value & 2) != 0;
		refs.mod->stutterConfig.reversed = (value & 4) != 0;
		refs.mod->stutterConfig.pingPong = (value & 8) != 0;
		return true;
	case OutputSetting::Polyphonic:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->polyphonic = static_cast<PolyphonyMode>(value & 0xFF);
		refs.sound->maxVoiceCount = static_cast<uint8_t>((value >> 8) & 0xFF);
		refs.sound->voicePriority = static_cast<VoicePriority>((value >> 16) & 0xFF);
		return true;
	case OutputSetting::TransposeCents:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->transpose = unpackTranspose(value);
		refs.sound->set_cents(unpackCents(value));
		return true;
	case OutputSetting::SidechainSend:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->sideChainSendLevel = value;
		return true;
	case OutputSetting::OscSync:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->oscillatorSync = value != 0;
		return true;
	case OutputSetting::Osc0RetrigPhase:
	case OutputSetting::Osc1RetrigPhase:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->oscRetriggerPhase[setting == OutputSetting::Osc0RetrigPhase ? 0 : 1] = static_cast<uint32_t>(value);
		return true;
	case OutputSetting::Unison: {
		if (refs.sound == nullptr) {
			return false;
		}
		char mem[MODEL_STACK_MAX_SIZE];
		if (ModelStackWithSoundFlags* stack = soundModelStack(mem, refs, song)) {
			refs.sound->setNumUnison(static_cast<int8_t>(value & 0xFF), stack);
			refs.sound->setUnisonDetune(static_cast<int8_t>((value >> 8) & 0xFF), stack);
		}
		refs.sound->setUnisonStereoSpread(static_cast<int8_t>((value >> 16) & 0xFF));
		return true;
	}
	case OutputSetting::Lfo0:
	case OutputSetting::Lfo1:
	case OutputSetting::Lfo2:
	case OutputSetting::Lfo3: {
		if (refs.sound == nullptr) {
			return false;
		}
		const int32_t idx = static_cast<int32_t>(setting) - static_cast<int32_t>(OutputSetting::Lfo0);
		unpackLfo(value, refs.sound->lfoConfig[idx]);
		return true;
	}
	case OutputSetting::Mod0Pitch:
	case OutputSetting::Mod1Pitch: {
		if (refs.sound == nullptr) {
			return false;
		}
		const int32_t m = setting == OutputSetting::Mod0Pitch ? 0 : 1;
		char mem[MODEL_STACK_MAX_SIZE];
		if (ModelStackWithSoundFlags* stack = soundModelStack(mem, refs, song)) {
			refs.sound->setModulatorTranspose(m, unpackTranspose(value), stack);
			refs.sound->setModulatorCents(m, unpackCents(value), stack);
		}
		if (m == 1) {
			refs.sound->modulator1ToModulator0 = (value & (1 << 24)) != 0;
		}
		return true;
	}
	case OutputSetting::Mod0RetrigPhase:
	case OutputSetting::Mod1RetrigPhase:
		if (refs.sound == nullptr) {
			return false;
		}
		refs.sound->modulatorRetriggerPhase[setting == OutputSetting::Mod0RetrigPhase ? 0 : 1] =
		    static_cast<uint32_t>(value);
		return true;
	case OutputSetting::Osc0Pitch:
	case OutputSetting::Osc1Pitch: {
		if (refs.sound == nullptr) {
			return false;
		}
		Source& src = refs.sound->sources[setting == OutputSetting::Osc0Pitch ? 0 : 1];
		const int16_t transpose = unpackTranspose(value);
		const int8_t cents = unpackCents(value);
		if (src.oscType == OscType::SAMPLE && src.ranges.getNumElements() > 0) {
			if (auto* range = static_cast<MultisampleRange*>(src.ranges.getElementAddress(0))) {
				range->sampleHolder.transpose = transpose;
				range->sampleHolder.setCents(cents);
			}
		}
		else {
			src.transpose = transpose;
			src.setCents(cents);
		}
		return true;
	}
	case OutputSetting::Osc0SampleFlags:
	case OutputSetting::Osc1SampleFlags: {
		if (refs.sound == nullptr) {
			return false;
		}
		Source& src = refs.sound->sources[setting == OutputSetting::Osc0SampleFlags ? 0 : 1];
		unpackSampleFlags(value, src);
		return true;
	}
	case OutputSetting::Osc0SamplePath:
	case OutputSetting::Osc1SamplePath:
		return false; // handled via applyOutputSettingPath()
	case OutputSetting::ArpMain:
		if (refs.arp == nullptr) {
			return false;
		}
		refs.arp->mode = static_cast<ArpMode>(value & 0x0F);
		refs.arp->preset = static_cast<ArpPreset>((value >> 4) & 0x0F);
		refs.arp->octaveMode = static_cast<ArpOctaveMode>((value >> 8) & 0x0F);
		refs.arp->noteMode = static_cast<ArpNoteMode>((value >> 12) & 0x0F);
		refs.arp->includeInKitArp = (value & (1 << 16)) != 0;
		refs.arp->randomizerLock = (value & (1 << 17)) != 0;
		refs.arp->mpeVelocity = static_cast<ArpMpeModSource>((value >> 18) & 0x0F);
		return true;
	case OutputSetting::ArpCounts:
		if (refs.arp == nullptr) {
			return false;
		}
		refs.arp->numOctaves = static_cast<uint8_t>(value & 0xFF);
		refs.arp->numStepRepeats = static_cast<uint8_t>((value >> 8) & 0xFF);
		refs.arp->chordTypeIndex = static_cast<uint8_t>((value >> 16) & 0xFF);
		return true;
	case OutputSetting::ArpSync:
		if (refs.arp == nullptr) {
			return false;
		}
		refs.arp->syncLevel = static_cast<SyncLevel>(value & 0xFF);
		refs.arp->syncType = static_cast<SyncType>((value >> 8) & 0xFF);
		return true;
	case OutputSetting::ReverbModel:
		if (!refs.isSong) {
			return false;
		}
		song.model = static_cast<dsp::Reverb::Model>(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setModel(song.model);
		}
		return true;
	case OutputSetting::ReverbRoomSize:
		if (!refs.isSong) {
			return false;
		}
		song.reverbRoomSize = q31ToFloat(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setRoomSize(song.reverbRoomSize);
		}
		return true;
	case OutputSetting::ReverbDamping:
		if (!refs.isSong) {
			return false;
		}
		song.reverbDamp = q31ToFloat(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setDamping(song.reverbDamp);
		}
		return true;
	case OutputSetting::ReverbWidth:
		if (!refs.isSong) {
			return false;
		}
		song.reverbWidth = q31ToFloat(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setWidth(song.reverbWidth);
		}
		return true;
	case OutputSetting::ReverbHpf:
		if (!refs.isSong) {
			return false;
		}
		song.reverbHPF = q31ToFloat(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setHPF(song.reverbHPF);
		}
		return true;
	case OutputSetting::ReverbLpf:
		if (!refs.isSong) {
			return false;
		}
		song.reverbLPF = q31ToFloat(value);
		if (songIsCurrent) {
			AudioEngine::reverb.setLPF(song.reverbLPF);
		}
		return true;
	case OutputSetting::ReverbPan:
		if (!refs.isSong) {
			return false;
		}
		song.reverbPan = value;
		if (songIsCurrent) {
			AudioEngine::reverbPan = value;
		}
		return true;
	case OutputSetting::ReverbSidechainAttack:
		if (!refs.isSong) {
			return false;
		}
		song.reverbSidechainAttack = value;
		if (songIsCurrent) {
			AudioEngine::reverbSidechain.attack = value;
		}
		return true;
	case OutputSetting::ReverbSidechainRelease:
		if (!refs.isSong) {
			return false;
		}
		song.reverbSidechainRelease = value;
		if (songIsCurrent) {
			AudioEngine::reverbSidechain.release = value;
		}
		return true;
	case OutputSetting::ReverbSidechainVolume:
		if (!refs.isSong) {
			return false;
		}
		song.reverbSidechainVolume = value;
		if (songIsCurrent) {
			AudioEngine::reverbSidechainVolume = value;
		}
		return true;
	case OutputSetting::ReverbSidechainShape:
		if (!refs.isSong) {
			return false;
		}
		song.reverbSidechainShape = value;
		if (songIsCurrent) {
			AudioEngine::reverbSidechainShape = value;
		}
		return true;
	case OutputSetting::ReverbSidechainSync:
		if (!refs.isSong) {
			return false;
		}
		song.reverbSidechainSync = static_cast<SyncLevel>(value & 0xFF);
		if (songIsCurrent) {
			AudioEngine::reverbSidechain.syncLevel = song.reverbSidechainSync;
			AudioEngine::reverbSidechain.syncType = static_cast<SyncType>((value >> 8) & 0xFF);
		}
		return true;
	}
	return false;
}

bool applyOutputSettingPath(OutputSetting setting, const char* path, int32_t length, uint8_t target, Lane lane,
                            const Roster& roster, const Genre& genre, Song& song) {
	if (!isPathSetting(setting)) {
		return false;
	}
	const TargetRefs refs = resolveTarget(target, lane, roster, genre, &song);
	if (refs.sound == nullptr) {
		return false;
	}
	const int32_t osc = setting == OutputSetting::Osc0SamplePath ? 0 : 1;
	Source& source = refs.sound->sources[osc];
	if (source.oscType != OscType::SAMPLE && source.oscType != OscType::WAVETABLE) {
		setOscTypeSafe(*refs.sound, osc, OscType::SAMPLE);
	}
	MultiRange* range = source.getOrCreateFirstRange();
	if (range == nullptr) {
		return false;
	}
	AudioFileHolder* holder = range->getAudioFileHolder();
	if (holder == nullptr) {
		return false;
	}
	holder->setAudioFile(nullptr);
	if (path == nullptr || length <= 0) {
		holder->filePath.clear();
		return true;
	}
	char copy[kMaxSoundPath];
	const int32_t n = std::min<int32_t>(length, kMaxSoundPath - 1);
	std::memcpy(copy, path, static_cast<size_t>(n));
	copy[n] = 0;
	if (holder->filePath.set(copy) != Error::NONE) {
		return false;
	}
	// manuallySelected = false leaves the sample browser's last-path state alone.
	holder->loadFile(false, false, true, CLUSTER_ENQUEUE);
	return true;
}

void finalizeSoundSettings(uint8_t target, Lane lane, const Roster& roster, const Genre& genre, Song& song) {
	const TargetRefs refs = resolveTarget(target, lane, roster, genre, &song);
	if (refs.sound != nullptr && refs.manager != nullptr) {
		refs.sound->setupPatchingForAllParamManagers(&song);
		refs.sound->patcher.performInitialPatching(*refs.sound, *refs.manager);
	}
}

int32_t restoreSettings(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre, Song& song,
                        bool loadPaths, bool songIsCurrent) {
	int32_t written = 0;
	piece.forEachSettings([&](const void* data, int32_t length) {
		SettingsBlockHeader hdr{};
		int32_t blockWritten = 0;
		readSettingsBlock(
		    data, length, hdr,
		    [&](OutputSetting setting, int32_t value) {
			    if (applyOutputSetting(setting, value, hdr.output, hdr.lane, roster, genre, song, songIsCurrent)) {
				    blockWritten++;
			    }
		    },
		    [&](OutputSetting setting, const char* path, int32_t pathLen) {
			    if (loadPaths
			        && applyOutputSettingPath(setting, path, pathLen, hdr.output, hdr.lane, roster, genre, song)) {
				    blockWritten++;
			    }
		    });
		if (blockWritten > 0 && hdr.output != kSongParams) {
			finalizeSoundSettings(hdr.output, hdr.lane, roster, genre, song);
		}
		written += blockWritten;
	});
	return written;
}

void resetSongSettings(Song& song, bool songIsCurrent) {
	ModControllableAudio& m = song.globalEffectable;
	m.lpfMode = FilterMode::TRANSISTOR_24DB;
	m.hpfMode = FilterMode::HPLADDER;
	m.filterRoute = FilterRoute::HIGH_TO_LOW;
	m.setModFXType(ModFXType::NONE);
	m.clippingAmount = 0;
	m.delay.pingPong = true;
	m.delay.analog = false;
	m.delay.syncLevel = static_cast<SyncLevel>(7 - song.insideWorldTickMagnitude);
	m.delay.syncType = SYNC_TYPE_EVEN;
	m.compressor = RMSFeedbackCompressor{};
	m.compressor.setBaseGain(0.85f);
	m.stutterConfig = StutterConfig{};

	song.model = dsp::Reverb::Model::MUTABLE;
	song.reverbRoomSize = 30.0f / 50.0f;
	song.reverbDamp = 36.0f / 50.0f;
	song.reverbHPF = 0.0f;
	song.reverbLPF = 1.0f;
	song.reverbWidth = 1.0f;
	song.reverbPan = 0;
	song.reverbSidechainVolume = getParamFromUserValue(modulation::params::STATIC_SIDECHAIN_VOLUME, -1);
	song.reverbSidechainShape = -601295438;
	song.reverbSidechainSync = SYNC_LEVEL_8TH;
	if (songIsCurrent) {
		AudioEngine::reverb.setModel(song.model);
		AudioEngine::reverb.setRoomSize(song.reverbRoomSize);
		AudioEngine::reverb.setDamping(song.reverbDamp);
		AudioEngine::reverb.setHPF(song.reverbHPF);
		AudioEngine::reverb.setLPF(song.reverbLPF);
		AudioEngine::reverb.setWidth(song.reverbWidth);
		AudioEngine::reverbPan = song.reverbPan;
		AudioEngine::reverbSidechainVolume = song.reverbSidechainVolume;
		AudioEngine::reverbSidechainShape = song.reverbSidechainShape;
		AudioEngine::reverbSidechain.syncLevel = song.reverbSidechainSync;
		AudioEngine::reverbSidechain.syncType = SYNC_TYPE_EVEN;
	}
}

// ---- SettingsTracker ----

void SettingsTracker::forget() {
	for (Slot& slot : slots_) {
		slot.valid = false;
		slot.editedMask = 0;
	}
	pollCursor_ = 0;
	clearPending();
}

void SettingsTracker::clearPending() {
	pendingScalarCount_ = 0;
	pendingPathCount_ = 0;
}

void SettingsTracker::captureSlot(int32_t slotIndex, uint8_t output, Lane lane, const Roster& roster,
                                  const Genre& genre, Song* song) {
	if (slotIndex < 0 || slotIndex >= kMaxSlots) {
		return;
	}
	Slot& slot = slots_[slotIndex];
	slot.output = output;
	slot.lane = lane;
	slot.editedMask = 0;
	const TargetRefs refs = resolveTarget(output, lane, roster, genre, song);
	if (refs.mod == nullptr) {
		slot.valid = false;
		return;
	}
	const char* paths[2] = {"", ""};
	readTarget(refs, song, slot.values, paths);
	slot.pathHash[0] = hashPath(paths[0], slot.pathLen[0]);
	slot.pathHash[1] = hashPath(paths[1], slot.pathLen[1]);
	slot.valid = true;
}

void SettingsTracker::resetAll(const Roster& roster, const Genre& genre, Song& song) {
	forget();
	captureSlot(0, kSongParams, Lane::None, roster, genre, &song);
	resetRoster(roster, genre);
}

void SettingsTracker::resetRoster(const Roster& roster, const Genre& genre) {
	for (int32_t i = 1; i < kMaxSlots; i++) {
		slots_[i].valid = false;
		slots_[i].editedMask = 0;
	}
	for (int32_t p = 0; p < partCount(genre); p++) {
		resetPart(p, roster, genre);
	}
}

void SettingsTracker::resetPart(int32_t part, const Roster& roster, const Genre& genre) {
	if (part < 0 || part >= partCount(genre)) {
		return;
	}
	captureSlot(slotIndexFor(static_cast<uint8_t>(part), Lane::None), static_cast<uint8_t>(part), Lane::None, roster,
	            genre, nullptr);
	for (const LaneSpec& lane : genre.parts[part].lanes) {
		captureSlot(slotIndexFor(static_cast<uint8_t>(part), lane.lane), static_cast<uint8_t>(part), lane.lane, roster,
		            genre, nullptr);
	}
	// Drop any queued mid-piece edits for the replaced Output.
	int32_t keptScalars = 0;
	for (int32_t i = 0; i < pendingScalarCount_; i++) {
		if (pendingScalars_[i].target != part) {
			pendingScalars_[keptScalars++] = pendingScalars_[i];
		}
	}
	pendingScalarCount_ = keptScalars;
	int32_t keptPaths = 0;
	for (int32_t i = 0; i < pendingPathCount_; i++) {
		if (pendingPaths_[i].target != part) {
			pendingPaths_[keptPaths++] = pendingPaths_[i];
		}
	}
	pendingPathCount_ = keptPaths;
}

void SettingsTracker::enqueueScalar(uint8_t target, Lane lane, OutputSetting setting, int32_t value) {
	const auto id = static_cast<uint16_t>(setting);
	for (int32_t i = 0; i < pendingScalarCount_; i++) {
		Command& c = pendingScalars_[i];
		if (c.target == target && c.lane == lane && c.length == id) {
			c.value = value;
			return;
		}
	}
	if (pendingScalarCount_ < kMaxPendingScalars) {
		pendingScalars_[pendingScalarCount_++] = {CommandKind::SetSetting, target, value, id, lane};
	}
}

void SettingsTracker::enqueuePath(uint8_t target, Lane lane, OutputSetting setting, const char* path, int32_t length) {
	const auto id = static_cast<uint16_t>(setting);
	for (int32_t i = 0; i < pendingPathCount_; i++) {
		PendingPath& p = pendingPaths_[i];
		if (p.target == target && p.lane == lane && p.setting == id) {
			p.path.clear();
			if (path != nullptr && length > 0) {
				p.path.set(path);
			}
			return;
		}
	}
	if (pendingPathCount_ < kMaxPendingPaths) {
		PendingPath& p = pendingPaths_[pendingPathCount_++];
		p.target = target;
		p.lane = lane;
		p.setting = id;
		p.path.clear();
		if (path != nullptr && length > 0) {
			p.path.set(path);
		}
	}
}

void SettingsTracker::scanSlot(int32_t slotIndex, const Roster& roster, const Genre& genre, Song& song, bool playing) {
	if (slotIndex < 0 || slotIndex >= kMaxSlots || !slots_[slotIndex].valid) {
		return;
	}
	Slot& slot = slots_[slotIndex];
	const TargetRefs refs = resolveTarget(slot.output, slot.lane, roster, genre, &song);
	if (refs.mod == nullptr) {
		return;
	}
	int32_t live[kNumOutputSettings] = {};
	const char* paths[2] = {"", ""};
	readTarget(refs, &song, live, paths);
	uint64_t mask = validScalarMask(refs);
	while (mask != 0) {
		const auto id = static_cast<uint16_t>(std::countr_zero(mask));
		mask &= mask - 1;
		if (live[id] != slot.values[id]) {
			slot.values[id] = live[id];
			const auto setting = static_cast<OutputSetting>(id);
			slot.editedMask |= settingBit(setting);
			if (playing) {
				enqueueScalar(slot.output, slot.lane, setting, live[id]);
			}
			TEST_EVENT("gen_setting target=%u lane=%d id=%u val=%ld", static_cast<unsigned>(slot.output),
			           slot.lane == Lane::None ? -1 : static_cast<int>(slot.lane), static_cast<unsigned>(id),
			           static_cast<long>(live[id]));
		}
	}
	if (refs.sound != nullptr) {
		for (int32_t osc = 0; osc < 2; osc++) {
			uint16_t len = 0;
			const uint32_t h = hashPath(paths[osc], len);
			if (h != slot.pathHash[osc] || len != slot.pathLen[osc]) {
				slot.pathHash[osc] = h;
				slot.pathLen[osc] = len;
				const OutputSetting setting = osc == 0 ? OutputSetting::Osc0SamplePath : OutputSetting::Osc1SamplePath;
				slot.editedMask |= settingBit(setting);
				if (playing) {
					enqueuePath(slot.output, slot.lane, setting, paths[osc], len);
				}
				TEST_EVENT("gen_setting_path target=%u lane=%d osc=%ld len=%u", static_cast<unsigned>(slot.output),
				           slot.lane == Lane::None ? -1 : static_cast<int>(slot.lane), static_cast<long>(osc),
				           static_cast<unsigned>(len));
			}
		}
	}
}

int32_t SettingsTracker::prioritySlot(const Roster& roster, const Genre& genre, Song& song, int32_t focusedPart) const {
	if (getCurrentUI() == &soundEditor && soundEditor.currentModControllable != nullptr) {
		ModControllableAudio* target = soundEditor.currentModControllable;
		if (target == &song.globalEffectable) {
			return 0;
		}
		for (int32_t p = 0; p < partCount(genre); p++) {
			const PartOutput& part = roster.parts[p];
			if (part.output == nullptr) {
				continue;
			}
			if ((part.output->type == OutputType::SYNTH && static_cast<SoundInstrument*>(part.output) == target)
			    || (part.output->type == OutputType::KIT && static_cast<Kit*>(part.output) == target)) {
				return slotIndexFor(static_cast<uint8_t>(p), Lane::None);
			}
			for (const LaneSpec& lane : genre.parts[p].lanes) {
				Drum* drum = part.drums[laneRow(lane.lane)];
				if (drum != nullptr && drum->type == DrumType::SOUND && static_cast<SoundDrum*>(drum) == target) {
					return slotIndexFor(static_cast<uint8_t>(p), lane.lane);
				}
			}
		}
	}
	if (focusedPart >= 0 && focusedPart < partCount(genre)) {
		return slotIndexFor(static_cast<uint8_t>(focusedPart), Lane::None);
	}
	return 0;
}

void SettingsTracker::poll(const Roster& roster, const Genre& genre, Song& song, bool playing, int32_t focusedPart) {
	// At most 2 slots per frame: the actively-edited/focused Output plus 1 round-robin slot.
	const int32_t prio = prioritySlot(roster, genre, song, focusedPart);
	if (prio >= 0) {
		scanSlot(prio, roster, genre, song, playing);
	}
	for (int32_t tries = 0; tries < kMaxSlots; tries++) {
		const int32_t s = pollCursor_;
		pollCursor_ = (pollCursor_ + 1) % kMaxSlots;
		if (s != prio && slots_[s].valid) {
			scanSlot(s, roster, genre, song, playing);
			break;
		}
	}
}

void SettingsTracker::scanAll(const Roster& roster, const Genre& genre, Song& song, bool playing) {
	for (int32_t s = 0; s < kMaxSlots; s++) {
		if (slots_[s].valid) {
			scanSlot(s, roster, genre, song, playing);
		}
	}
}

void SettingsTracker::drainToLog(SteeringLog& log, int32_t step) {
	if (pendingScalarCount_ == 0 && pendingPathCount_ == 0) {
		return;
	}
	for (int32_t i = 0; i < pendingScalarCount_; i++) {
		log.append(step, pendingScalars_[i]);
	}
	for (int32_t i = 0; i < pendingPathCount_; i++) {
		const PendingPath& p = pendingPaths_[i];
		log.appendSettingPath(step, p.target, p.lane, p.setting, p.path.c_str());
	}
	clearPending();
}

void SettingsTracker::logHeader(SteeringLog& log, const Roster& roster, const Genre& genre, Song& song) {
	// Sweep all slots so stopped edits (e.g. from SampleBrowser or MIDI Start) reach the header.
	scanAll(roster, genre, song, false);
	constexpr uint64_t kPathBits =
	    settingBit(OutputSetting::Osc0SamplePath) | settingBit(OutputSetting::Osc1SamplePath);
	for (int32_t s = 0; s < kMaxSlots; s++) {
		const Slot& slot = slots_[s];
		if (!slot.valid || slot.editedMask == 0) {
			continue;
		}
		const uint64_t scalarMask = slot.editedMask & ~kPathBits;
		uint64_t pathMask = slot.editedMask & kPathBits;
		const char* paths[2] = {"", ""};
		uint8_t pathLens[2] = {0, 0};
		int32_t pathBytes = 0;
		if (pathMask != 0) {
			const TargetRefs refs = resolveTarget(slot.output, slot.lane, roster, genre, &song);
			if (refs.sound != nullptr) {
				for (int32_t osc = 0; osc < 2; osc++) {
					const OutputSetting id = osc == 0 ? OutputSetting::Osc0SamplePath : OutputSetting::Osc1SamplePath;
					if ((pathMask & settingBit(id)) != 0) {
						paths[osc] = sourcePath(refs.sound->sources[osc]);
						pathLens[osc] =
						    static_cast<uint8_t>(std::min<size_t>(std::strlen(paths[osc]), kMaxSoundPath - 1));
						pathBytes += kPathSettingPrefixBytes + pathLens[osc];
					}
				}
			}
			else {
				pathMask = 0;
			}
		}
		const auto scalarCount = static_cast<uint8_t>(std::popcount(scalarMask));
		const auto pathCount = static_cast<uint8_t>(std::popcount(pathMask));
		if (scalarCount == 0 && pathCount == 0) {
			continue;
		}
		const int32_t total =
		    static_cast<int32_t>(sizeof(SettingsBlockHeader)) + scalarCount * kScalarSettingBytes + pathBytes;
		uint8_t* at = log.settingsSpace(total);
		if (at == nullptr) {
			return; // header full: piece cut at step 0
		}
		const SettingsBlockHeader hdr{slot.output, slot.lane, scalarCount, pathCount};
		std::memcpy(at, &hdr, sizeof hdr);
		at += sizeof hdr;
		uint64_t m = scalarMask;
		while (m != 0) {
			const auto id = static_cast<uint16_t>(std::countr_zero(m));
			m &= m - 1;
			std::memcpy(at, &id, sizeof id);
			std::memcpy(at + sizeof id, &slot.values[id], sizeof(int32_t));
			at += kScalarSettingBytes;
		}
		for (int32_t osc = 0; osc < 2; osc++) {
			const OutputSetting id = osc == 0 ? OutputSetting::Osc0SamplePath : OutputSetting::Osc1SamplePath;
			if ((pathMask & settingBit(id)) != 0) {
				const auto rawId = static_cast<uint16_t>(id);
				std::memcpy(at, &rawId, sizeof rawId);
				at[sizeof rawId] = pathLens[osc];
				at += kPathSettingPrefixBytes;
				if (pathLens[osc] > 0) {
					std::memcpy(at, paths[osc], pathLens[osc]);
					at += pathLens[osc];
				}
			}
		}
	}
}

bool SettingsTracker::partNeedsRebuild(const SteeringLog::Piece& piece, int32_t part) const {
	constexpr uint64_t kPathBits =
	    settingBit(OutputSetting::Osc0SamplePath) | settingBit(OutputSetting::Osc1SamplePath);
	for (int32_t s = 1; s < kMaxSlots; s++) {
		const Slot& slot = slots_[s];
		if (!slot.valid || slot.output != part) {
			continue;
		}
		const uint64_t inHeader = headerMaskFor(piece, slot.output, slot.lane);
		// Any setting edited since load that wasn't in the header, or any osc path change (which needs card I/O),
		// requires rebuilding the part in task context.
		if ((slot.editedMask & ~inHeader) != 0 || ((slot.editedMask | inHeader) & kPathBits) != 0) {
			return true;
		}
	}
	return false;
}

bool SettingsTracker::songNeedsReset(const SteeringLog::Piece& piece) const {
	if (!slots_[0].valid || slots_[0].editedMask == 0) {
		return false;
	}
	const uint64_t inHeader = headerMaskFor(piece, kSongParams, Lane::None);
	return (slots_[0].editedMask & ~inHeader) != 0;
}

void SettingsTracker::syncToHeader(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre,
                                   Song& song) {
	clearPending();
	for (int32_t s = 0; s < kMaxSlots; s++) {
		Slot& slot = slots_[s];
		if (!slot.valid) {
			continue;
		}
		captureSlot(s, slot.output, slot.lane, roster, genre, &song);
		slot.editedMask = headerMaskFor(piece, slot.output, slot.lane);
	}
}

} // namespace deluge::generative::firmware
