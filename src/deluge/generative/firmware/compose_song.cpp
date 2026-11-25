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
#include "generative/core/output_settings.h"
#include "generative/firmware/log_header.h"
#include "generative/firmware/settings_tracker.h"
#include "generative/firmware/sound_controls.h"
#include "generative/perform/riser_timing.h"
#include "generative/runner/runner.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/instrument.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
#include "processing/sound/sound_instrument.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace deluge::generative::firmware {

namespace {

/// Flags `slot` of `manager`'s collection as automated, as ParamSet::paramHasAutomationNow() and patch_cable_set.cpp's
/// flagCable() do (setNodeAtPos() doesn't).
void flagAutomated(ParamManager& manager, int32_t collection, int32_t slot) {
	manager.summaries[collection].whichParamsAreAutomated[slot >> 5] |= 1U << (slot & 31);
}

} // namespace

void ComposeSong::prepare(Song* song, Roster& roster, const Genre& genre) {
	song_ = song;
	roster_ = &roster;
	genre_ = &genre;
	error_ = Error::NONE;
	notes_ = 0;
	nodes_ = 0;
	offScale_ = 0;
	host_ = nullptr;
	chunks_ = nullptr;
	palette_ = nullptr;
	piece_ = nullptr;
	rerolled_ = 0;
	songStart_ = 0;
	editCount_ = 0;
	settingEditCount_ = 0;
	settingPathCount_ = 0;
	cuts_ = newTracks_ = droppedSegments_ = droppedEdits_ = 0;
	soundsMissing_ = 0;
	editedSounds_ = false;
	beginSegments(0);
	// The homes are the loaded presets' values, as a piece's start reads them (spec §15.6: home + the macro layer).
	soundControls.begin(roster, genre);
	bound_ = true;
}

void ComposeSong::prepareCapture(const SteeringLog::Piece& piece, const SoundPicks& picks, const Palette& palette,
                                 Chunks& chunks, CaptureHost& host) {
	piece_ = &piece;
	picks_ = picks;
	playing_ = picks;
	palette_ = &palette;
	chunks_ = &chunks;
	host_ = &host;
}

void ComposeSong::restart(Song* song, Roster& roster, int32_t tick) {
	song_ = song;
	roster_ = &roster;
	songStart_ = tick;
	beginSegments(tick);
	soundControls.begin(roster, *genre_);
	bound_ = true;
	begin(Recipe{});
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

void ComposeSong::beginSegments(int32_t start) {
	segmentCount_ = 0;
	std::fill(std::begin(newest_), std::end(newest_), int16_t{-1});
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		if (roster_->parts[p].clip != nullptr) {
			addSegment(static_cast<uint8_t>(p), start, roster_->parts[p]);
		}
	}
}

bool ComposeSong::addSegment(uint8_t part, int32_t start, const PartOutput& output) {
	if (segmentCount_ == kSegmentRoom) {
		droppedSegments_++;
		return false;
	}
	Segment& segment = segments_[segmentCount_];
	segment = Segment{};
	segment.start = start;
	segment.clip = output.clip;
	segment.output = output.output;
	std::copy(std::begin(output.drums), std::end(output.drums), segment.drums);
	segment.previous = newest_[part];
	newest_[part] = static_cast<int16_t>(segmentCount_++);
	return true;
}

ComposeSong::Segment* ComposeSong::segmentAt(uint8_t part, int32_t tick) {
	int32_t i = newest_[part];
	while (i >= 0 && segments_[i].start > tick && segments_[i].previous >= 0) {
		i = segments_[i].previous;
	}
	return i >= 0 ? &segments_[i] : nullptr;
}

