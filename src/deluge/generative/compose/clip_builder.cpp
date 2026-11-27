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
#include <algorithm>
#include <iterator>

namespace deluge::generative::compose {

namespace {

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

} // namespace

bool ClipBuilder::Slope::below(const Slope& other) const {
	return int64_t{num} * other.den < int64_t{other.num} * den;
}

void ClipBuilder::begin(SongWriter& writer) {
	writer_ = &writer;
	for (Open& open : open_) {
		open.used = false;
	}
	for (auto& slots : setSlot_) {
		std::fill(std::begin(slots), std::end(slots), 0);
	}
	counts_ = Counts{};
	std::fill(std::begin(partStart_), std::end(partStart_), 0);
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
	case EventKind::Pump:     // Compose plays no punch-ins (spec §8.9)
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
	counts_.unpaired++;
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
			counts_.sounding |= partBit(on.part);
		}
		return;
	}
	// A muted note's note-off: its note-on never came here.
}

void ClipBuilder::control(const Event& event) {
	const auto offset = static_cast<int16_t>(event.controlOffset());
	if (Track* lane = track(event.part, event.lane, event.control, false, offset)) {
		lane->move(*writer_, event.tick, offset, event.snap);
	}
	// The control's extra lane: Space's delay lift, Brightness's close past -kControlFull. It starts only once it
	// leaves 0, so a set no Throw lifts or no handover closes gets none (spec §15.6).
	if (event.control == SoundControl::Space || event.control == SoundControl::Brightness) {
		const auto extra =
		    static_cast<int16_t>(event.control == SoundControl::Space ? event.throwDelay() : event.closeExtra());
		if (Track* lane = track(event.part, event.lane, event.control, true, extra)) {
			lane->move(*writer_, event.tick, extra, event.snap);
		}
	}
}

ClipBuilder::Track* ClipBuilder::track(uint8_t part, Lane lane, SoundControl control, bool extra, int16_t value) {
	uint8_t& slot = setSlot_[part][lane == Lane::None ? kNumLanes : static_cast<int32_t>(lane)];
	if (slot == 0) {
		if (value == 0) {
			return nullptr; // a lane starts at home: only a value away from it starts one
		}
		if (counts_.sets == kMaxControlSets) {
			counts_.dropped++;
			return nullptr;
		}
		slot = static_cast<uint8_t>(++counts_.sets);
		for (int32_t i = 0; i < kTracksPerSet; i++) {
			tracks_[(slot - 1) * kTracksPerSet + i] = Track{};
		}
	}
	const int32_t index = !extra                           ? static_cast<int32_t>(control)
	                      : control == SoundControl::Space ? kNumSoundControls
	                                                       : kNumSoundControls + 1;
	Track& track = tracks_[(slot - 1) * kTracksPerSet + index];
	if (!track.started) {
		if (value == 0) {
			return nullptr;
		}
		track.started = true;
		track.id = {part, lane, control, extra};
		// A lane that first leaves home after a split or a cut on its part starts there, not at the piece's start.
		track.origin = partStart_[part];
		track.endTick = partStart_[part];
	}
	return &track;
}

void ClipBuilder::Track::move(SongWriter& writer, int32_t tick, int16_t to, bool snap) {
	if (to == value) {
		return; // a Space event whose other lane moved
	}
	if (!moved) {
		if (tick == origin && snap) {
			value = to; // the song's first step sets it: a base, not a move
			return;
		}
		// It moves: the lane starts from its value so far, held from the song's start.
		moved = true;
		write(writer, {origin, value}, false);
	}
	if (snap) {
		// It holds its value up to the line, then jumps: a ramp that ended on the line has its node a tick before.
		endBar(writer);
		flushPending(writer, tick);
		write(writer, {tick, to}, false);
	}
	else {
		if (endTick < tick) {
			// Held since the last change, then the ramp across this step. After a hold of a bar or more the ramp's
			// start goes to the cone itself, after the bar so far: a bar measured from a point before the hold could
			// drop it, and the held bars would climb to the ramp. A shorter hold, such as drift's stalls, stays in the
			// bar, so drift keeps a node a bar.
			if (tick - endTick >= kBarTicks) {
				endBar(writer);
				conePoint(writer, {tick, value});
			}
			else {
				point(writer, {tick, value});
			}
		}
		point(writer, {tick + kTicksPerStep, to});
	}
	value = to;
	endTick = snap ? tick : tick + kTicksPerStep;
}

void ClipBuilder::Track::finish(SongWriter& writer, int32_t tick) {
	endBar(writer);
	flushPending(writer, tick); // a ramp on the Outro's last step ends on the song's end, outside the clip
}

void ClipBuilder::Track::point(SongWriter& writer, Point p) {
	// A point on a bar line ends that bar.
	const int32_t start = (p.tick - 1) / kBarTicks * kBarTicks;
	if (barCount > 0 && start != barStart) {
		endBar(writer);
	}
	barStart = start;
	bar[barCount++] = {static_cast<uint8_t>((p.tick - start) / kTicksPerStep), p.value};
}

void ClipBuilder::Track::endBar(SongWriter& writer) {
	if (barCount == 0) {
		return;
	}
	const auto pointOf = [&](const BarPoint& b) { return Point{barStart + b.step * kTicksPerStep, b.value}; };
	const Point end = pointOf(bar[barCount - 1]);
	const int32_t span = end.tick - from.tick;
	const int32_t rise = end.value - from.value;
	bool straight = true;
	for (int32_t i = 0; i < barCount - 1 && straight; i++) {
		const Point p = pointOf(bar[i]);
		// |p - line(p)| <= tolerance, scaled by the line's span.
		const int64_t off = int64_t{p.value - from.value} * span - int64_t{rise} * (p.tick - from.tick);
		const int64_t room = int64_t{kBarTolerance} * span;
		straight = off <= room && -off <= room;
	}
	for (int32_t i = straight ? barCount - 1 : 0; i < barCount; i++) {
		conePoint(writer, pointOf(bar[i]));
	}
	barCount = 0;
}

void ClipBuilder::Track::conePoint(SongWriter& writer, Point p) {
	const Slope line{p.value - anchor.value, p.tick - anchor.tick};
	if (pending) {
		// The line from the anchor to this point must pass every point since within the tolerance.
		if (!line.below(lo) && !hi.below(line)) {
			last = p;
			const Slope low{line.num - kNodeTolerance, line.den};
			const Slope high{line.num + kNodeTolerance, line.den};
			lo = lo.below(low) ? low : lo;
			hi = high.below(hi) ? high : hi;
			from = p;
			return;
		}
		// It can't: the last point is a corner, so it gets its node and the line starts again from it.
		write(writer, last, true);
	}
	pending = true;
	last = p;
	const int32_t den = p.tick - anchor.tick;
	lo = {p.value - anchor.value - kNodeTolerance, den};
	hi = {p.value - anchor.value + kNodeTolerance, den};
	from = p;
}

void ClipBuilder::Track::write(SongWriter& writer, Point p, bool interpolated) {
	writer.node(id, {p.tick, p.value, interpolated});
	pending = false;
	from = p;
	anchor = p;
}

void ClipBuilder::Track::flushPending(SongWriter& writer, int32_t before) {
	if (!pending) {
		return;
	}
	// A node can't share its tick with the jump after it: it moves a tick earlier.
	write(writer, {std::min(last.tick, before - 1), last.value}, true);
}

} // namespace deluge::generative::compose
