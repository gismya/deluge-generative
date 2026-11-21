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

// The portable Compose driver (spec §2.5, §15): it renders the Current Recipe, seed and takes from Start, as an
// untouched Perform would play them (spec §6.8), until LENGTH's target step count, then queues End where it cuts no
// Build or Break, and writes the whole song to a SongWriter through a ClipBuilder. Its firmware adapter runs it as a
// runner job, a chunk of steps at a time (spec §15.7), and builds the Song.

#pragma once

#include "generative/compose/clip_builder.h"
#include "generative/core/generator.h"
#include "generative/core/recipe.h"
#include "generative/core/steering.h"
#include <cstdint>

namespace deluge::generative::compose {

/// LENGTH as a target step count, worked out once before the render: floor(seconds × tempo / 1500), tempo in
/// hundredths of a BPM (spec §15.1). In integer math (ADR 0004).
constexpr int32_t targetSteps(int32_t lengthSeconds, int32_t tempo) {
	return static_cast<int32_t>(int64_t{lengthSeconds} * tempo / 1500);
}

class ComposeDriver {
public:
	/// How many Grooves past the target may end at a bar line into a short Build, playing on, before Compose ends the
	/// song on the next phrase line where it cuts nothing: genre data where every Groove goes into a short Build would
	/// otherwise never end. No built-in genre gets there in practice. (tuning)
	static constexpr int32_t kMaxShortBuildGrooves = 2;

	/// Starts `generator` on the Recipe, seed and takes from Start, with the Intro (spec §15.1), and calls the writer's
	/// begin(). The song's End comes once the step index reaches `targetSteps`. `skipIntro` starts in the Groove, as
	/// SHIFT+PLAY does: only the firmware check (spec §21.3) asks for it, to build a golden's case.
	void begin(Generator& generator, const Recipe& recipe, uint32_t seed, const Takes& takes, int32_t targetSteps,
	           SongWriter& writer, bool skipIntro = false);
	/// Decides up to `steps` more steps, writing what they play. A runner job calls it a chunk at a time and yields
	/// between (spec §15.7). Once the Outro's last line is reached, the core is flushed and the writer finished.
	/// @return whether the song is done.
	bool run(int32_t steps);

	/// The step End was queued before, which is the Outro's first step, or -1 while it isn't queued yet. Everything
	/// before it is what an untouched Perform plays (spec §6.8).
	[[nodiscard]] int32_t endQueuedAt() const { return endQueuedAt_; }
	/// The song's end, the Outro's last line, in ticks; 0 until done.
	[[nodiscard]] int32_t endTick() const { return endTick_; }
	[[nodiscard]] const ClipBuilder& clips() const { return clips_; }

private:
	/// Whether End goes on the queue before the next step's decision: the step index has reached the target, and the
	/// step is a phrase line where a Groove ends on its own, or a Groove that never ends on its own (Motion 0) plays.
	/// Counts each Groove past the target that ends into a short Build, and past kMaxShortBuildGrooves ends the song on
	/// the next phrase line that cuts nothing. A Groove past the cap that is one phrase long has no such line: its
	/// Build plays, and End goes on the queue before the Build's Drop, so the Outro takes the place of the Groove after
	/// it.
	[[nodiscard]] bool endsHere();
	/// Writes every event due at or before `upTo`, in play order, as Perform's playDue() plays them.
	void writeDue(int32_t upTo);
	/// The sample Riser's note-on starts late, so its file ends on the Drop, at the Recipe's tempo (spec §9.5).
	void timeRiser(Event& noteOn);
	void finish();

	Generator* generator_ = nullptr;
	SongWriter* writer_ = nullptr;
	SteeringQueue queue_;
	ClipBuilder clips_;
	int32_t target_ = 0;
	int32_t tempo_ = 0;
	int32_t endQueuedAt_ = -1;
	int32_t endTick_ = 0;
	int32_t shortBuildGrooves_ = 0;
	bool done_ = false;
};

} // namespace deluge::generative::compose
