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

// Tracks non-param sound-editor settings on Perform's Song, parts, and Kit rows (spec §17.2, §17.6): detects changes
// in bounded per-frame work from PerformView::graphicsRoutine(), logs sparse kSettingsTag (0x92) header blocks and
// mid-piece SetSetting / kSettingPathTag (0x85) records, and applies settings back for Capture and AS IT BEGAN.

#pragma once

#include "generative/core/output_settings.h"
#include "generative/core/recipe.h"
#include "generative/core/steering_log.h"
#include "generative/firmware/perform_song.h"
#include <cstdint>

class Song;

namespace deluge::generative::firmware {

/// Applies one scalar OutputSetting to the Song, part Output, or Kit row in `roster`/`song`.
/// With `songIsCurrent`, Song Reverb settings also update AudioEngine::reverb immediately (Perform / AS IT BEGAN).
bool applyOutputSetting(OutputSetting setting, int32_t value, uint8_t target, Lane lane, const Roster& roster,
                        const Genre& genre, Song& song, bool songIsCurrent);

/// Applies one path OutputSetting (Osc0SamplePath / Osc1SamplePath) in task context (reads from card).
bool applyOutputSettingPath(OutputSetting setting, const char* path, int32_t length, uint8_t target, Lane lane,
                            const Roster& roster, const Genre& genre, Song& song);

/// Re-patches any Sound whose patching depends on settings changed during a batch apply (SynthMode, OscType, filters).
void finalizeSoundSettings(uint8_t target, Lane lane, const Roster& roster, const Genre& genre, Song& song);

/// Applies every kSettingsTag (0x92) header block in `piece` onto `roster` and (when `includeSong`) `song`.
/// Set `loadPaths` true only in task context (Capture, BeganJob); false in the tick (RestartBegan::restore()).
/// @return the number of settings written.
int32_t restoreSettings(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre, Song& song,
                        bool loadPaths, bool songIsCurrent, bool includeSong = true);

/// Resets Song-level non-param settings (globalEffectable FX/filters/compressor/stutter and Reverb) to a fresh Song's
/// defaults (for AS IT BEGAN when Song settings were edited without a header block).
void resetSongSettings(Song& song, bool songIsCurrent);

/// Per-frame bounded change tracker and pending log buffer for Perform's non-param settings (lives in .sdram_bss).
class SettingsTracker {
public:
	static constexpr int32_t kMaxSlots = 1 + kMaxParts + kNumLanes; ///< Song + parts + Kit rows
	static constexpr int32_t kMaxPendingScalars = 32;
	static constexpr int32_t kMaxPendingPaths = 4;

	/// Entry: captures baseline for Song + all parts + all Kit rows; clears edited masks and pending edits.
	void resetAll(const Roster& roster, const Genre& genre, Song& song);
	/// Transition: captures baseline for the new roster's parts and Kit rows; leaves Song's edits intact.
	void resetRoster(const Roster& roster, const Genre& genre);
	/// Sound swap or AS IT BEGAN rebuild: captures baseline for `part` and its Kit rows; drops its pending edits.
	void resetPart(int32_t part, const Roster& roster, const Genre& genre);
	/// Leave / dropSong: marks every slot invalid and clears pending edits.
	void forget();

	/// Bounded per-frame poll from PerformView::graphicsRoutine(): checks at most 2 slots (the SoundEditor target or
	/// focused part/Song, plus 1 round-robin slot) so frame time stays a few microseconds.
	void poll(const Roster& roster, const Genre& genre, Song& song, bool playing, int32_t focusedPart);
	/// Full sweep of all valid slots (piece start, piece stop, before logHeader, before AS IT BEGAN press).
	void scanAll(const Roster& roster, const Genre& genre, Song& song, bool playing);
	/// Single-slot sweep of the Song (slot 0, tick-safe, no allocation) before AS IT BEGAN checks songNeedsReset().
	void scanSong(const Roster& roster, const Genre& genre, Song& song);

	/// Clears pending mid-piece edits (called when a new piece opens: its header already holds the live state).
	void clearPending();
	/// In the tick (OutputSink::logSettings()): writes pending SetSetting and SettingPath (0x85) records on `step`.
	void drainToLog(SteeringLog& log, int32_t step);
	/// In the tick (logPieceHeader()): sweeps live state and writes a sparse kSettingsTag (0x92) block per edited slot.
	void logHeader(SteeringLog& log, const Roster& roster, const Genre& genre, Song& song);

	/// AS IT BEGAN: whether `part` has any edited setting not covered by `piece`'s header blocks, or any edited path
	/// setting (live or in header), so BeganJob must rebuild it from its preset in task context.
	[[nodiscard]] bool partNeedsRebuild(const SteeringLog::Piece& piece, int32_t part) const;
	/// AS IT BEGAN: whether the Song has any edited setting not covered by `piece`'s header blocks.
	[[nodiscard]] bool songNeedsReset(const SteeringLog::Piece& piece) const;
	/// AS IT BEGAN: refreshes seen values from live state and sets each slot's editedMask to match `piece`'s header.
	void syncToHeader(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre, Song& song);

private:
	struct Slot {
		int32_t values[kNumOutputSettings] = {};
		uint32_t pathHash[2] = {};
		uint16_t pathLen[2] = {};
		uint64_t editedMask = 0;
		uint8_t output = 0;
		Lane lane = Lane::None;
		bool valid = false;
	};
	struct PendingPath {
		uint8_t target = 0;
		Lane lane = Lane::None;
		uint16_t setting = 0;
		SoundPath path;
	};

	void captureSlot(int32_t slotIndex, uint8_t output, Lane lane, const Roster& roster, const Genre& genre,
	                 Song* song);
	void scanSlot(int32_t slotIndex, const Roster& roster, const Genre& genre, Song& song, bool playing);
	int32_t prioritySlot(const Roster& roster, const Genre& genre, Song& song, int32_t focusedPart) const;
	void enqueueScalar(uint8_t target, Lane lane, OutputSetting setting, int32_t value);
	void enqueuePath(uint8_t target, Lane lane, OutputSetting setting, const char* path, int32_t length);

	Slot slots_[kMaxSlots];
	int32_t pollCursor_ = 0;
	Command pendingScalars_[kMaxPendingScalars];
	int32_t pendingScalarCount_ = 0;
	PendingPath pendingPaths_[kMaxPendingPaths];
	int32_t pendingPathCount_ = 0;
};

extern SettingsTracker settingsTracker;

} // namespace deluge::generative::firmware
