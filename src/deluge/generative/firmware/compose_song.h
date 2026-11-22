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

// Compose's Song adapter (spec §15): the firmware's SongWriter. It writes the Compose driver's notes and nodes into a
// private Song that buildPerformSong() made, one clip per part for the whole song (spec §15.2, §15.3), and finishes it
// for the arranger.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/compose/clip_builder.h"
#include "generative/core/steering_log.h"
#include "generative/firmware/perform_song.h"

class AutoParam;
class InstrumentClip;
class NoteRow;
class ParamManager;
class Song;

namespace deluge::generative {
class Chunks;
}

namespace deluge::generative::firmware {

class ComposeSong;

/// Where a Capture build hears that a song of a split piece is done (spec §17.5): it saves that song and frees it,
/// then builds the next song's Outputs from `writer`'s picks and hands it to `writer` with restart().
class CaptureHost {
public:
	virtual ~CaptureHost() = default;
	/// The song ended at `tick` (end() came first) and the next starts there. @return an error to stop the build.
	virtual Error nextSong(ComposeSong& writer, int32_t tick) = 0;
};

class ComposeSong final : public compose::SongWriter {
public:
	/// Before the driver's begin(): the private song and the roster buildPerformSong() made for it. Binds the parts'
	/// sound controls (soundControls, which only Perform uses otherwise) for the automation.
	void prepare(Song* song, Roster& roster, const Genre& genre);
	/// Capture (spec §17.6), after prepare(): the piece's header (for restoring header snapshots on setting-triggered
	/// new tracks), its picks, and its genre's palette, for new tracks which read the card through `chunks`; `host`
	/// hears of splits.
	void prepareCapture(const SteeringLog::Piece& piece, const SoundPicks& picks, const Palette& palette,
	                    Chunks& chunks, CaptureHost& host);
	/// A split's next song (spec §17.5), from `tick`: its Outputs built from playingPicks(), with restoreEdits()
	/// applied. Binds them as prepare() does.
	void restart(Song* song, Roster& roster, int32_t tick);
	/// The build stopped before end(): lets go of the sound controls. The song is the caller's to free.
	void abandon();

	void begin(const Recipe& recipe) override;
	void note(const compose::Note& note) override;
	void node(const compose::Automated& automated, const compose::Node& node) override;
	void base(const compose::Automated& automated, int8_t value) override;
	/// Every clip ends at `tick`; parts that never sounded are freed, and each clip left gets one arranger instance
	/// from where it starts (spec §15.2). A pitched clip with a note outside the song's scale leaves scale mode (spec
	/// §17.6).
	void end(int32_t tick, uint32_t soundingParts) override;
	int32_t sampleMicros(const Event& noteOn) override;

	// Capture's calls (spec §17.6).
	void cutClip(uint8_t part, int32_t tick) override;
	void soundPick(uint8_t part, uint8_t token, int32_t slot, const char* path, int32_t length) override;
	void newTrack(uint8_t part, uint8_t token, int32_t tick) override;
	void tempo(const compose::TempoNode& node) override;
	void paramEdit(const Command& edit, int32_t tick) override;
	void settingEdit(const Command& edit, int32_t tick) override;
	void settingPath(uint8_t part, Lane lane, uint16_t setting, const char* path, int32_t length,
	                 int32_t tick) override;
	void nextSong(int32_t tick) override;

	/// The sounds playing at a split: the header's picks, with each part's re-rolls that started a track.
	[[nodiscard]] const SoundPicks& playingPicks() const { return playing_; }
	/// Parts whose sound a re-roll replaced: the header's param snapshot isn't theirs any more.
	[[nodiscard]] uint32_t rerolledParts() const { return rerolled_; }
	/// A split's next song (or a setting-triggered new track with `includeSong = false`), before restoreEdits() and
	/// restart(): the non-param settings heard so far, each setting's newest, applied onto `roster` and (when
	/// `includeSong`) `song` (spec §17.5).
	void restoreSettingsEdits(const Roster& roster, Song& song, bool includeSong = true) const;
	/// A split's next song (or a setting-triggered new track with `includeSong = false`), before restart(): the param
	/// edits and homes heard so far, each param's newest, written as its value on `roster` and (when `includeSong`)
	/// `song` (spec §17.5).
	void restoreEdits(const Roster& roster, Song& song, bool includeSong = true) const;

	/// The first error a write met (no memory for a note or a node, or a new track's card read), or Error::NONE.
	[[nodiscard]] Error error() const { return error_; }
	/// Notes and nodes written, for H19's figures.
	[[nodiscard]] uint32_t notes() const { return notes_; }
	[[nodiscard]] uint32_t nodes() const { return nodes_; }
	/// Synth notes outside the song's scale, which got a row anyway rather than letting the firmware change the scale
	/// (spec §15.8). 0 unless the core plays out of its scale, or a Capture's key changed.
	[[nodiscard]] uint32_t offScale() const { return offScale_; }
	/// Capture: clips cut, tracks started, and cuts or tracks dropped for want of room (kMaxSegments).
	[[nodiscard]] uint32_t cuts() const { return cuts_; }
	[[nodiscard]] uint32_t newTracks() const { return newTracks_; }
	[[nodiscard]] uint32_t droppedSegments() const { return droppedSegments_; }
	/// Capture: sounds a new track couldn't load, which fell back (spec §10.1).
	[[nodiscard]] int32_t soundsMissing() const { return soundsMissing_; }

