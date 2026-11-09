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
#include "fatfs/fatfs.hpp"
#include "io/midi/test_port.h"
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

namespace {

/// Whether a FatFs result means the card itself failed (spec §10.7). Anything else about one folder, such as a name
/// FatFs rejects ('*', ':') or a path that isn't there, only means that folder lists nothing.
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

} // namespace

bool cardErrorInjected() {
#if ENABLE_SYSEX_TEST
	return TestPort::cardErrorHeld();
#else
	return false;
#endif
}

namespace {
/// Whether the card can't be read at all: not there, or (test builds) a card error held by the test port.
bool cardUnusable() {
	return cardErrorInjected() || StorageManager::initSD() != Error::NONE;
}
} // namespace

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
	while (true) {
		// A big folder takes a while to list: a runner job yields inside it too, so BACK stays prompt (spec §12.10).
		// The job holds the card, so nothing else reads it while it yields.
		if (chunks_ != nullptr && chunks_->boundary()) {
			cancelled_ = true;
			return false;
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

void applySoundSettings(Sound& sound, ParamManager& paramManager, const SoundSettings& settings) {
	// Auto keeps what loading set: a sample row's default, or an FX row preset's own mode.
	if (settings.voiceMode != VoiceMode::Auto) {
		sound.polyphonic = polyphonyMode(settings.voiceMode);
	}
	if (settings.maxVoices > 0) {
		sound.maxVoiceCount = settings.maxVoices;
	}
	sound.voicePriority = voicePriority(settings.priority);

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
