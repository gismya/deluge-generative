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

// Perform's preset preview (spec §13.3, §13.5): while stopped, the preset under a browser's cursor loads into an Output
// of the part, as a preload does, and plays one audition note. It is never the part's sound: the roster, the part's
// picks, its previous sound and its sound controls never see it, and a move, a start or a close frees it. This decides
// when to build, play, let go and free; the firmware's adapter does each.

#pragma once

#include <concepts>
#include <cstdint>

namespace deluge::generative::perform {

/// How long the audition note holds before its release, in the adapter clock's samples: a second at 44.1 kHz.
constexpr uint32_t kPresetPreviewNoteSamples = 44100;

/// What the lifecycle needs of the firmware. The adapter keeps which preset is wanted (it is told before each want())
/// and the Output that shows.
template <class Adapter>
concept PresetPreviewAdapter = requires(Adapter& adapter, const Adapter& reader, uint8_t token) {
	/// A job runs or waits: a build would only queue behind it.
	{ reader.runnerBusy() } -> std::same_as<bool>;
	/// Builds the wanted preset's Output as a live job, which ends in built() or buildEnded() with `token`. False if
	/// the runner took nothing.
	{ adapter.submitBuild(token) } -> std::same_as<bool>;
	/// Cancels the running build.
	{ adapter.cancelBuild() } -> std::same_as<void>;
	/// Frees the Output that shows, its note already let go.
	{ adapter.freeShown() } -> std::same_as<void>;
	/// The audition note on the Output that shows, and its release.
	{ adapter.noteOn() } -> std::same_as<void>;
	{ adapter.noteOff() } -> std::same_as<void>;
};

template <class Adapter>
class PresetPreviewLifecycle {
public:
	explicit PresetPreviewLifecycle(Adapter& adapter) : adapter_(adapter) {}

	/// The cursor is on a preset to preview, which the adapter now holds. It builds once the runner is free, in place
	/// of the one that shows; a build for an earlier preset is cancelled.
	void want() {
		wanted_ = true;
		token_++;
	}

	/// Nothing previews now (a folder under the cursor, a start, the browser closing): the note goes, what shows is
	/// freed, and a running build is cancelled, its Output freed as it ends.
	void stop() {
		wanted_ = false;
		if (building_ && !cancelSent_) {
			adapter_.cancelBuild();
			cancelSent_ = true;
		}
		hide();
	}

	/// Task context, each frame while a browser previews: lets the note go after its length, and starts the wanted
	/// build once the runner is free. `now` is in samples.
	void service(uint32_t now) {
		if (noteOn_ && now - noteAt_ >= kPresetPreviewNoteSamples) {
			adapter_.noteOff();
			noteOn_ = false;
		}
		if (!wanted_ || tried_ == token_) {
			return;
		}
		if (building_) {
			if (!cancelSent_) {
				adapter_.cancelBuild(); // the cursor moved on: the newest builds once this one has unwound
				cancelSent_ = true;
			}
			return;
		}
		if (adapter_.runnerBusy()) {
			return;
		}
		hide();
		if (adapter_.submitBuild(token_)) {
			building_ = true;
			cancelSent_ = false;
			buildToken_ = token_;
		}
	}

	/// The build of `token` has its Output, in the job's task, at sample `now`. @return whether it is kept: it shows
	/// and its note plays. Else the adapter frees it at once.
	bool built(uint8_t token, uint32_t now) {
		building_ = false;
		if (!wanted_ || token != token_) {
			return false;
		}
		tried_ = token;
		shown_ = true;
		adapter_.noteOn();
		noteOn_ = true;
		noteAt_ = now;
		return true;
	}

	/// The build ended with no Output: cancelled, or failed (which isn't tried again until the cursor moves).
	void buildEnded() {
		building_ = false;
		if (!cancelSent_ && wanted_ && token_ == buildToken_) {
			tried_ = token_;
		}
	}

	/// A build runs: the runner is busy with it until it ends.
	[[nodiscard]] bool building() const { return building_; }
	/// An Output shows.
	[[nodiscard]] bool shows() const { return shown_; }

private:
	/// The note goes and what shows is freed.
	void hide() {
		if (noteOn_) {
			adapter_.noteOff();
			noteOn_ = false;
		}
		if (shown_) {
			adapter_.freeShown();
			shown_ = false;
		}
		tried_ = static_cast<uint8_t>(token_ - 1); // nothing shows: the wanted preset builds again
	}

	Adapter& adapter_;
	uint32_t noteAt_ = 0;
	/// Counts wants: a build's token says which want it was for.
	uint8_t token_ = 0;
	/// The want whose build has ended (shown, or failed): service() doesn't build it again.
	uint8_t tried_ = 0;
	/// The want the running build is for.
	uint8_t buildToken_ = 0;
	bool wanted_ = false;
	bool building_ = false;
	bool cancelSent_ = false;
	bool shown_ = false;
	bool noteOn_ = false;
};

} // namespace deluge::generative::perform