int32_t ComposeSong::startOf(uint8_t target) const {
	if (target >= kMaxParts || newest_[target] < 0) {
		return songStart_;
	}
	return segments_[newest_[target]].start;
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
		// The next note bounds it; with none, nothing does: the clip ends with the song (finish()).
		const int32_t longer = std::max(same->getLength(), length);
		same->setLength(i + 1 < count ? std::min(longer, notes.getElement(i + 1)->pos - tick) : longer);
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
	// A note goes in the clip its start falls in (spec §17.6): one cut there may have come since it started.
	Segment* segment = segmentAt(note.part, note.tick);
	if (segment == nullptr || segment->clip == nullptr) {
		return;
	}
	InstrumentClip* clip = segment->clip;
	NoteRow* row = nullptr;
	if (note.lane != Lane::None) {
		Drum* drum = segment->drums[perform::kitRowOf(note.lane, note.pitch)];
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
	if (!addNote(*row, note.tick - segment->start, note.length, note.velocity)) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	segment->sounding = true;
	notes_++;
}

void ComposeSong::node(const compose::Automated& automated, const compose::Node& node) {
	if (error_ != Error::NONE) {
		return;
	}
	// The part's lanes end at each cut (ClipBuilder::cutPart()), so a node is for the clip it is bound to now.
	const int32_t tick = std::max<int32_t>(node.tick - startOf(automated.part), 0);
	if (!soundControls.writeNode(automated.part, automated.lane, automated.control, automated.throwDelay, tick,
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
	return noteOn.lane == Lane::None ? 0 : sampleRowMicros(part.drums[perform::kitRowOf(noteOn.lane, noteOn.pitch)]);
}

int32_t ComposeSong::fallbackMicros(const Event& noteOn) {
	Event onFallback = noteOn;
	onFallback.pitch = perform::kRiserFallbackPitch; // kitRowOf() reads the fallback's row for a Riser
	return noteOn.lane == Lane::Riser ? sampleMicros(onFallback) : 0;
}

bool ComposeSong::finishClip(Segment& segment, int32_t position, int32_t length, bool synth) {
	InstrumentClip* clip = segment.clip;
	clip->loopLength = length;
	bool outside = false;
	for (int32_t r = 0; r < clip->noteRows.getNumElements(); r++) {
		// A note still ringing at the clip's end stops there.
		NoteRow* row = clip->noteRows.getElement(r);
		NoteVector& notes = row->notes;
		while (notes.getNumElements() > 0 && notes.getLast()->pos >= length) {
			notes.deleteAtIndex(notes.getNumElements() - 1);
		}
		if (notes.getNumElements() > 0) {
			Note* last = notes.getLast();
			last->setLength(std::min(last->getLength(), length - last->pos));
			outside = outside || (synth && !song_->isYNoteAllowed(row->y, true));
		}
	}
	// A scale-mode clip with an outside note widens the whole song's scale: this clip leaves scale mode instead
	// (spec §17.6). Only a Capture's key change brings one.
	if (outside) {
		clip->inScaleMode = false;
	}
	if (!segment.sounding) {
		return true; // nothing to play: no arranger instance
	}
	// One arranger instance of each clip, from its start to its end (spec §15.2, §17.6).
	const int32_t i = segment.output->clipInstances.insertAtKey(position);
	if (i < 0) {
		return false;
	}
	ClipInstance* instance = segment.output->clipInstances.getElement(i);
	instance->length = length;
	instance->clip = clip;
	return true;
}

void ComposeSong::end(int32_t tick, uint32_t soundingParts) {
	soundControls.repatch();
	abandon(); // before any part is freed: nothing may stay bound to it
	if (error_ != Error::NONE) {
		return;
	}
	for (int32_t p = 0; p < partCount(*genre_); p++) {
		if (newest_[p] < 0) {
			continue;
		}
		Segment& newest = segments_[newest_[p]];
		if ((soundingParts & partBit(p)) == 0 && newest.previous < 0) {
			freePartOutput(song_, roster_->parts[p]); // a part that never sounds gets no clip and no track (spec §15.3)
			newest_[p] = -1;
			continue;
		}
		const bool synth = genre_->parts[p].synth();
		int32_t next = tick;
		for (int32_t i = newest_[p]; i >= 0; i = segments_[i].previous) {
			Segment& segment = segments_[i];
			if (next > segment.start && !finishClip(segment, segment.start - songStart_, next - segment.start, synth)) {
				fail(Error::INSUFFICIENT_RAM);
				return;
			}
			next = segment.start;
		}
		if (editedSounds_ && synth) {
			// A param edit on a Sound wrote its value straight in: its patcher works it in, as a load does.
			for (int32_t i = newest_[p]; i >= 0; i = segments_[i].previous) {
				auto* sound = static_cast<SoundInstrument*>(segments_[i].output);
				sound->patcher.performInitialPatching(*sound, segments_[i].clip->paramManager);
			}
		}
	}
	song_->lastClipInstanceEnteredStartPos = 0; // opens in arranger view (spec §15.7)
	// CLIP from the session or arranger opens a clip, as in any saved song: the first part's that is left.
	for (int32_t p = 0; p < partCount(*genre_) && song_->getCurrentClip() == nullptr; p++) {
		song_->setCurrentClip(roster_->parts[p].clip);
	}
}

// ---- Capture (spec §17.6) ----

void ComposeSong::cutClip(uint8_t part, int32_t tick) {
	if (error_ != Error::NONE || newest_[part] < 0 || !genre_->parts[part].synth()) {
		return;
	}
	const Segment old = segments_[newest_[part]];
	if (old.start == tick) {
		return; // a setting-change track already opened a fresh clip on this tick
	}
	if (segmentCount_ == kSegmentRoom) {
		droppedSegments_++; // the part's notes stay in the clip they're in, out of scale mode if need be
		return;
	}
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	if (memory == nullptr) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	// The next clip on the same track: the Output's params as they are, without the old clip's automation.
	auto* clip = new (memory) InstrumentClip(song_);
	clip->loopLength = song_->getBarLength();
	clip->activeIfNoSolo = true;
	auto* instrument = static_cast<Instrument*>(old.output);
	Error error = clip->setAudioInstrument(instrument, song_, false, nullptr, old.clip);
	if (error == Error::NONE) {
		error = song_->sessionClips.insertClipAtIndex(clip, song_->sessionClips.getNumElements());
	}
	if (error != Error::NONE) {
		song_->deleteClipObject(clip, false, InstrumentRemoval::NONE);
		fail(error);
		return;
	}
	clip->arpSettings.cloneFrom(&old.clip->arpSettings);
	clip->inScaleMode = true;
	PartOutput output = roster_->parts[part];
	output.clip = clip;
	roster_->parts[part].clip = clip;
	addSegment(part, tick, output);
	soundControls.moveToClip(part, clip);
	cuts_++;
}

void ComposeSong::soundPick(uint8_t /*part*/, uint8_t /*token*/, int32_t slot, const char* path, int32_t length) {
	SoundPath* pick = slot < kMaxParts               ? &picks_.part[slot]
	                  : slot < kMaxParts + kNumLanes ? &picks_.lane[slot - kMaxParts]
	                                                 : nullptr;
	if (pick == nullptr) {
		return;
	}
	const int32_t n = std::min<int32_t>(length, kMaxSoundPath - 1);
	std::memcpy(pick->text, path, n);
	pick->text[n] = 0;
}

void ComposeSong::newTrack(uint8_t part, uint8_t /*token*/, int32_t tick) {
	if (error_ != Error::NONE || chunks_ == nullptr) {
		return;
	}
	if (segmentCount_ == kSegmentRoom) {
		droppedSegments_++; // the part plays on with its old sound
		return;
	}
	// The re-roll's picks play from here: the part's preset, or each of its lanes' samples.
	playing_.part[part] = picks_.part[part];
	for (const LaneSpec& lane : genre_->parts[part].lanes) {
		playing_.lane[static_cast<int32_t>(lane.lane)] = picks_.lane[static_cast<int32_t>(lane.lane)];
	}
	PartOutput output;
	const Error error = buildPartOutput(song_, *genre_, part, playing_, *palette_, &output, *chunks_, &soundsMissing_);
	if (error != Error::NONE) {
		fail(error);
		return;
	}
	output.clip->inScaleMode = genre_->parts[part].synth();
	roster_->parts[part] = output;
	addSegment(part, tick, output);
	// The new sound's homes are its preset's (spec §11.6); edits before the re-roll were the old sound's.
	soundControls.rebindPart(*roster_, *genre_, part);
	rerolled_ |= partBit(part);
	int32_t kept = 0;
	for (int32_t i = 0; i < editCount_; i++) {
		if (edits_[i].target != part) {
			edits_[kept++] = edits_[i];
		}
	}
	editCount_ = kept;
	int32_t keptSettings = 0;
	for (int32_t i = 0; i < settingEditCount_; i++) {
		if (settingEdits_[i].target != part) {
			settingEdits_[keptSettings++] = settingEdits_[i];
		}
	}
	settingEditCount_ = keptSettings;
	int32_t keptPaths = 0;
	for (int32_t i = 0; i < settingPathCount_; i++) {
		if (settingPaths_[i].target != part) {
			settingPaths_[keptPaths++] = settingPaths_[i];
		}
	}
	settingPathCount_ = keptPaths;
	newTracks_++;
	TEST_EVENT("gen_capture_track part=%u name=%s", static_cast<unsigned>(part), output.output->name.get());
}

void ComposeSong::tempo(const compose::TempoNode& node) {
	const int32_t tick = node.tick - songStart_;
	if (error_ != Error::NONE || tick < 0) {
		return;
	}
	// Song-level tempo automation (spec §17.6), in hundredths of a BPM as UNPATCHED_TEMPO holds it.
	ParamManager& manager = song_->paramManager;
	AutoParam& param = manager.getUnpatchedParamSet()->params[deluge::modulation::params::UNPATCHED_TEMPO];
	if (param.setNodeAtPos(tick, node.tempo, node.interpolated) < 0) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	flagAutomated(manager, 0, deluge::modulation::params::UNPATCHED_TEMPO);
	nodes_++;
}

bool ComposeSong::writeEdit(AutoParam& param, ParamManager& manager, int32_t collection, int32_t slot, int32_t start,
                            int32_t tick, int32_t from, int32_t value) {
	const int32_t at = std::max<int32_t>(tick - start, 0);
	if (!param.isAutomated() && at > 0 && param.setNodeAtPos(0, from, false) < 0) {
		return false;
	}
	if (param.setNodeAtPos(at, value, false) < 0) {
		return false;
	}
	flagAutomated(manager, collection, slot);
	nodes_++;
	return true;
}

void ComposeSong::paramEdit(const Command& edit, int32_t tick) {
	if (error_ != Error::NONE) {
		return;
	}
	const std::optional<LoggedParam> found = loggedParam(edit, *roster_, *genre_, *song_);
	if (!found) {
		return;
	}
	const int32_t start = startOf(edit.target);
	bool written = true;
	// The edit's row, then for the Riser lane the fallback Riser's row, which plays its notes in some Builds (spec
	// §9.5).
	const std::optional<LoggedParam> fallback = loggedParam(edit, *roster_, *genre_, *song_, true);
	const LoggedParam* const rows[] = {&*found, fallback ? &*fallback : nullptr};
	if (edit.kind == CommandKind::SetHome) {
		// A sound control's home (spec §11.6): the control's nodes from here are at the new home (the driver ended and
		// resumed the part's lanes around this call). A param no lane automates holds each home in turn.
		const std::optional<int32_t> old = soundControls.moveHome(edit.target, edit.lane, edit.length, edit.value);
		if (!old) {
			return;
		}
		for (const LoggedParam* at : rows) {
			if (at != nullptr && written && !at->param->isAutomated()) {
				written = writeEdit(*at->param, *at->manager, at->collection, at->slot, start, tick, *old, edit.value);
			}
		}
	}
	else {
		// A param no control moves: automation on the song or the part's clip (spec §17.6). Its value now goes with it
		// into a clip cut after.
		for (const LoggedParam* at : rows) {
			if (at != nullptr && written) {
				AutoParam& param = *at->param;
				written = writeEdit(param, *at->manager, at->collection, at->slot, start, tick, param.getCurrentValue(),
				                    edit.value);
				param.setCurrentValueBasicForSetup(edit.value);
			}
		}
		editedSounds_ = editedSounds_ || edit.target != kSongParams;
	}
	if (!written) {
		fail(Error::INSUFFICIENT_RAM);
		return;
	}
	remember(edit);
}

void ComposeSong::remember(const Command& edit) {
	for (int32_t i = 0; i < editCount_; i++) {
		Command& kept = edits_[i];
		if (kept.kind == edit.kind && kept.target == edit.target && kept.lane == edit.lane
		    && kept.length == edit.length) {
			kept.value = edit.value;
			return;
		}
	}
	if (editCount_ < kMaxEdits) {
		edits_[editCount_++] = edit;
	}
	else {
		droppedEdits_++; // a split's next song starts this param from the header's value
	}
}

void ComposeSong::startSettingTrack(uint8_t part, int32_t tick) {
	if (error_ != Error::NONE || chunks_ == nullptr || part >= partCount(*genre_) || tick <= startOf(part)) {
		return;
	}
	if (segmentCount_ == kSegmentRoom) {
		droppedSegments_++; // no room for another track: the setting applies in place on the current one
		return;
	}
	PartOutput output;
	const Error error = buildPartOutput(song_, *genre_, part, playing_, *palette_, &output, *chunks_, &soundsMissing_);
	if (error != Error::NONE) {
		fail(error);
		return;
	}
	output.clip->inScaleMode = genre_->parts[part].synth();
	roster_->parts[part] = output;
	addSegment(part, tick, output);
	// Restore the part's state up to this step: header settings & params (if not re-rolled), then prior setting and
	// param/home edits on this part alone (never re-applying Song edits onto *song_).
	Roster single{};
	single.parts[part] = output;
	if ((rerolled_ & partBit(part)) == 0 && piece_ != nullptr) {
		restoreSettings(*piece_, single, *genre_, *song_, true, false, false);
		restoreSnapshot(*piece_, single, *genre_, *song_, false);
	}
	restoreSettingsEdits(single, *song_, false);
	restoreEdits(single, *song_, false);
	soundControls.rebindPart(*roster_, *genre_, part);
	newTracks_++;
}

void ComposeSong::settingEdit(const Command& edit, int32_t tick) {
	if (error_ != Error::NONE) {
		return;
	}
	if (edit.target != kSongParams) {
		startSettingTrack(edit.target, tick);
		if (error_ != Error::NONE) {
			return;
		}
	}
	const auto setting = static_cast<OutputSetting>(edit.length);
	if (applyOutputSetting(setting, edit.value, edit.target, edit.lane, *roster_, *genre_, *song_, false)
	    && edit.target != kSongParams) {
		finalizeSoundSettings(edit.target, edit.lane, *roster_, *genre_, *song_);
	}
	rememberSetting(edit);
}

void ComposeSong::settingPath(uint8_t part, Lane lane, uint16_t setting, const char* path, int32_t length,
                              int32_t tick) {
	if (error_ != Error::NONE || part == kSongParams) {
		return;
	}
	startSettingTrack(part, tick);
	if (error_ != Error::NONE) {
		return;
	}
	const auto id = static_cast<OutputSetting>(setting);
	if (applyOutputSettingPath(id, path, length, part, lane, *roster_, *genre_, *song_)) {
		finalizeSoundSettings(part, lane, *roster_, *genre_, *song_);
	}
	rememberSettingPath(part, lane, setting, path, length);
}

void ComposeSong::rememberSetting(const Command& edit) {
	for (int32_t i = 0; i < settingEditCount_; i++) {
		Command& kept = settingEdits_[i];
		if (kept.target == edit.target && kept.lane == edit.lane && kept.length == edit.length) {
			kept.value = edit.value;
			return;
		}
	}
	if (settingEditCount_ < kMaxSettingEdits) {
		settingEdits_[settingEditCount_++] = edit;
	}
	else {
		droppedEdits_++;
	}
}

void ComposeSong::rememberSettingPath(uint8_t part, Lane lane, uint16_t setting, const char* path, int32_t length) {
	auto setCopy = [&](SoundPath& out) {
		out.clear();
		if (path != nullptr && length > 0) {
			const int32_t n = std::min<int32_t>(length, kMaxSoundPath - 1);
			std::memcpy(out.text, path, static_cast<size_t>(n));
			out.text[n] = 0;
		}
	};
	for (int32_t i = 0; i < settingPathCount_; i++) {
		KeptSettingPath& kept = settingPaths_[i];
		if (kept.target == part && kept.lane == lane && kept.setting == setting) {
			setCopy(kept.path);
			return;
		}
	}
	if (settingPathCount_ < kMaxSettingPaths) {
		KeptSettingPath& kept = settingPaths_[settingPathCount_++];
		kept.target = part;
		kept.lane = lane;
		kept.setting = setting;
		setCopy(kept.path);
	}
	else {
		droppedEdits_++;
	}
}

void ComposeSong::restoreSettingsEdits(const Roster& roster, Song& song, bool includeSong) const {
	// Apply in ascending OutputSetting order (SynthMode, OscType before filters/sample flags), then paths.
	for (int32_t id = 0; id < kNumOutputSettings; id++) {
		for (int32_t i = 0; i < settingEditCount_; i++) {
			const Command& edit = settingEdits_[i];
			if (!includeSong && edit.target == kSongParams) {
				continue;
			}
			if (edit.length == static_cast<uint16_t>(id)) {
				applyOutputSetting(static_cast<OutputSetting>(id), edit.value, edit.target, edit.lane, roster, *genre_,
				                   song, false);
			}
		}
	}
	for (int32_t i = 0; i < settingPathCount_; i++) {
		const KeptSettingPath& kept = settingPaths_[i];
		if (!includeSong && kept.target == kSongParams) {
			continue;
		}
		applyOutputSettingPath(static_cast<OutputSetting>(kept.setting), kept.path.c_str(),
		                       static_cast<int32_t>(std::strlen(kept.path.c_str())), kept.target, kept.lane, roster,
		                       *genre_, song);
	}
	for (int32_t i = 0; i < settingEditCount_; i++) {
		const Command& edit = settingEdits_[i];
		if (edit.target != kSongParams) {
			finalizeSoundSettings(edit.target, edit.lane, roster, *genre_, song);
		}
	}
	for (int32_t i = 0; i < settingPathCount_; i++) {
		const KeptSettingPath& kept = settingPaths_[i];
		if (kept.target != kSongParams) {
			finalizeSoundSettings(kept.target, kept.lane, roster, *genre_, song);
		}
	}
}

void ComposeSong::restoreEdits(const Roster& roster, Song& song, bool includeSong) const {
	for (int32_t i = 0; i < editCount_; i++) {
		if (!includeSong && edits_[i].target == kSongParams) {
			continue;
		}
		for (const bool fallback : {false, true}) { // a Riser edit's fallback row too (spec §9.5)
			if (const std::optional<LoggedParam> found = loggedParam(edits_[i], roster, *genre_, song, fallback)) {
				// A SetParam's value, or a SetHome's home: prepare()'s binding reads it as the home.
				found->param->setCurrentValueBasicForSetup(edits_[i].value);
			}
		}
	}
}

void ComposeSong::nextSong(int32_t tick) {
	if (error_ != Error::NONE || host_ == nullptr) {
		return;
	}
	const Error error = host_->nextSong(*this, tick);
	if (error != Error::NONE) {
		fail(error);
	}
}

} // namespace deluge::generative::firmware
