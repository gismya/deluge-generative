// What specs share to steer a piece and read its stream (spec §21.2): commands, genres and filters that used to be
// copied from spec to spec. harness.h runs the piece.

#pragma once

#include "harness.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace harness {

// Commands.

/// A press on a MUTE pad: lands on the next bar line, or with `now` (SHIFT held) on the next undecided step.
inline Command mute(uint8_t part, bool now = false) {
	return {CommandKind::SetMute, part, now ? kPressNow : kPress};
}
/// A forced Section, from the next phrase line.
inline Command force(SectionKind kind) {
	return {CommandKind::ForceSection, static_cast<uint8_t>(kind), 0};
}

/// Plays Techno from its first Groove (seed 3), so every part plays from bar 1, pushing each command when the driver
/// is about to decide its step.
inline Result playSteered(int32_t bars, std::vector<std::pair<int32_t, Command>> commands, SteeringLog* log = nullptr) {
	Run run;
	run.recipe = technoRecipe();
	run.seed = 3;
	run.skipIntro = true;
	run.log = log;
	run.steer = steerAt(std::move(commands));
	return play(run, bars);
}

// Genres.

/// Techno with a fixed Build length.
template <uint8_t Bars>
struct BuildOf {
	static constexpr WeightedBars kBars[] = {{Bars, 1}};
	static inline const Genre genre = [] {
		Genre g = genres::techno;
		g.arc.buildBars = kBars;
		return g;
	}();
};

/// A Bass of long notes for a spec's own genre: one every 2 bars, lasting `Steps`, its part gated at `Gate` points.
template <int32_t Steps, int32_t Gate = 0>
struct LongBassParts {
	static constexpr Pattern kEveryTwoBars[] = {
	    {"X...............................", "................................", 0}};
	static constexpr DegreeWeight kRoot[] = {{0, 1}};
	static constexpr PartSpec parts[] = {{
	    .role = Role::Bass,
	    .flavour = Flavour::Loop,
	    .pool = kEveryTwoBars,
	    .densityGate = macroPoints(Gate),
	    .loop = {.degrees = kRoot, .octaveUp = 0, .accent = 0, .slide = 0, .gateTicks = Steps * kTicksPerStep},
	}};
};

// Reading a stream.

/// Note-ons of `part` (and `lane`, unless Lane::None stands for any) from step `from` up to `to`.
inline std::vector<Event> ons(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == part && (lane == Lane::None || e.lane == lane) && e.tick >= stepTick(from)
		    && e.tick < stepTick(to)) {
			out.push_back(e);
		}
	}
	return out;
}

/// The ticks any of `events` is on.
inline std::set<int32_t> ticksOf(const std::vector<Event>& events) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		out.insert(e.tick);
	}
	return out;
}

/// The first tick at or after `from` a Section of `kind` starts, or -1.
inline int32_t sectionStart(const Result& result, SectionKind kind, int32_t from = 0) {
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.section == kind && e.tick >= from) {
			return e.tick;
		}
	}
	return -1;
}

/// The sound-control events.
inline std::vector<Event> controls(const Result& result) {
	return only(result.events, EventKind::Control);
}

/// The last Control event for (part, lane, control) at or before `tick`, or nullptr (home).
inline const Event* lastAt(const Result& result, uint8_t part, Lane lane, SoundControl control, int32_t tick) {
	const Event* last = nullptr;
	for (const Event& e : result.events) {
		if (e.kind == EventKind::Control && e.tick <= tick && e.part == part && e.lane == lane
		    && e.control == control) {
			last = &e;
		}
	}
	return last;
}
/// The last offset sent for (part, lane, control) at or before `tick`, or 0 (home) if none was.
inline int32_t offsetAt(const Result& result, uint8_t part, Lane lane, SoundControl control, int32_t tick) {
	const Event* e = lastAt(result, part, lane, control, tick);
	return e != nullptr ? e->offset : 0;
}

/// A note-on the mute gate closed: sent with its muted flag (spec §12.5).
inline Event mutedNoteOn(int32_t tick, uint8_t part, Lane lane, uint8_t pitch) {
	Event e = Event::noteOn(tick, part, lane, pitch, 100);
	e.muted = true;
	return e;
}

} // namespace harness
