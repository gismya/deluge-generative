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
#include "gui/ui/browser/default_name.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <cstdio>
#include <cstring>
#include <string>

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

/// SONGS as nextDefaultName() reads it: the whole folder, straight from the card.
class SongFolder final : public deluge::gui::browser::FileListView {
public:
	[[nodiscard]] std::string highestNumberedName(char const* prefix) const override {
		std::string highest;
		const auto prefixLength = static_cast<int32_t>(std::strlen(prefix));
		forEachSong([&](const char* name, int32_t stem) {
			if (stem <= prefixLength || strncasecmp(name, prefix, prefixLength) != 0) {
				return;
			}
			for (int32_t i = prefixLength; i < stem; i++) {
				if (name[i] < '0' || name[i] > '9') {
					return;
				}
			}
			if (highest.empty() || strcmpspecial(name, highest.c_str()) > 0) {
				highest = name;
			}
		});
		return highest;
	}

	/// Songs named by Capture never take letter variations (they aren't SONGnnn).
	[[nodiscard]] uint32_t takenLetterSuffixes(char const* /*stem*/) const override { return 0; }

private:
	template <typename Found>
	static void forEachSong(Found found) {
		DIR dir;
		if (f_opendir(&dir, kSongFolder) != FR_OK) {
			return;
		}
		FILINFO info;
		while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
			if ((info.fattrib & AM_DIR) != 0 || info.fname[0] == '.') {
				continue;
			}
			const int32_t stem = songStemLength(info.fname);
			if (stem > 0) {
				found(info.fname, stem);
			}
		}
		f_closedir(&dir);
	}
};

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

Error saveCurrentSong(const char* name, char (&saved)[kMaxSongFileName + 1]) {
	Error error = StorageManager::initSD();
	if (error != Error::NONE) {
		return error;
	}
	const FRESULT made = f_mkdir(kSongFolder);
	if (made != FR_OK && made != FR_EXIST) {
		return fresultToDelugeErrorCode(made);
	}
	snprintf(saved, sizeof(saved), "%s", name);
	if (songExists(saved)) {
		// The firmware's normal rule for a name already taken (spec §17.7): its next free variation.
		const std::string next = deluge::gui::browser::nextDefaultName(saved, "", SongFolder{});
		if (next == saved || next.size() > kMaxSongFileName) {
			return Error::FILE_ALREADY_EXISTS;
		}
		snprintf(saved, sizeof(saved), "%s", next.c_str());
	}
	char path[kMaxSongFileName + 16];
	snprintf(path, sizeof(path), "%s/%s%s", kSongFolder, saved, writeJsonFlag ? ".Json" : ".XML");

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
	currentSong->name.set(saved);
	currentSong->dirPath.set(kSongFolder);
	if (FlashStorage::defaultStartupSongMode == StartupSongMode::LASTSAVED) {
		runtimeFeatureSettings.writeSettingsToFile();
	}
	return Error::NONE;
}

} // namespace deluge::generative::firmware
