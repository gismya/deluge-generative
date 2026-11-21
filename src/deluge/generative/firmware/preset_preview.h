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

// Perform's preset preview (spec §13.3, §13.5, #443): the firmware's adapter for perform::PresetPreviewLifecycle.
// While stopped, the preset under a browser's cursor loads as the part would load it (buildPartOutput(), its load-time
// settings, a Kit's other lanes as they are) in a live runner job, linked into Perform's song beside the part's own
// Output, and plays one audition note. The part's Output, its picks, its previous sound, its sound controls and the
// steering log never see it: a move frees it before the next loads, and a start or a close frees it at once, so BACK
// leaves the part exactly as it was. A choice still preloads as a re-roll does.

#pragma once

#include "generative/core/event.h"
#include "generative/core/recipe.h"
#include "generative/firmware/perform_song.h"
#include "generative/perform/preset_preview.h"
#include <cstdint>

namespace deluge::generative::firmware {

class PresetPreview {
public:
	PresetPreview() : lifecycle_(*this) {}

	/// The cursor is on `choice`, a preset for `part` (a synth part's, or an FX lane's): it previews once the runner
	/// is free, in place of the one that shows.
	void show(int32_t part, const SoundChoice& choice);
	/// Nothing previews: the note goes and the Output is freed. A build still running is cancelled, and frees what it
	/// built as it ends.
	void stop();
	/// Each frame while a browser is open, in task context.
	void service();
	/// A build runs: the runner is busy until it unwinds.
	[[nodiscard]] bool building() const { return lifecycle_.building(); }

	// The adapter (perform::PresetPreviewAdapter). Public so the concept sees them; only the lifecycle calls them.
	[[nodiscard]] bool runnerBusy() const;
	bool submitBuild(uint8_t token);
	void cancelBuild();
	void freeShown();
	void noteOn();
	void noteOff();

private:
	friend class PresetPreviewJob;
	/// The job ended, in its task: `output` built for `token` (empty if it built none).
	void buildEnded(uint8_t token, PartOutput& output);
	/// Frees an Output the preview built, and the samples only it used (spec §20), unless another job runs: it may be
	/// loading samples that no Output holds yet.
	static void freePreviewOutput(PartOutput& output);
	/// The audition note's event on the Output that shows.
	[[nodiscard]] Event noteEvent() const;

	perform::PresetPreviewLifecycle<PresetPreview> lifecycle_;
	/// The wanted preset and its part, which the next build loads.
	SoundChoice wanted_;
	int32_t wantedPart_ = -1;
	/// The Output that shows, and the part and lane it previews.
	PartOutput shown_;
	int32_t shownPart_ = -1;
	Lane shownLane_ = Lane::None;
};

static_assert(perform::PresetPreviewAdapter<PresetPreview>);

extern PresetPreview presetPreview;

} // namespace deluge::generative::firmware
