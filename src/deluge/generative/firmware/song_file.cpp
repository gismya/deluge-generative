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

#include "generative/firmware/song_file.h"
#include "extern.h"
#include "fatfs/ff.h"
#include "generative/perform/capture.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <cstdio>
#include <cstring>

namespace deluge::generative::firmware {

namespace {

constexpr const char* kSongFolder = "SONGS";

/// Whether `name` ends in `.XML` or `.Json`, the song files the browser lists: the stem's length, or -1.
int32_t songStemLength(const char* name) {
	const char* dot = std::strrchr(name, '.');
	if (dot == nullptr || (strcasecmp(dot + 1, "XML") != 0 && strcasecmp(dot + 1, "Json") != 0)) {
		return -1;
	}
	return static_cast<int32_t>(dot - name);
}

bool songExists(const char* name) {
	char path[kMaxSongFileName + 16];
	for (const char* extension : {".XML", ".Json"}) {
		snprintf(path, sizeof(path), "%s/%s%s", kSongFolder, name, extension);
		if (StorageManager::fileExists(path)) {
			return true;
		}
	}
	return false;
}

} // namespace

uint32_t captureLettersTaken(const char* base) {
	uint32_t taken = 0;
	if (StorageManager::initSD() != Error::NONE) {
		return taken;
	}
	DIR dir;
	if (f_opendir(&dir, kSongFolder) != FR_OK) {
		return taken; // no SONGS yet: every name is free
	}
	FILINFO info;
	char stem[sizeof(info.fname)];
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
		if ((info.fattrib & AM_DIR) != 0 || info.fname[0] == '.') {
			continue;
		}
		const int32_t length = songStemLength(info.fname);
		if (length <= 0) {
			continue;
		}
		std::memcpy(stem, info.fname, length);
		stem[length] = 0;
		const int32_t letter = perform::captureLetterOf(base, stem);
		if (letter >= 0) {
			taken |= 1U << letter;
		}
	}
	f_closedir(&dir);
	return taken;
}

Error saveCurrentSong(const char* name) {
	Error error = StorageManager::initSD();
	if (error != Error::NONE) {
		return error;
	}
	const FRESULT made = f_mkdir(kSongFolder);
	if (made != FR_OK && made != FR_EXIST) {
		return fresultToDelugeErrorCode(made);
	}
	if (std::strlen(name) > static_cast<size_t>(kMaxSongFileName) || songExists(name)) {
		return Error::FILE_ALREADY_EXISTS;
	}
	char path[kMaxSongFileName + 16];
	snprintf(path, sizeof(path), "%s/%s%s", kSongFolder, name, writeJsonFlag ? ".Json" : ".XML");

	// As SaveSongUI::performSave() writes a new file.
	error = writeJsonFlag ? StorageManager::createJsonFile(path, smJsonSerializer, false, false)
	                      : StorageManager::createXMLFile(path, smSerializer, false, false);
	if (error != Error::NONE) {
		return error;
	}
	currentSong->writeToFile();
	error = GetSerializer().closeFileAfterWriting(
	    path, writeJsonFlag ? "{\"song\": {\n" : "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<song\n", "\n</song>\n");
	if (error != Error::NONE) {
		f_unlink(path); // stock leaves a truncated file; a pulled card refuses this too, and that's fine
		return error;
	}
	currentSong->name.set(name);
	currentSong->dirPath.set(kSongFolder);
	if (FlashStorage::defaultStartupSongMode == StartupSongMode::LASTSAVED) {
		runtimeFeatureSettings.writeSettingsToFile();
	}
	return Error::NONE;
}

} // namespace deluge::generative::firmware
