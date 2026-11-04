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

// Generative mode POC: the portable Generator core's public interface.
//
// This is the contract between the core (src/deluge/generative/core/), its drivers (the Perform hook in the
// firmware, the tests/ harness) and nothing else. The core is portable C++: it includes no firmware headers,
// never allocates, never touches the card, and has no globals. All state lives inside a Generator object.
//
// POC scope: one genre (Techno), two parts (Drums with four lanes, Bass), the Energy and Density macros,
// a fixed seed and take 0. No Arc, sound controls, interplay, chords, ornaments or Compose.
// Vocabulary: GLOSSARY.md. Design: docs/adr/0002, docs/adr/0003, and the closed tickets on issue #1.

#pragma once

#include <cstdint>

namespace deluge::generative {

/// Ticks per 16th step at the Deluge's default 96 PPQN.
constexpr int32_t kTicksPerStep = 24;
/// The Generator decides each step this many ticks before its grid line (ADR 0003).
constexpr int32_t kLookaheadTicks = 12;
/// Lean runs from -kMaxLean to +kMaxLean ticks.
constexpr int32_t kMaxLean = 12;
constexpr int32_t kStepsPerBar = 16;

/// The parts in this POC's Techno roster, in role-priority order.
enum class PartId : uint8_t { Drums = 0, Bass = 1 };
constexpr int32_t kNumParts = 2;

/// Drum lanes, in the fixed order the Drums kit's NoteRows use (row 0 = Kick).
/// Within a step, note-ons go out in this order (Kick first, ADR 0002 voice-start order).
enum class Lane : uint8_t { Kick = 0, Clap = 1, ClosedHat = 2, OpenHat = 3 };
constexpr int32_t kNumLanes = 4;

/// The seven-note scales this POC can play. Interval tables live in the core.
enum class Scale : uint8_t { Minor = 0, Phrygian = 1, Dorian = 2 };

/// Macro range: 0–50, as in GLOSSARY.md.
constexpr uint8_t kMacroMax = 50;

/// What the Generator should make. Holds no notes. The seed sits beside it, not inside it.
struct Recipe {
	uint8_t key = 0; ///< Root pitch class, 0 = C ... 11 = B.
	Scale scale = Scale::Minor;
	uint8_t energy = 25;     ///< 0–50. How hard, bright and driven the notes are.
	uint8_t density = 25;    ///< 0–50. How many notes play.
	uint8_t complexity = 25; ///< 0–50. Unused in the POC beyond what the core chooses.
	uint8_t motion = 25;     ///< 0–50. How much the loop mutates over phrases.
};

/// Where the Generator's notes go. The Perform driver forwards them to each part's Output; the harness prints them.
/// Calls arrive in the order they must be played: within one tick, note-offs first, then note-ons in
/// role priority (Drums lanes in Lane order, then Bass).
class EventSink {
public:
	virtual ~EventSink() = default;
	/// @param tick   The swung tick the event belongs to (equal to the `now` passed to tick()).
	/// @param lane   Drum lane for Drums; 0 for Bass.
	/// @param note   Absolute MIDI note for Bass (already resolved from scale steps); 0 for Drums.
	/// @param velocity 1–127.
	virtual void noteOn(int32_t tick, PartId part, uint8_t lane, int16_t note, uint8_t velocity) = 0;
	virtual void noteOff(int32_t tick, PartId part, uint8_t lane, int16_t note) = 0;
};

/// The single engine that turns a Recipe into notes. Fixed size, no allocation, no globals.
/// Every driver (Perform's tick hook, Compose, the harness) runs it the same way:
///
///   gen.start(recipe, seed);
///   int32_t now = 0;
///   while (playing) { int32_t wait = gen.tick(now, sink); now += wait; }  // or call tick() on every tick
///   gen.stop(sink);
///
/// The same Recipe and seed, steered the same way at the same ticks, produce the same event stream.
class Generator {
public:
	Generator();

	/// Reset all state and begin a piece at swung tick 0 (the first step's grid line).
	void start(const Recipe& recipe, uint32_t seed);

	/// Steering: replace the Recipe. Takes effect from the next step not yet decided (up to
	/// kLookaheadTicks + kTicksPerStep ticks later). Safe to call between tick() calls.
	void setRecipe(const Recipe& recipe);
	const Recipe& recipe() const { return recipe_; }

	/// Advance to swung tick `now` (monotonic, non-decreasing; tick 0 is the first step's grid line).
	/// Decides any step whose decision point (grid line - kLookaheadTicks) is <= now, then emits every
	/// buffered event whose tick is <= now, in play order.
	/// May be called on every tick, or only when the previous return value says something is due.
	/// Skipped ticks are caught up: events whose tick is earlier than `now` are emitted late, not dropped.
	/// @return Ticks from `now` until the next tick at which something is due (always >= 1).
	int32_t tick(int32_t now, EventSink& sink);

	/// Send note-offs for every sounding note and clear pending events. Call on transport stop or exit.
	void stop(EventSink& sink);

	/// Index of the last step whose events have started playing (for display); -1 before the first.
	int32_t currentStep() const { return currentStep_; }

	// ---- Internal state. Public so the fixed-size block is visible; drivers must not touch it. ----
	static constexpr int32_t kMaxEvents = 64;
	struct Event {
		int32_t tick;
		PartId part;
		uint8_t lane;
		int16_t note;
		uint8_t velocity; ///< 0 = note-off.
	};

private:
	Recipe recipe_;
	uint32_t seed_ = 0;
	int32_t nextStepToDecide_ = 0;
	int32_t currentStep_ = -1;
	Event events_[kMaxEvents];
	int32_t numEvents_ = 0;
	// Further fixed-size state (loop patterns, sounding-note table, mutation counters) is added by the core
	// implementation. Keep it plain data: no pointers to heap, no std containers.
};

} // namespace deluge::generative
