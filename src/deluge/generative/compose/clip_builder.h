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

// Compose's output shape (spec §15.3, §15.4, §15.6): the event stream, as the Perform driver would play it, turned
// into what a song stores. NoteOn and NoteOff pair into notes with lengths, muted notes drop out, and Control events
// become automation nodes, thinned. Portable, so the harness checks the same code the firmware runs; a SongWriter
// (the firmware's builds the Song) takes the result.

#pragma once

#include "generative/core/capacity.h"
#include "generative/core/event.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/sounding_notes.h"
#include "generative/core/steering.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative::compose {

/// One clip note: a NoteOn paired with its NoteOff. Fixed, exactly as Perform plays it (spec §15.4): the firmware
/// writes it at probability 100, with no iterance and no fill flags.
struct Note {
	int32_t tick = 0;       ///< Generator ticks (24 per 16th, the song's ticks at magnitude 2) from the song's start.
	int32_t length = 0;     ///< Ticks, at least 1.
	uint8_t part = 0;       ///< The part's index in the genre's roster: its clip.
	Lane lane = Lane::None; ///< A Drums or FX part's lane: its row. Lane::None on a pitched part.
	uint8_t pitch = 0;      ///< A pitched part's absolute MIDI note.
	uint8_t velocity = 0;   ///< 1–127.
	constexpr bool operator==(const Note&) const = default;
};

/// What one automation lane moves (spec §15.6): a sound control on a part (Lane::None: a synth part, or a Kit's
/// kit-wide set) or on a Drums or FX lane's row, or, with `throwDelay`, the delay Throw's feedback lift on that set
/// (Event::throwDelay(), spec §11.4), which rides on the Space events. The firmware maps each to the params of the
/// engine it lands on (spec §11.2): FM Brightness becomes the modulator 1 and 2 volumes.
struct Automated {
	uint8_t part = 0;
	Lane lane = Lane::None;
	SoundControl control = SoundControl::Brightness;
	bool throwDelay = false;
	constexpr bool operator==(const Automated&) const = default;
};

/// One automation node, in the control's own units: the offset from home, -kControlFull..kControlFull, or for a
/// throwDelay lane the lift in percent, 0–100.
struct Node {
	int32_t tick = 0;
	int8_t value = 0;
	/// The firmware's ParamNode flag: the value moves in a straight line from the lane's previous node to this one.
	/// Otherwise it holds the previous node's value and jumps here.
	bool interpolated = false;
	constexpr bool operator==(const Node&) const = default;
};

/// A node on the song's tempo automation (spec §17.6), in hundredths of a BPM, as Node is for a control.
struct TempoNode {
	int32_t tick = 0;
	int32_t tempo = 0;
	bool interpolated = false;
	constexpr bool operator==(const TempoNode&) const = default;
};

/// Where Compose writes (spec §2.5): the firmware's adapter builds a private Song (spec §15.7), the harness records.
/// The calls come in this order: begin(), then notes and nodes as the song plays out (a part's notes in the order they
/// end, each lane's nodes in tick order), then a base() per automation lane the stream touched, then end().
class SongWriter {
public:
	virtual ~SongWriter() = default;
	/// Before anything else: the song's tempo, swing, key and scale are the Recipe's, set once (spec §15.5). The song
	/// key goes first, since a row the firmware makes for a note can change the scale (spec §15.8).
	virtual void begin(const Recipe& recipe) = 0;
	/// A finished note for its part's clip: Synth clips in scale mode, Kit clips not (spec §15.4).
	virtual void note(const Note& note) = 0;
	/// A node on `lane`'s automation. A lane that never moves gets none (spec §15.6), and the firmware flags a lane
	/// that gets one as automated, since setNodeAtPos() doesn't.
	virtual void node(const Automated& automated, const Node& node) = 0;
	/// The stored value of each automation lane the stream touched, as an offset from home: the macro layer at the
	/// Recipe's centre for a lane that moves, its one value for a lane that doesn't (spec §15.6). A control the stream
	/// never touched stays at home.
	virtual void base(const Automated& automated, int8_t value) = 0;
	/// The song ends at `tick`, the end of the Outro: each clip is that long, from the Intro's start (spec §15.2,
	/// §15.3). `soundingParts` has bit p set when part p played a note; a part that never sounds gets no clip and no
	/// track. A Drums or FX clip gets a row for every lane its part has, silent ones too.
	virtual void end(int32_t tick, uint32_t soundingParts) = 0;
	/// How long `noteOn`'s sound plays by itself, in microseconds: a sample's length, 0 for a synth. Asked only for a
	/// Riser in a Build, which then starts so its file ends on the Drop (spec §9.5, §10.5), as Perform's does.
	virtual int32_t sampleMicros(const Event& /*noteOn*/) { return 0; }

