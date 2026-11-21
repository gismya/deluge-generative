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

#include "generative/perform/sound_browser.h"
#include <cstring>

namespace deluge::generative::perform {

namespace {

/// The folder presets are browsed from when nothing better is known.
constexpr const char* kSynthsFolder = "SYNTHS";

/// Sets `out` to `path`'s folder: everything before its last '/', or empty when it has none.
void folderOf(const char* path, SoundPath& out) {
	out.clear();
	const char* slash = std::strrchr(path, '/');
	if (slash == nullptr) {
		return;
	}
	const auto length = static_cast<size_t>(slash - path);
	if (length < sizeof(out.text)) {
		std::memcpy(out.text, path, length);
		out.text[length] = 0;
	}
}

} // namespace

BrowserStart browserStart(const Palette& palette, Lane lane, const SoundPath& pick, BrowseKind kind) {
	BrowserStart start;
	const LaneSounds* sounds = lane != Lane::None ? palette.laneSounds(lane) : nullptr;
	if (kind == BrowseKind::Sample) {
		const auto folders = sounds != nullptr ? laneFolders(*sounds) : defaultLaneFolders(lane);
		start.fallback.set(folders.empty() ? "SAMPLES" : folders[0]);
	}
	else if (sounds != nullptr && !sounds->synths.empty()) {
		folderOf(sounds->synths[0].path, start.fallback);
	}
	if (start.fallback.empty()) {
		start.fallback.set(kSynthsFolder);
	}

	// The pick, where it is of the kind listed: an FX lane's pick may be a sample or a preset.
	const bool ofKind = !pick.empty() && (isAudioFile(pick.c_str()) == (kind == BrowseKind::Sample));
	const char* slash = ofKind ? std::strrchr(pick.c_str(), '/') : nullptr;
	if (slash != nullptr) {
		folderOf(pick.c_str(), start.folder);
		start.name = slash + 1;
	}
	if (start.folder.empty()) {
		start.folder = start.fallback;
		start.name = nullptr;
	}
	return start;
}

} // namespace deluge::generative::perform
