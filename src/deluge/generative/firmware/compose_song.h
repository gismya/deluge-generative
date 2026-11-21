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
#include "generative/firmware/perform_song.h"

class NoteRow;
class Song;

namespace deluge::generative::firmware {

class ComposeSong final : public compose::SongWriter {
public:
	/// Before the driver's begin(): the private song and the roster buildPerformSong() made for it. Binds the parts'
	/// sound controls (soundControls, which only Perform uses otherwise) for the automation.
	void prepare(Song* song, Roster& roster, const Genre& genre);
	/// The build stopped before end(): lets go of the sound controls. The song is the caller's to free.
	void abandon();

	void begin(const Recipe& recipe) override;
	void note(const compose::Note& note) override;
	void node(const compose::Automated& automated, const compose::Node& node) override;
	void base(const compose::Automated& automated, int8_t value) override;
	/// Every clip ends at `tick`; parts that never sounded are freed, and each clip left gets one arranger instance
	/// from the start (spec §15.2).
	void end(int32_t tick, uint32_t soundingParts) override;
	int32_t sampleMicros(const Event& noteOn) override;

	/// The first error a write met (no memory for a note or a node), or Error::NONE.
	[[nodiscard]] Error error() const { return error_; }
	/// Notes and nodes written, for H19's figures.
	[[nodiscard]] uint32_t notes() const { return notes_; }
	[[nodiscard]] uint32_t nodes() const { return nodes_; }
	/// Synth notes outside the song's scale, which got a row anyway rather than letting the firmware change the scale
	/// (spec §15.8). 0 unless the core plays out of its scale.
	[[nodiscard]] uint32_t offScale() const { return offScale_; }

private:
	/// Adds a note to `row`, in position order. Notes arrive in the order they end, so one may start before a note
	/// already there: a note never overlaps the next on its row, so the earlier one is cut where the later starts.
	bool addNote(NoteRow& row, int32_t tick, int32_t length, int32_t velocity);
	void fail(Error error);

	Song* song_ = nullptr;
	Roster* roster_ = nullptr;
	const Genre* genre_ = nullptr;
	Error error_ = Error::NONE;
	uint32_t notes_ = 0;
	uint32_t nodes_ = 0;
	uint32_t offScale_ = 0;
	bool bound_ = false;
};

} // namespace deluge::generative::firmware