	// ---- Capture's replay (spec §17.5, §17.6). A Compose song gets none of these. ----
	// Ticks are from the piece's step 0. Each comes when the replay reaches it, among the notes and nodes; a note or
	// node goes to the clip and the song its tick falls in.

	/// `part`'s clip ends at `tick`, where a key or scale change lands, and its next clip runs on from there on the
	/// same track (spec §17.6). Pitched parts only: Drums and FX clips are never cut.
	virtual void cutClip(uint8_t /*part*/, int32_t /*tick*/) {}
	/// One of the picks of `part`'s sound `token`, from its re-roll (spec §17.2): its slot (pickSlot()) and path,
	/// `length` chars, not terminated, valid only during the call. Comes on the step its SetSound landed on, before
	/// newTrack().
	virtual void soundPick(uint8_t /*part*/, uint8_t /*token*/, int32_t /*slot*/, const char* /*path*/,
	                       int32_t /*length*/) {}
	/// From `tick` on, `part` plays sound `token`, from the picks soundPick() gave it: a new track, and the part's clip
	/// on its old track ends there (spec §17.6). A swap a transition's confirm dropped never comes here: the part keeps
	/// its old track.
	virtual void newTrack(uint8_t /*part*/, uint8_t /*token*/, int32_t /*tick*/) {}
	/// A node on the song's tempo automation (spec §17.6), in tick order. The song's own tempo is begin()'s Recipe's.
	virtual void tempo(const TempoNode& /*node*/) {}
	/// A player's param edit from `tick` on (spec §17.2, §17.6): a SetParam, automation on the song or on the part's
	/// clip, or a SetHome, a sound control's new home.
	virtual void paramEdit(const Command& /*edit*/, int32_t /*tick*/) {}
	/// Stock's stutter starts (`on`) or ends at `tick` (SetStutter, spec §17.2).
	virtual void stutter(bool /*on*/, int32_t /*tick*/) {}
	/// A split (spec §17.5): end() has ended one song at `tick`, and the next starts there, with that Section and the
	/// replayed state at that step. Its notes, nodes, bases and end() follow.
	virtual void nextSong(int32_t /*tick*/) {}
};

/// Turns the event stream into notes and nodes for a SongWriter, as the events would play: feed it every event in
/// play order (Generator::popDue()), then finish() once at the song's end. The pairing and the node thinning are
/// here; the Compose driver steps the core and feeds it.
class ClipBuilder {
public:
	/// How far, in a control's units, a bar whose curve bends this little may stray from a straight line across it:
	/// such a bar keeps only its end, so drift gets at most one node a bar (spec §15.6). (tuning)
	static constexpr int32_t kBarTolerance = 16;
	/// How far the line through the kept points may stray from them: bar ends on a curve this straight merge into one
	/// node, and a ramp that bends more within a bar keeps enough of its step lines to follow it (spec §15.6). (tuning)
	static constexpr int32_t kNodeTolerance = 2;
	/// One automation lane per control and set, and one more per set for the Throw's delay lift on Space.
	static constexpr int32_t kTracksPerSet = kNumSoundControls + 1;
	static constexpr int32_t kMaxTracks = kMaxControlSets * kTracksPerSet;

	void begin(SongWriter& writer);
	/// One event, in play order. A Riser's note-on moves its tick before it comes here (ComposeDriver).
	void take(const Event& event);
	/// The song ends at `tick`, after the core's flush has given its last note-offs: every lane's last node is written,
	/// before `tick`, then each lane's base(), then end(). `centre(lane)` is the base for a lane that moves.
	template <typename Centre>
	void finish(int32_t tick, Centre centre) {
		for (int32_t i = 0; i < counts_.sets * kTracksPerSet; i++) {
			Track& track = tracks_[i];
			if (track.started) {
				track.finish(*writer_, tick);
				writer_->base(track.id, track.moved ? centre(track.id) : track.value);
			}
		}
		for (const Open& open : open_) {
			counts_.unpaired += open.used ? 1 : 0;
		}
		writer_->end(tick, counts_.sounding);
	}

	/// A split (spec §17.5): the song ends at `tick`, a Section line, as finish() ends it, and the next song starts
	/// there. A note still sounding (a Sustain's, held across the line) ends here and is struck again at `tick`, at its
	/// velocity; each lane goes on in the next song from the value it has here.
	template <typename Centre>
	void split(int32_t tick, Centre centre) {
		for (Open& open : open_) {
			if (open.used && tick > open.noteOn.tick) {
				const Event& on = open.noteOn;
				writer_->note({on.tick, tick - on.tick, on.part, on.lane, on.pitch, on.velocity});
				counts_.sounding |= partBit(on.part);
			}
		}
		for (int32_t i = 0; i < counts_.sets * kTracksPerSet; i++) {
			Track& track = tracks_[i];
			if (track.started) {
				track.finish(*writer_, tick);
				writer_->base(track.id, track.moved ? centre(track.id) : track.value);
			}
		}
		writer_->end(tick, counts_.sounding);
		writer_->nextSong(tick);
		counts_.sounding = 0;
		for (Open& open : open_) {
			open.noteOn.tick = open.used ? tick : open.noteOn.tick;
		}
		for (int32_t i = 0; i < counts_.sets * kTracksPerSet; i++) {
			Track& track = tracks_[i];
			if (track.started) {
				Track next;
				next.id = track.id;
				next.started = true;
				next.value = track.value;
				next.endTick = tick;
				next.origin = tick;
				track = next;
			}
		}
	}

