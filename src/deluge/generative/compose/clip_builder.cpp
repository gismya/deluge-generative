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

#include "generative/compose/clip_builder.h"

namespace deluge::generative::compose {

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

/// Whether a/b < c/d, for b and d above 0.
bool slopeBelow(int32_t a, int32_t b, int32_t c, int32_t d) {
	return int64_t{a} * d < int64_t{c} * b;
}

} // namespace

void ClipBuilder::begin(SongWriter& writer) {
	writer_ = &writer;
	for (Open& open : open_) {
		open.used = false;
	}
	trackCount_ = 0;
	sounding_ = 0;
	unpaired_ = 0;
}

void ClipBuilder::take(const Event& event) {
	switch (event.kind) {
	case EventKind::NoteOn:
		if (!event.muted) {
			noteOn(event); // a muted or soloed-out part's note stays out of the song (spec §2.5)
		}
		break;
	case EventKind::NoteOff:
		noteOff(event);
		break;
	case EventKind::Control:
		control(event);
		break;
	case EventKind::Section:  // the arranger has no markers (spec §15.2)
	case EventKind::KeyScale: // the song's key is the Recipe's, set once (spec §15.5); Capture cuts clips on it
	case EventKind::Sound:    // Compose plays no sound re-roll; Capture cuts clips on it (spec §15.3)
		break;
	}
}

void ClipBuilder::noteOn(const Event& event) {
	for (Open& open : open_) {
		if (!open.used) {
			open = {event, true};
			return;
		}
	}
	unpaired_++;
}

void ClipBuilder::noteOff(const Event& event) {
	for (Open& open : open_) {
		if (!open.used || !open.noteOn.sameKey(event)) {
			continue;
		}
		open.used = false;
		const Event& on = open.noteOn;
		// A sample Riser that would start at or after its own note-off never sounds (spec §9.5).
		if (event.tick > on.tick) {
			writer_->note({on.tick, event.tick - on.tick, on.part, on.lane, on.pitch, on.velocity});
			sounding_ |= 1U << on.part;
		}
		return;
	}
	// A muted note's note-off: its note-on never came here.
}

void ClipBuilder::control(const Event& event) {
	const Automated id{event.part, event.lane, event.control, false};
	moveTrack(id, event.tick, event.offset, event.snap);
	if (event.control == SoundControl::Space) {
		moveTrack({event.part, event.lane, event.control, true}, event.tick, static_cast<int8_t>(event.throwDelay()),
		          event.snap);
	}
}

void ClipBuilder::moveTrack(const Automated& id, int32_t tick, int8_t value, bool snap) {
	for (int32_t i = 0; i < trackCount_; i++) {
		if (tracks_[i].id == id) {
			move(tracks_[i], tick, value, snap);
			return;
		}
	}
	// A lane starts at home: only a value away from it starts one.
	if (value == 0 || trackCount_ == kMaxTracks) {
		return; // kMaxTracks holds every set the largest roster has: rosterFits() rules out more
	}
	Track& track = tracks_[trackCount_++];
	track = Track{};
	track.id = id;
	move(track, tick, value, snap);
}

void ClipBuilder::move(Track& track, int32_t tick, int8_t value, bool snap) {
	if (value == track.value) {
		return; // a Space event whose other lane moved, or a lane's first event at home
	}
	if (!track.moved) {
		if (tick == 0) {
			track.value = value; // the piece's first step sets it: a base, not a move
			return;
		}
		// It moves: the lane starts from its value so far, held from the song's start.
		track.moved = true;
		write(track, 0, track.value, false);
	}
	if (snap) {
		// It holds its value up to the line, then jumps: a ramp that ended on the line has its node a tick before.
		endBar(track);
		flushPending(track, tick);
		write(track, tick, value, false);
	}
	else {
		if (track.endTick < tick) {
			point(track, tick, track.value); // held since the last change, then the ramp across this step
		}
		point(track, tick + kTicksPerStep, value);
	}
	track.value = value;
	track.endTick = snap ? tick : tick + kTicksPerStep;
}

void ClipBuilder::point(Track& track, int32_t tick, int8_t value) {
	// A point on a bar line ends that bar.
	const int32_t barStart = (tick - 1) / kBarTicks * kBarTicks;
	if (track.barCount > 0 && barStart != track.barStart) {
		endBar(track);
	}
	track.barStart = barStart;
	track.bar[track.barCount++] = {static_cast<uint8_t>((tick - barStart) / kTicksPerStep), value};
}

void ClipBuilder::endBar(Track& track) {
	if (track.barCount == 0) {
		return;
	}
	const auto tickOf = [&](const Track::Point& p) { return track.barStart + p.step * kTicksPerStep; };
	const Track::Point& end = track.bar[track.barCount - 1];
	const int32_t span = tickOf(end) - track.fromTick;
	const int32_t rise = end.value - track.fromValue;
	bool straight = true;
	for (int32_t i = 0; i < track.barCount - 1 && straight; i++) {
		const Track::Point& p = track.bar[i];
		// |p - line(p)| <= tolerance, scaled by the line's span.
		// A held lane's span can run back to the song's start: 64-bit.
		const int64_t off = int64_t{p.value - track.fromValue} * span - int64_t{rise} * (tickOf(p) - track.fromTick);
		const int64_t room = int64_t{kBarTolerance} * span;
		straight = off <= room && -off <= room;
	}
	for (int32_t i = straight ? track.barCount - 1 : 0; i < track.barCount; i++) {
		const int32_t tick = tickOf(track.bar[i]);
		conePoint(track, tick, track.bar[i].value);
		track.fromTick = tick;
		track.fromValue = track.bar[i].value;
	}
	track.barCount = 0;
}

void ClipBuilder::conePoint(Track& track, int32_t tick, int8_t value) {
	const int32_t tolerance = kNodeTolerance;
	if (track.pending) {
		// The line from the anchor to this point must pass every point since within the tolerance.
		const int32_t num = value - track.anchorValue;
		const int32_t den = tick - track.anchorTick;
		if (!slopeBelow(num, den, track.loNum, track.loDen) && !slopeBelow(track.hiNum, track.hiDen, num, den)) {
			track.lastTick = tick;
			track.lastValue = value;
			if (slopeBelow(track.loNum, track.loDen, num - tolerance, den)) {
				track.loNum = num - tolerance;
				track.loDen = den;
			}
			if (slopeBelow(num + tolerance, den, track.hiNum, track.hiDen)) {
				track.hiNum = num + tolerance;
				track.hiDen = den;
			}
			return;
		}
		// It can't: the last point is a corner, so it gets its node and the line starts again from it.
		write(track, track.lastTick, track.lastValue, true);
	}
	track.pending = true;
	track.lastTick = tick;
	track.lastValue = value;
	const int32_t den = tick - track.anchorTick;
	track.loNum = value - track.anchorValue - tolerance;
	track.loDen = den;
	track.hiNum = value - track.anchorValue + tolerance;
	track.hiDen = den;
}

void ClipBuilder::write(Track& track, int32_t tick, int8_t value, bool interpolated) {
	writer_->node(track.id, {tick, value, interpolated});
	track.pending = false;
	track.fromTick = tick;
	track.fromValue = value;
	track.anchorTick = tick;
	track.anchorValue = value;
}

void ClipBuilder::flushPending(Track& track, int32_t before) {
	if (!track.pending) {
		return;
	}
	// A node can't share its tick with the jump after it: it moves a tick earlier.
	const int32_t tick = track.lastTick >= before ? before - 1 : track.lastTick;
	write(track, tick, track.lastValue, true);
}

} // namespace deluge::generative::compose
