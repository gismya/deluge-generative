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

// The Generator core's shared vocabulary: time units, roles, lanes, Section kinds and scales (spec §2, §5).
//
// Everything under generative/core/ is portable C++. It includes no firmware headers, never allocates, has no
// globals and uses integer math only for anything that can change the event stream (ADR 0004). The same sources
// build into the firmware and into the tests/ harness.

#pragma once

#include <cstdint>

namespace deluge::generative {

// ---- Time (spec §12.1, ADR 0003) ----

/// The Generator's fixed unit: 24 ticks per 16th, whatever the song's tick magnitude.
constexpr int32_t kTicksPerStep = 24;
/// Each step is decided this many ticks before its grid line.
constexpr int32_t kLookaheadTicks = 12;
/// Lean runs from -kMaxLean to +kMaxLean ticks.
constexpr int32_t kMaxLean = 12;
constexpr int32_t kStepsPerBeat = 4;
constexpr int32_t kStepsPerBar = 16; ///< Every v1 genre is 4/4 on a 16th grid (spec §4.3).
constexpr int32_t kTicksPerBeat = kStepsPerBeat * kTicksPerStep;
constexpr int32_t kTicksPerBar = kStepsPerBar * kTicksPerStep;

/// The song tick magnitude whose ticks are Generator ticks: 6 << 2 = 24 per 16th. Perform, Compose and Capture
/// songs are created at it (spec §12.3).
constexpr int32_t kGeneratorTickMagnitude = 2;
static_assert((6 << kGeneratorTickMagnitude) == kTicksPerStep);

/// How many values low..high holds, at least 1.
constexpr int32_t inclusiveSpan(int32_t low, int32_t high) {
	return high >= low ? high - low + 1 : 1;
}

/// The tick of a step's grid line.
constexpr int32_t stepTick(int32_t step) {
	return step * kTicksPerStep;
}
/// The tick at which a step is decided.
constexpr int32_t decisionTick(int32_t step) {
	return stepTick(step) - kLookaheadTicks;
}

// ---- Parts and roles (spec §5.1) ----

/// Hard limit on a genre's roster. The fixed state block and the six MUTE pads both assume it.
constexpr int32_t kMaxParts = 6;

/// The musical job a part does. Declared in the order Generator code uses for "earlier" (spec §5.5).
enum class Role : uint8_t { Drums, Bass, Chords, Pad, Lead, FX };
constexpr int32_t kNumRoles = 6;

/// Voice-start order within a step (spec §12.6): 0 starts first. Overflow drops from the highest rank down
/// (spec §2.7), so it doubles as the reverse drop order.
constexpr int32_t voiceStartRank(Role role) {
	switch (role) {
	case Role::Drums:
		return 0;
	case Role::Bass:
		return 1;
	case Role::Lead:
		return 2;
	case Role::Chords:
		return 3;
	case Role::Pad:
		return 4;
	case Role::FX:
		return 5;
	}
	return 5;
}

/// The note-making method within a role (spec §5.2). Drums and FX have none.
enum class Flavour : uint8_t { None, Loop, Arp, Motif, Stab, Sustain };

// ---- Lanes (spec §5.4) ----

/// Lane kinds, Drums lanes then FX lanes, each group in its fixed Kit row order.
enum class Lane : uint8_t {
	Kick,
	Snare,
	Clap,
	Rim,
	ClosedHat,
	OpenHat,
	Ride,
	Crash,
	Tom,
	Perc1,
	Perc2,
	Riser,
	Fall,
	Impact,
	Siren,
	None = 0xFF, ///< A pitched part's events.
};
constexpr int32_t kNumDrumLanes = 11;
constexpr int32_t kNumFxLanes = 4;
constexpr int32_t kNumLanes = kNumDrumLanes + kNumFxLanes;
/// The most lanes one part can have.
constexpr int32_t kMaxLanesPerPart = kNumDrumLanes;
/// Chords and Pad play at most 4 notes (spec §5.8).
constexpr int32_t kMaxChordNotes = 4;

constexpr bool isFxLane(Lane lane) {
	return lane != Lane::None && static_cast<int32_t>(lane) >= kNumDrumLanes;
}
/// The lane's row in its part's Kit: every Kit carries one NoteRow per lane of its kind, in the fixed order, even
/// for lanes the genre leaves silent (spec §5.3, §12.8).
constexpr int32_t laneRow(Lane lane) {
	return isFxLane(lane) ? static_cast<int32_t>(lane) - kNumDrumLanes : static_cast<int32_t>(lane);
}
/// How many rows a Kit part's Kit has: every Drums lane, or every FX lane.
constexpr int32_t kitRows(bool fx) {
	return fx ? kNumFxLanes : kNumDrumLanes;
}
/// The lane on a Kit part's row.
constexpr Lane laneAtRow(bool fx, int32_t row) {
	return static_cast<Lane>(fx ? kNumDrumLanes + row : row);
}

// ---- Sections (spec §9.1) ----

enum class SectionKind : uint8_t { Intro, Groove, Break, Build, Outro };
constexpr int32_t kNumSectionKinds = 5;

/// A Section kind's name, as the OLED and test events show it.
constexpr const char* sectionName(SectionKind kind) {
	constexpr const char* kNames[kNumSectionKinds] = {"INTRO", "GROOVE", "BREAK", "BUILD", "OUTRO"};
	return kNames[static_cast<int32_t>(kind)];
}

// ---- Sound controls (spec §11.1) ----

/// The six sound controls: the only way the Generator changes a sound live. Each moves a list of params around their
/// homes, a list the driver works out per engine (spec §11.2).
enum class SoundControl : uint8_t { Brightness, Decay, Drive, Space, Wobble, Thin };
constexpr int32_t kNumSoundControls = 6;
/// A control's offset from home runs from -kControlFull to kControlFull: percent of its range on that side, so
/// kControlFull reaches the end of each target's range (spec §11.2). The core clamps to it.
constexpr int32_t kControlFull = 100;

// ---- Scales (spec §3.2, §7.2) ----

/// The 12 seven-note preset scales. Every one sits on the 7-degree grid unchanged.
enum class Scale : uint8_t {
	Major,
	Minor,
	Dorian,
	Phrygian,
	Lydian,
	Mixolydian,
	Locrian,
	MelodicMinor,
	HarmonicMinor,
	HungarianMinor,
	Marva,
	Arabian,
};
constexpr int32_t kNumScales = 12;
constexpr int32_t kDegreesPerScale = 7;

/// Semitones above the root of each degree.
constexpr uint8_t kScaleSemitones[kNumScales][kDegreesPerScale] = {
    {0, 2, 4, 5, 7, 9, 11}, // Major
    {0, 2, 3, 5, 7, 8, 10}, // Minor
    {0, 2, 3, 5, 7, 9, 10}, // Dorian
    {0, 1, 3, 5, 7, 8, 10}, // Phrygian
    {0, 2, 4, 6, 7, 9, 11}, // Lydian
    {0, 2, 4, 5, 7, 9, 10}, // Mixolydian
    {0, 1, 3, 5, 6, 8, 10}, // Locrian
    {0, 2, 3, 5, 7, 9, 11}, // Melodic Minor
    {0, 2, 3, 5, 7, 8, 11}, // Harmonic Minor
    {0, 2, 3, 6, 7, 8, 11}, // Hungarian Minor
    {0, 1, 4, 6, 7, 9, 11}, // Marva
    {0, 2, 4, 5, 6, 8, 10}, // Arabian
};

/// Whether a pitch class (0–11) is in `scale` on `root` (0 = C).
constexpr bool inScale(int32_t pitchClass, int32_t root, Scale scale) {
	int32_t fromRoot = ((pitchClass - root) % 12 + 12) % 12;
	for (uint8_t semitone : kScaleSemitones[static_cast<int32_t>(scale)]) {
		if (semitone == fromRoot) {
			return true;
		}
	}
	return false;
}

} // namespace deluge::generative