	/// The clips one song may hold past one a part: key changes and re-rolls each add one for a part (spec §17.6).
	/// (tuning)
	static constexpr int32_t kMaxSegments = 96;
	/// The param edits and homes a split song starts from (restoreEdits()): each param's newest (spec §17.5). Past
	/// it, a param's first edit isn't kept for the next song (droppedEdits()). (tuning)
	static constexpr int32_t kMaxEdits = 64;
	/// Non-param setting edits remembered for setting-triggered new tracks and split songs. (tuning)
	static constexpr int32_t kMaxSettingEdits = 64;
	static constexpr int32_t kMaxSettingPaths = 16;
	/// Capture: params whose edits restoreEdits() can't give a split's next song (kMaxEdits).
	[[nodiscard]] uint32_t droppedEdits() const { return droppedEdits_; }

private:
	/// One clip of a part: from `start` (ticks from the piece's step 0) to the next one's start or the song's end.
	struct Segment {
		int32_t start = 0;
		InstrumentClip* clip = nullptr;
		Output* output = nullptr;
		Drum* drums[kMaxLanesPerPart] = {};
		int16_t previous = -1; ///< The part's clip before, or -1
		bool sounding = false;
	};
	/// Adds a note to `row`, in position order. Notes arrive in the order they end, so one may start before a note
	/// already there: a note never overlaps the next on its row, so the earlier one is cut where the later starts.
	bool addNote(NoteRow& row, int32_t tick, int32_t length, int32_t velocity);
	void fail(Error error);
	/// Starts the segments: one a part, from `start`, each the roster's clip.
	void beginSegments(int32_t start);
	bool addSegment(uint8_t part, int32_t start, const PartOutput& output);
	/// The clip `part`'s note at `tick` goes in: the newest that starts at or before it.
	Segment* segmentAt(uint8_t part, int32_t tick);
	/// Finishes one clip, `length` ticks long, and gives it its arranger instance at `position`.
	bool finishClip(Segment& segment, int32_t position, int32_t length, bool synth);
	/// Writes `value` from `tick` on `param`, whose clip starts at `start`: a held node at the clip's start first, if
	/// it had none, so the clip plays its old value before `tick`.
	bool writeEdit(AutoParam& param, ParamManager& manager, int32_t collection, int32_t slot, int32_t start,
	               int32_t tick, int32_t from, int32_t value);
	/// Where `target`'s clip starts now: a part's newest segment, or the song's start for the song's params.
	[[nodiscard]] int32_t startOf(uint8_t target) const;
	void remember(const Command& edit);
	void rememberSetting(const Command& edit);
	void rememberSettingPath(uint8_t part, Lane lane, uint16_t setting, const char* path, int32_t length);
	/// Starts a new track for `part` at `tick` on a mid-piece non-param setting change, carrying over its current
	/// preset picks, header snapshot (if not re-rolled), and prior setting/param/home edits (spec §17.6).
	void startSettingTrack(uint8_t part, int32_t tick);

	struct KeptSettingPath {
		uint8_t target = 0;
		Lane lane = Lane::None;
		uint16_t setting = 0;
		SoundPath path;
	};

	Song* song_ = nullptr;
	Roster* roster_ = nullptr;
	const Genre* genre_ = nullptr;
	Error error_ = Error::NONE;
	uint32_t notes_ = 0;
	uint32_t nodes_ = 0;
	uint32_t offScale_ = 0;
	bool bound_ = false;

	// Capture.
	CaptureHost* host_ = nullptr;
	Chunks* chunks_ = nullptr;
	const Palette* palette_ = nullptr;
	const SteeringLog::Piece* piece_ = nullptr;
	SoundPicks picks_;   ///< The picks re-rolls have logged, by slot
	SoundPicks playing_; ///< The picks whose tracks play
	uint32_t rerolled_ = 0;
	int32_t songStart_ = 0;
	/// Each part's first clip, then the kMaxSegments more cuts and re-rolls start.
	static constexpr int32_t kSegmentRoom = kMaxSegments + kMaxParts;
	Segment segments_[kSegmentRoom];
	int32_t segmentCount_ = 0;
	int16_t newest_[kMaxParts] = {};
	Command edits_[kMaxEdits];
	int32_t editCount_ = 0;
	Command settingEdits_[kMaxSettingEdits];
	int32_t settingEditCount_ = 0;
	KeptSettingPath settingPaths_[kMaxSettingPaths];
	int32_t settingPathCount_ = 0;
	uint32_t cuts_ = 0;
	uint32_t newTracks_ = 0;
	uint32_t droppedSegments_ = 0;
	uint32_t droppedEdits_ = 0;
	int32_t soundsMissing_ = 0;
	bool editedSounds_ = false;
};

} // namespace deluge::generative::firmware
