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

// A sample Riser ends on the Drop (spec §9.5, §10.5): where its note starts, from the sample's length and the tempo
// planned across the Build. Driver-side, like tempo (spec §2.2): the core never sees it.

#pragma once

#include "generative/core/event.h"
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include <algorithm>
#include <cstdint>

namespace deluge::generative::perform {

/// Microseconds one 16th lasts at a tempo of 1 hundredth of a BPM: 60 s × 100 / 4 16ths a beat.
constexpr int32_t kStepMicrosAtOneHundredth = 1'500'000'000;

/// How long one 16th lasts at `tempo` (hundredths of a BPM), in microseconds. A tempo under kMinTempo (a Glide still
/// at 0) counts as kMinTempo: a beat of 16ths then fits 32 bits.
constexpr int32_t stepMicros(int32_t tempo) {
	return kStepMicrosAtOneHundredth / std::max<int32_t>(tempo, kMinTempo);
}

/// Walks the beats from `to` back to `from`, a beat at a time at the tempo `tempoAt(tick)` plans for the beat's middle,
/// handing each `visit(line, ticks, beatMicros)`: the beat's later line, how many of its ticks lie after `from`, and a
/// whole beat's length in microseconds. Stops when `visit` returns false. A planned glide moves slowly enough that this
/// is within a millisecond or so, and outside a glide it is exact. Integer math; one divide a beat.
template <typename TempoAt, typename Visit>
void walkBeats(int32_t from, int32_t to, TempoAt tempoAt, Visit visit) {
	constexpr int32_t kBeatTicks = kStepsPerBar / 4 * kTicksPerStep;
	for (int32_t line = to; line > from; line -= kBeatTicks) {
		const int32_t beat = stepMicros(tempoAt(line - kBeatTicks / 2)) * (kStepsPerBar / 4);
		if (!visit(line, std::min<int32_t>(kBeatTicks, line - from), beat)) {
			return;
		}
	}
}

/// The tick a sample `micros` long starts on so that it ends on `dropTick`, at the tempo `tempoAt(tick)` plans for each
/// tick (walkBeats()); never before `earliest`, the note's own tick, so a sample longer than that starts there.
template <typename TempoAt>
int32_t riserStart(int32_t earliest, int32_t dropTick, int32_t micros, TempoAt tempoAt) {
	constexpr int32_t kBeatTicks = kStepsPerBar / 4 * kTicksPerStep;
	int32_t left = micros;
	int32_t start = earliest; // longer than the Build: it starts with it, as a synth Riser does
	walkBeats(earliest, dropTick, tempoAt, [&](int32_t line, int32_t /*ticks*/, int32_t beat) {
		if (beat >= left) {
			const int64_t into = (int64_t{left} * kBeatTicks + beat / 2) / beat;
			start = std::max<int32_t>(earliest, line - static_cast<int32_t>(into));
			return false;
		}
		left -= beat;
		return true;
	});
	return start;
}

/// The time from `from` to `to`, in microseconds, as riserStart() plans it (walkBeats()).
template <typename TempoAt>
int64_t plannedMicros(int32_t from, int32_t to, TempoAt tempoAt) {
	constexpr int32_t kBeatTicks = kStepsPerBar / 4 * kTicksPerStep;
	int64_t total = 0;
	walkBeats(from, to, tempoAt, [&](int32_t /*line*/, int32_t ticks, int32_t beat) {
		total += int64_t{beat} * ticks / kBeatTicks;
		return true;
	});
	return total;
}

// ---- The fallback Riser (spec §9.5, §10.5) ----

/// A pack Riser a Riser lane can fall back to, and its length.
struct PackRiser {
	const char* path;
	int32_t ms;
};

/// The pack's short Risers, longest first. riser_fit.py checks each length against its WAV.
inline constexpr PackRiser kPackRisers[] = {
    {"SAMPLES/FX/Riser/GEN Riser 2.wav", 6000},
    {"SAMPLES/FX/Riser/GEN Riser 3.wav", 4000},
    {"SAMPLES/FX/Riser/Short/GEN Riser Short.wav", 2000},
};

/// The fallback a Riser lane loads beside a drawn sample `drawnMicros` long: the longest pack short Riser shorter than
/// it, or none.
constexpr const PackRiser* fallbackFor(int32_t drawnMicros) {
	for (const PackRiser& riser : kPackRisers) {
		if (int64_t{riser.ms} * 1000 < drawnMicros) {
			return &riser;
		}
	}
	return nullptr;
}

/// The FX Kit's row the fallback loads into, after the FX lanes' rows.
constexpr int32_t kRiserFallbackRow = kNumFxLanes;
static_assert(kRiserFallbackRow < kMaxLanesPerPart);

/// A Riser note-on's pitch when the driver gives it the fallback (a Kit lane's notes otherwise carry 0).
constexpr uint8_t kRiserFallbackPitch = 1;

/// The Kit row a laned note plays on: its lane's, or the fallback's for a Riser note the driver gave it.
constexpr int32_t kitRowOf(Lane lane, uint8_t pitch) {
	return lane == Lane::Riser && pitch == kRiserFallbackPitch ? kRiserFallbackRow : laneRow(lane);
}

/// Which sample a Riser plays in a Build.
enum class RiserPick : uint8_t {
	Drawn,    ///< the lane's own: it fits
	Fallback, ///< the lane's own is longer than the Build, the fallback fits
	None,     ///< neither fits: no Riser in this Build
};

/// Which sample a sample Riser plays from `earliest` (the Build's first step) to `dropTick` (spec §9.5): the drawn one
/// `drawnMicros` long if it fits the time planned between them, else the fallback `fallbackMicros` long (0: none
/// loaded) if it fits, else none. Decided once, at the Build's first step, from lengths known then: no draw, so
/// Perform, Compose and Capture's replay agree wherever they plan the same tempo.
template <typename TempoAt>
RiserPick pickRiser(int32_t earliest, int32_t dropTick, int32_t drawnMicros, int32_t fallbackMicros, TempoAt tempoAt) {
	const int64_t build = plannedMicros(earliest, dropTick, tempoAt);
	if (drawnMicros <= build) {
		return RiserPick::Drawn;
	}
	// The fallback is shorter than the drawn sample (fallbackFor()), so one that fits is never the longer.
	return fallbackMicros > 0 && fallbackMicros <= build ? RiserPick::Fallback : RiserPick::None;
}

/// Picks `noteOn`'s sample (pickRiser()) for a sample Riser `micros` long with a fallback `fallbackMicros` long, its
/// Drop on `dropTick`: marks the note-on for the fallback's row and gives `micros` its length where that plays. False
/// when no Riser fits, so the note isn't played. Perform's driver and Compose's share it.
template <typename TempoAt>
bool pickRiserSample(Event& noteOn, int32_t dropTick, int32_t& micros, int32_t fallbackMicros, TempoAt tempoAt) {
	switch (pickRiser(noteOn.tick, dropTick, micros, fallbackMicros, tempoAt)) {
	case RiserPick::Drawn:
		return true;
	case RiserPick::Fallback:
		noteOn.pitch = kRiserFallbackPitch;
		micros = fallbackMicros;
		return true;
	case RiserPick::None:
		break;
	}
	return false;
}

} // namespace deluge::generative::perform
