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

#include "generative/firmware/compose_song.h"
#include "generative/firmware/sound_controls.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/song/song.h"

namespace deluge::generative::firmware {

void ComposeSong::prepare(Song* song, Roster& roster, const Genre& genre) {
	song_ = song;
	roster_ = &roster;
	genre_ = &genre;
	error_ = Error::NONE;
	notes_ = 0;
	nodes_ = 0;
	offScale_ = 0;
	// The homes are the loaded presets' values, as a piece's start reads them (spec §15.6: home + the macro layer).
	soundControls.begin(roster, genre);
	bound_ = true;
}

void ComposeSong::abandon() {
	if (bound_) {
		soundControls.forget();
		bound_ = false;
	}
}

void ComposeSong::fail(Error error) {
	if (error_ == Error::NONE) {
		error_ = error;
	}
}

void ComposeSong::begin(const Recipe& /*recipe*/) {
	// buildPerformSong() has set the tempo, swing, key and scale from the Recipe (spec §15.5), the key first, before
	// any row. Synth clips are in scale mode, Kit clips aren't (spec §15.4).
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		PartOutput& part = roster_->parts[p];
		if (part.clip != nullptr) {
			part.clip->inScaleMode = genre_->parts[p].synth();
		}
	}
}

bool ComposeSong::addNote(NoteRow& row, int32_t tick, int32_t length, int32_t velocity) {
	NoteVector& notes = row.notes;
	const int32_t count = notes.getNumElements();
	int32_t i = notes.search(tick, GREATER_OR_EQUAL);
	if (i < count && notes.getElement(i)->pos == tick) {
		// Two notes start together on one row (a re-strike that lands on its own note-off): the longer stands.
		Note* same = notes.getElement(i);
		const int32_t next = i + 1 < count ? notes.getElement(i + 1)->pos - tick : length;
		same->setLength(std::min(std::max(same->getLength(), length), next));
		same->setVelocity(std::max<int32_t>(same->getVelocity(), velocity));
		return true;
	}
	if (i > 0) {
		Note* before = notes.getElement(i - 1);
		if (before->pos + before->getLength() > tick) {
			before->setLength(tick - before->pos);
		}
	}
	if (i < count) {
		length = std::min(length, notes.getElement(i)->pos - tick);
	}
	if (notes.insertAtIndex(i) != Error::NONE) {
		return false;
	}
	Note* note = notes.getElement(i);
	note->pos = tick;
	note->setLength(std::max<int32_t>(length, 1));
	note->setVelocity(velocity);
	note->setLift(kDefaultLiftValue);
	// Fixed, exactly as Perform plays it (spec §15.4).
	note->setProbability(kNumProbabilityValues);
	note->setIterance(kDefaultIteranceValue);
	note->setFill(FillMode::OFF);
	return true;
}

void ComposeSong::note(const compose::Note& note) {
	if (error_ != Error::NONE) {
		return;
	}
	PartOutput& part = roster_->parts[note.part];
	InstrumentClip* clip = part.clip;
	if (clip == nullptr) {
		return;
	}
	NoteRow* row = nullptr;
	if (note.lane != Lane::None) {
		Drum* drum = part.drums[laneRow(note.lane)];
		row = drum != nullptr ? clip->getNoteRowForDrum(drum) : nullptr;
		if (row == nullptr) {
			return; // a lane with nothing loaded plays nothing, as in Perform
		}
	}
	else {
		row = clip->getNoteRowForYNote(note.pitch);
		if (row == nullptr) {
			// Not getOrCreateNoteRowForYNote(): for a note outside the scale it changes the song's scale (spec §15.8).
			// The core plays in its scale, so this is counted, and the note keeps a row of its own.
			if (!song_->isYNoteAllowed(note.pitch, true)) {
				offScale_++;
			}
			row = clip->noteRows.insertNoteRowAtY(note.pitch);
			if (row == nullptr) {
				fail(Error::INSUFFICIENT_RAM);
				return;
			}
		}
	}
	if (!addNote(*row, note.tick, note.length, note.velocity)) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	notes_++;
}

void ComposeSong::node(const compose::Automated& automated, const compose::Node& node) {
	if (error_ != Error::NONE) {
		return;
	}
	if (!soundControls.writeNode(automated.part, automated.lane, automated.control, automated.throwDelay, node.tick,
	                             node.value, node.interpolated)) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	nodes_++;
}

void ComposeSong::base(const compose::Automated& automated, int8_t value) {
	soundControls.writeBase(automated.part, automated.lane, automated.control, automated.throwDelay, value);
}

int32_t ComposeSong::sampleMicros(const Event& noteOn) {
	const PartOutput& part = roster_->parts[noteOn.part];
	return noteOn.lane == Lane::None ? 0 : sampleRowMicros(part.drums[laneRow(noteOn.lane)]);
}

void ComposeSong::end(int32_t tick, uint32_t soundingParts) {
	soundControls.repatch();
	abandon(); // before any part is freed: nothing may stay bound to it
	if (error_ != Error::NONE) {
		return;
	}
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		PartOutput& part = roster_->parts[p];
		if (part.clip == nullptr) {
			continue;
		}
		if ((soundingParts & (1U << p)) == 0) {
			freePartOutput(song_, part); // a part that never sounds gets no clip and no track (spec §15.3)
			continue;
		}
		InstrumentClip* clip = part.clip;
		clip->loopLength = tick; // one clip for the whole song (spec §15.2)
		for (int32_t r = 0; r < clip->noteRows.getNumElements(); r++) {
			// A note still ringing at the end stops there.
			NoteVector& notes = clip->noteRows.getElement(r)->notes;
			while (notes.getNumElements() > 0 && notes.getLast()->pos >= tick) {
				notes.deleteAtIndex(notes.getNumElements() - 1);
			}
			if (notes.getNumElements() > 0) {
				Note* last = notes.getLast();
				last->setLength(std::min(last->getLength(), tick - last->pos));
			}
		}
		// One arranger instance of each clip, from the start to the end (spec §15.2).
		const int32_t i = part.output->clipInstances.insertAtKey(0);
		if (i < 0) {
			fail(Error::INSUFFICIENT_RAM);
			return;
		}
		ClipInstance* instance = part.output->clipInstances.getElement(i);
		instance->length = tick;
		instance->clip = clip;
	}
	song_->lastClipInstanceEnteredStartPos = 0; // opens in arranger view (spec §15.7)
	// CLIP from the session or arranger opens a clip, as in any saved song: the first part's that is left.
	for (int32_t p = 0; p < partCount(*genre_) && song_->getCurrentClip() == nullptr; p++) {
		song_->setCurrentClip(roster_->parts[p].clip);
	}
}

} // namespace deluge::generative::firmware
