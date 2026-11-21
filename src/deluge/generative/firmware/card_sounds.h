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
#include "fatfs/fatfs.hpp"
#include "generative/core/palette.h"
#include "generative/core/sound_settings.h"
#include "generative/runner/runner.h"

class Delay;
class Song;
class Sound;
class ParamManager;
class AutoParam;

namespace deluge::generative::firmware {

/// Lists card folders over FatFs, in directory order. Runs in task context: it reads the card.
class CardFolders final : public FolderLister {
public:
	/// With a runner job's `chunks`, each folder read is a chunk boundary, so BACK is seen during a long scan
	/// (spec §12.10). Without, audio gets a turn before each read.
	explicit CardFolders(Chunks* chunks = nullptr) : chunks_(chunks) {}
	/// Returns false on a card error, and when the job was cancelled at a folder read (cancelled() says which).
	bool list(const char* folder, FileVisitor& visitor) override;
	/// Whether a listing stopped because the runner job was cancelled, not because the card failed.
	[[nodiscard]] bool cancelled() const { return cancelled_; }

private:
	Chunks* chunks_;
	bool cancelled_ = false;
};

/// Says whether card files are there, over FatFs (a Recipe file's load checks its sound picks, spec §16.6). Runs in
/// task context: it reads the card.
class CardFiles final : public FileChecker {
public:
	/// With a runner job's `chunks`, each check is a chunk boundary. Without, audio gets a turn before each.
	explicit CardFiles(Chunks* chunks = nullptr) : chunks_(chunks) {}
	/// CardError on a card error, and when the job was cancelled at a check (cancelled() says which).
	LoadOutcome exists(const char* path) override;
	[[nodiscard]] bool cancelled() const { return cancelled_; }

private:
	Chunks* chunks_;
	bool cancelled_ = false;
};

/// Test builds: whether the test port holds a card error ("hold card_error"), so Perform's loads fail as a pulled
/// card's would. Always false otherwise.
bool cardErrorInjected();

/// Whether a FatFs result means the card itself failed (spec §10.7). Anything else about one file or folder, such as
/// a name FatFs rejects ('*', ':'), a path that isn't there or one it refuses (DENIED), only means it isn't there.
bool cardFailed(FatFS::Error error);

/// Whether a load failed because the card did (spec §10.7), rather than because the file is missing or unreadable.
/// RAM running out counts too: either way the load stops instead of falling back.
bool stopsLoading(Error error);

/// What a load error means to the core's fallback chain.
LoadOutcome loadOutcome(Error error);

/// Writes `settings` onto `sound` and its ParamManager (spec §10.6). Plain field and param writes, before playback; a
/// changed volume patches the Sound again, so `paramManager` must be the one it plays from.
void applySoundSettings(Sound& sound, ParamManager& paramManager, const SoundSettings& settings, const Song& song);

/// Writes a delay setup's four fields onto `delay` (spec §10.6), if `setup` is on: a Sound's (applySoundSettings()
/// does), or a Kit's at kit level. The sync level is the setup's note in `song`'s ticks (delaySyncLevel()), so call it
/// after applySyncDefaults(). Feedback stays as the preset saved it.
void applyDelaySetup(Delay& delay, const DelaySetup& setup, const Song& song);
#if ENABLE_SYSEX_TEST
/// `gen_sound_delay <part> <lane> sync= type= pingpong= analog= feedback=`: the fields a setup wrote and the feedback
/// home in menu units, for a sound or kit with one.
void reportDelay(const Delay& delay, int32_t feedback, const char* part, const char* lane);
/// Test builds: whether the preset preview is building an Output (spec §13.3). Its build reports none of the events
/// a part's own build does (gen_sound, gen_sound_level, ...), so a script's gen_sound expects see only the part's own
/// sounds. One build runs at a time (the runner's), so it is set around a build and read by its events.
[[nodiscard]] bool previewBuild();
void setPreviewBuild(bool preview);
#else
constexpr bool previewBuild() {
	return false;
}
#endif

/// A Sound's post-FX volume knob, which a part's `.volume` scales (spec §10.6).
AutoParam& postFxVolume(ParamManager& paramManager);

} // namespace deluge::generative::firmware
