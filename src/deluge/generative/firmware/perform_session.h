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

// The Perform driver's firmware adapter (spec §2.5): it owns the Generator blocks, the steering queue and the
// portable driver for as long as Perform is entered, runs them from the Session tick (F1, spec §12.1), and plays
// their events into the roster's Outputs with sendNote and noteOnPreKitArp.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/generator.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include "generative/firmware/perform_song.h"
#include "generative/perform/perform_driver.h"
#include <cstdint>

class Song;

namespace deluge::generative::firmware {

/// Plays events into the roster's Outputs, building a ModelStack per call.
class OutputSink final : public perform::NoteSink {
public:
	void play(const Event& event) override;
	/// The safety net after a flush: every voice on every part's Output released. Tails ring out (spec §12.5).
	void allNotesOff();

	Roster roster;
	const Genre* genre = nullptr;
	/// The last Section event played, for the OLED.
	SectionKind section = SectionKind::Intro;
	uint16_t occurrence = 0;
	bool sectionKnown = false;

private:
	void note(const Event& event, bool on);
};

class PerformSession {
public:
	/// Builds Perform's song from the Current Recipe privately while the player's song plays, then swaps it in
	/// stopped and allocates the Generator blocks (spec §14.3). Card reads; task context. On failure the player's song
	/// is untouched. The caller then opens the Perform view.
	Error enter();
	/// Stops the transport and frees the Generator blocks. Perform's song stays current; the caller replaces it.
	void leave();
	[[nodiscard]] bool active() const { return generator_ != nullptr; }

	/// F1: called from Session::doTickForward with the swung tick count since playback started.
	/// @return Song ticks until it next needs a call.
	int32_t tick(int32_t swungTick);
	/// The transport stopped: flush and release every note (spec §12.5).
	void playbackEnded();

	/// Steers a macro: sets the Current Recipe's centre at once, and the core's from the next undecided step.
	void setMacro(Macro macro, int32_t units);

	// For the Perform view.
	[[nodiscard]] bool playing() const { return driver_.running(); }
	/// Generator ticks since step 0's grid line, while playing.
	[[nodiscard]] int32_t clock() const { return driver_.clock(); }
	[[nodiscard]] const OutputSink& output() const { return sink_; }
	/// Where the Arc has a macro now: the centre plus the Arc's offset.
	[[nodiscard]] int32_t effectiveMacro(Macro macro) const;

private:
	void startPiece();
	void stopPiece();

	/// Perform allocates two blocks on entry, because a transition overlaps two instances (spec §2.1, §12.9).
	Generator* blocks_[2] = {};
	Generator* generator_ = nullptr;
	SteeringQueue queue_;
	perform::PerformDriver driver_;
	OutputSink sink_;
	int32_t lastSwungTick_ = 0;
};

extern PerformSession performSession;

} // namespace deluge::generative::firmware
