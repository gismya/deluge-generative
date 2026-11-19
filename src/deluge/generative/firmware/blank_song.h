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

// Swapping songs in stopped, as stock Clear song does on its stopped path (gui/context_menu/clear_song.cpp): leaving
// Perform onto a blank song (spec §13.8), Perform's song (spec §14.3), a Compose song (spec §15.7), and the blank song
// a Compose build that fits only alone lands on when it frees the player's song (spec §15.9).

#pragma once

#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/global_effectable/global_effectable.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <new>

namespace deluge::generative::firmware {

/// Ends playback and disarms RECORD: a song is swapped in stopped (spec §14.3, §15.7). RECORD armed in a stock view
/// would stay on, and Perform can't turn it off (F9).
inline void stopForSwap() {
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback();
	}
	playbackHandler.recording = RecordingMode::OFF;
	playbackHandler.setLedStates();
}

/// Deletes the current song: its undo log, the UIs' pointers into it, then the song. Nothing may yield from here to
/// swapInSong()'s end: nothing may run without a current song but the card loader's waits, as in Clear song.
/// Playback must have ended.
inline void deleteCurrentSong() {
	actionLogger.deleteAllLogs();
	nullifyUIs();
	deleteOldSongBeforeLoadingNew();
}

/// Swaps `song` in, after deleteCurrentSong(), and clears the loading mode and "Loading" animation the swap leaves up,
/// as Clear song does after it: else it would cover the screen over the new song (COMPOSING, after a Compose build
/// frees the player's song) and stay up. The caller sets the UI for the new song.
inline void swapInSong(Song* song) {
	preLoadedSong = song; // a built song only now: a launch event would swap in any non-null preLoadedSong
	playbackHandler.doSongSwap(false);
	audioFileManager.deleteAnyTempRecordedSamplesFromMemory(); // the old song's, as Clear song does
	currentUIMode = UI_MODE_NONE;
	display->removeWorkingAnimation();
}

/// Deletes the current song and swaps in a new blank one, in session view, as SHIFT+LOAD → NEW builds it.
/// `whileNoSong()` runs between the delete and the new song (freeing what the old song left, so the new one lands in
/// the same place every time). Playback must have ended; the caller sets the UI for the new song after.
template <typename WhileNoSong>
void swapInBlankSong(WhileNoSong whileNoSong) {
	deleteCurrentSong();
	whileNoSong();

	void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	if (songMemory == nullptr) {
		// As Clear song: the old song is gone and the UIs are nullified, so there's nothing to resume into.
		FREEZE_WITH_ERROR("E458");
		while (true) {}
	}
	// In preLoadedSong from the start, as Clear song: the preset load reads its tempo for default sync levels.
	preLoadedSong = new (songMemory) Song();
	preLoadedSong->paramManager.setupUnpatched();
	GlobalEffectable::initParams(&preLoadedSong->paramManager);
	preLoadedSong->setupDefault();
	preLoadedSong->inClipMinderViewOnLoad = false; // session view, not stock NEW's clip view
	preLoadedSong->ensureAtLeastOneSessionClip();  // loads a synth preset from the card
	swapInSong(preLoadedSong);
	currentSong->loadAllSamples();
}

} // namespace deluge::generative::firmware
