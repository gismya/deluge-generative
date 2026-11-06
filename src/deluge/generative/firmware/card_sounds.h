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

// The card side of the Generator's sounds (spec §10): listing card folders for the core's sound picks, telling a card
// error from a missing file, and writing the load-time settings onto a loaded Sound.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/palette.h"
#include "generative/core/sound_settings.h"

class Sound;
class ParamManager;

namespace deluge::generative::firmware {

/// Lists card folders over FatFs, in directory order. Runs in task context: it reads the card.
class CardFolders final : public FolderLister {
public:
	bool list(const char* folder, FileVisitor& visitor) override;
};

/// Whether a load failed because the card did (spec §10.7), rather than because the file is missing or unreadable.
/// RAM running out counts too: either way the load stops instead of falling back.
bool stopsLoading(Error error);

/// What a load error means to the core's fallback chain.
LoadOutcome loadOutcome(Error error);

/// Writes `settings` onto `sound` and its ParamManager (spec §10.6). Plain field and param writes, before playback.
void applySoundSettings(Sound& sound, ParamManager& paramManager, const SoundSettings& settings);

} // namespace deluge::generative::firmware