	/// Notes that found no room for their note-on, or whose note-off never came by finish(): 0 unless the core broke
	/// its note-off promise (spec §2.2). A note-off with no note-on is a muted note's and isn't counted.
	[[nodiscard]] uint32_t unpaired() const { return counts_.unpaired; }
	/// Control sets that found no room, their lanes not written: 0 unless a roster outgrows kMaxControlSets, which
	/// rosterFits() rules out.
	[[nodiscard]] uint32_t droppedSets() const { return counts_.dropped; }

private:
	struct Open {
		Event noteOn;
		bool used = false;
	};
	/// A point on a lane's curve.
	struct Point {
		int32_t tick = 0;
		int8_t value = 0;
	};
	/// A line's slope, value per tick: num / den, den above 0.
	struct Slope {
		int32_t num = 0;
		int32_t den = 1;
		[[nodiscard]] bool below(const Slope& other) const;
	};
	/// One automation lane: the curve Perform plays (value at each step line, jumps on snaps) and the thinning's
	/// state. Its value is 0 (home) until its first event.
	struct Track {
		Automated id;
		bool started = false; ///< It has had a value away from home.
		bool moved = false;   ///< It has written a node: it moves.
		int8_t value = 0;     ///< The value the curve has reached at `endTick`.
		int32_t endTick = 0;  ///< Where the played curve ends so far.
		int32_t origin = 0;   ///< Where its song starts: 0, or a split's line (spec §17.5).
		// The thinning, first a bar at a time: the bar's points (ramp ends, and a ramp's start after a hold under a
		// bar), each on a step line, from `from`, the last point the cone took, under a bar before the bar's first.
		uint8_t barCount = 0;
		int32_t barStart = 0; ///< The tick of the bar line the buffered points come after.
		Point from;
		struct BarPoint {
			uint8_t step; ///< Steps from barStart, 1 to kStepsPerBar.
			int8_t value;
		};
		BarPoint bar[kStepsPerBar];
		// Then a cone: from the anchor, the last node written, a straight line to `last` passes within the tolerance of
		// every point in between, while the line's slope stays in [lo, hi].
		bool pending = false; ///< `last` is a point not yet written.
		Point anchor;
		Point last;
		Slope lo;
		Slope hi;

		/// A new value on the curve: a jump at `tick` (snap) or a ramp reaching it a step after `tick`.
		void move(SongWriter& writer, int32_t tick, int8_t value, bool snap);
		/// The song ends at `tick`: the last node is written, a tick before it at the latest.
		void finish(SongWriter& writer, int32_t tick);

	private:
		/// A ramp's end, on a step line: buffered until its bar ends.
		void point(SongWriter& writer, Point p);
		/// The buffered bar goes to the cone: only its last point if the bar stays within kBarTolerance of the line to
		/// it, else every point.
		void endBar(SongWriter& writer);
		/// A point the cone's line may pass through.
		void conePoint(SongWriter& writer, Point p);
		void write(SongWriter& writer, Point p, bool interpolated);
		/// The pending point gets its node, a tick before `before` at the latest.
		void flushPending(SongWriter& writer, int32_t before);
	};

	void noteOn(const Event& event);
	void noteOff(const Event& event);
	void control(const Event& event);
	/// The lane `id` on the set of `part` and `lane`, started if `value` leaves home; nullptr if it stays home or its
	/// set finds no room.
	Track* track(uint8_t part, Lane lane, SoundControl control, bool throwDelay, int8_t value);

	/// Set slots from 1 by part and lane (Lane::None last); 0 for none yet. A set's lanes are tracks_[(slot - 1) *
	/// kTracksPerSet], one per control, then the delay lift.
	static constexpr int32_t kLaneSlots = kNumLanes + 1;
	struct Counts {
		int32_t sets = 0;
		uint32_t sounding = 0;
		uint32_t unpaired = 0;
		uint32_t dropped = 0;
	};

	SongWriter* writer_ = nullptr;
	Open open_[SoundingNotes::kTotal];
	uint8_t setSlot_[kMaxParts][kLaneSlots] = {};
	Track tracks_[kMaxTracks];
	Counts counts_;
};

} // namespace deluge::generative::compose
