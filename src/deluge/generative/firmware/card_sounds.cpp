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

#include "generative/firmware/card_sounds.h"
#include "dsp/delay/delay.h"
#if ENABLE_SYSEX_TEST
#include "generative/perform/sound_mapping.h"
#endif
#include "fatfs/fatfs.hpp"
#include "io/midi/test_port.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <climits>

namespace deluge::generative::firmware {

namespace params = deluge::modulation::params;

bool cardFailed(FatFS::Error error) {
	switch (error) {
	case FatFS::Error::DISK_ERR:
	case FatFS::Error::INT_ERR:
	case FatFS::Error::NOT_READY:
	case FatFS::Error::NOT_ENABLED:
	case FatFS::Error::NO_FILESYSTEM:
		return true;
	default:
		return false;
	}
}

AutoParam& postFxVolume(ParamManager& paramManager) {
	return paramManager.getPatchedParamSet()->params[params::GLOBAL_VOLUME_POST_FX];
}

bool cardErrorInjected() {
#if ENABLE_SYSEX_TEST
	return TestPort::cardErrorHeld();
#else
	return false;
#endif
}

namespace {
/// Outside a runner job, audio gets a turn this often while a folder lists, as well as before each folder: a large
/// folder could otherwise underrun a playing song (tuning).
constexpr int32_t kEntriesPerAudioTurn = 16;

/// Whether the card can't be read at all: not there, or (test builds) a card error held by the test port.
bool cardUnusable() {
	return cardErrorInjected() || StorageManager::initSD() != Error::NONE;
}
} // namespace

LoadOutcome CardFiles::exists(const char* path) {
	if (chunks_ != nullptr) {
		if (chunks_->boundary()) {
			cancelled_ = true;
			return LoadOutcome::CardError;
		}
	}
	else {
		AudioEngine::routineWithClusterLoading();
	}
	if (cardUnusable()) {
		return LoadOutcome::CardError;
	}
	auto info = FatFS::stat(path);
	if (!info) {
		// Not there, or a name FatFs can't take: missing. Only the card's own failure stops the load.
		return cardFailed(info.error()) ? LoadOutcome::CardError : LoadOutcome::Missing;
	}
	return (info->fattrib & AM_DIR) == 0 ? LoadOutcome::Loaded : LoadOutcome::Missing;
}

bool CardFolders::list(const char* folder, FileVisitor& visitor) {
	if (chunks_ != nullptr) {
		if (chunks_->boundary()) {
			cancelled_ = true;
			return false;
		}
	}
	else {
		AudioEngine::routineWithClusterLoading(); // audio gets a turn before every folder read
	}
	if (cardUnusable()) {
		return false;
	}
	auto opened = FatFS::Directory::open(folder);
	if (!opened) {
		// A folder that isn't there, or that FatFs can't name, lists nothing: the lane falls back (spec §10.2).
		return !cardFailed(opened.error());
	}
	FatFS::Directory& directory = opened.value();
	for (int32_t entries = 1;; entries++) {
		// A big folder takes a while to list: a runner job yields inside it too, so BACK stays prompt (spec §12.10).
		// The job holds the card, so nothing else reads it while it yields. Outside a job, audio gets a turn.
		if (chunks_ != nullptr) {
			if (chunks_->boundary()) {
				cancelled_ = true;
				return false;
			}
		}
		else if (entries % kEntriesPerAudioTurn == 0) {
			AudioEngine::routineWithClusterLoading();
		}
		audioFileManager.loadAnyEnqueuedClusters();
		auto entry = directory.read();
		if (!entry) {
			return !cardFailed(entry.error());
		}
		if (entry->fname[0] == 0) {
			return true; // end of the folder
		}
		if ((entry->fattrib & AM_DIR) == 0) {
			visitor.file(entry->fname);
		}
	}
}

bool stopsLoading(Error error) {
	switch (error) {
	case Error::SD_CARD:
	case Error::SD_CARD_NOT_PRESENT:
	case Error::SD_CARD_NO_FILESYSTEM:
	case Error::INSUFFICIENT_RAM:
		return true;
	default:
		return false;
	}
}

LoadOutcome loadOutcome(Error error) {
	if (error == Error::NONE) {
		return LoadOutcome::Loaded;
	}
	return stopsLoading(error) ? LoadOutcome::CardError : LoadOutcome::Missing;
}

namespace {

PolyphonyMode polyphonyMode(VoiceMode mode) {
	switch (mode) {
	case VoiceMode::Poly:
		return PolyphonyMode::POLY;
	case VoiceMode::Mono:
		return PolyphonyMode::MONO;
	case VoiceMode::Legato:
		return PolyphonyMode::LEGATO;
	case VoiceMode::Choke:
		return PolyphonyMode::CHOKE;
	case VoiceMode::Auto:
		break;
	}
	return PolyphonyMode::AUTO;
}

VoicePriority voicePriority(VoicePriorityLevel level) {
	switch (level) {
	case VoicePriorityLevel::Low:
		return VoicePriority::LOW;
	case VoicePriorityLevel::High:
		return VoicePriority::HIGH;
	case VoicePriorityLevel::Medium:
		break;
	}
	return VoicePriority::MEDIUM;
}

} // namespace

void applyDelaySetup(Delay& delay, const DelaySetup& setup, const Song& song) {
	if (!setup.on) {
		return; // the preset's own delay, as saved
	}
	// Plain fields, read by the render each block: Compose saves them with the song (spec §10.6).
	delay.syncLevel = static_cast<SyncLevel>(
	    delaySyncLevel(setup.note, song.insideWorldTickMagnitude + song.insideWorldTickMagnitudeOffsetFromBPM));
	switch (setup.feel) {
	case DelayFeel::Even:
		delay.syncType = SYNC_TYPE_EVEN;
		break;
	case DelayFeel::Dotted:
		delay.syncType = SYNC_TYPE_DOTTED;
		break;
	case DelayFeel::Triplet:
		delay.syncType = SYNC_TYPE_TRIPLET;
		break;
	}
	delay.pingPong = setup.pingPong;
	delay.analog = setup.analog;
}

#if ENABLE_SYSEX_TEST
namespace {
bool previewBuild = false;
} // namespace

const char* buildEventPrefix() {
	return previewBuild ? "preview_" : "";
}

void setPreviewBuild(bool preview) {
	previewBuild = preview;
}

void reportDelay(const Delay& delay, int32_t feedback, const char* part, const char* lane) {
	TEST_EVENT("%sgen_sound_delay %s %s sync=%d type=%d pingpong=%d analog=%d feedback=%ld", buildEventPrefix(), part,
	           lane, static_cast<int>(delay.syncLevel), static_cast<int>(delay.syncType), delay.pingPong ? 1 : 0,
	           delay.analog ? 1 : 0, static_cast<long>(perform::menuUnits(feedback)));
}
#endif

void applySoundSettings(Sound& sound, ParamManager& paramManager, const SoundSettings& settings, const Song& song) {
	applyDelaySetup(sound.delay, settings.delay, song);
	// Auto keeps what loading set: a sample row's default, or an FX row preset's own mode.
	if (settings.voiceMode != VoiceMode::Auto) {
		sound.polyphonic = polyphonyMode(settings.voiceMode);
	}
	if (settings.maxVoices > 0) {
		sound.maxVoiceCount = settings.maxVoices;
	}
	sound.voicePriority = voicePriority(settings.priority);
	if (settings.zeroTranspose) {
		// Read at each note-on (Voice::noteOn, calculatePhaseIncrements), never cached: the load's fresh Output has no
		// voices, so there are no phase increments to recalculate, as the sound editor's transpose does.
		sound.transpose = 0;
	}

	if (settings.volume != 100) {
		AutoParam& volume = postFxVolume(paramManager);
		volume.setCurrentValueBasicForSetup(scaleKnob(volume.getCurrentValue(), settings.volume));
		// The patcher worked out the Sound's levels when its clip became active (link() → setActiveClip()), and a
		// setup write doesn't tell it: patch again, or the Sound plays on at the preset's level (#218).
		sound.patcher.performInitialPatching(sound, paramManager);
	}

	// Portamento: a sliding part keeps a preset's, or gets the role default if it's off. Everyone else gets 0, which
	// the param stores as its minimum.
	AutoParam& portamento = paramManager.getUnpatchedParamSet()->params[params::UNPATCHED_PORTAMENTO];
	bool off = portamento.getCurrentValue() == INT32_MIN;
	if (!settings.slides) {
		portamento.setCurrentValueBasicForSetup(INT32_MIN);
	}
	else if (off) {
		portamento.setCurrentValueBasicForSetup(getParamFromUserValue(
		    params::UNPATCHED_START + params::UNPATCHED_PORTAMENTO, static_cast<int8_t>(settings.portamento)));
	}
}

} // namespace deluge::generative::firmware
