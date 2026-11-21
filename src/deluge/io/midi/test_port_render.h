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

#pragma once

#if ENABLE_SYSEX_TEST

#include <cstddef>
#include <cstdint>

class Sound;

namespace TestPort {

/// Generative mode (F2, spec §19, §20 H2 and H16): in test-port builds, the time the audio engine spent in
/// renderAudio(), in getSystemTicks() counts, and the voices it culled for CPU, since the reader last reset it. Perform
/// reports it to time live ramps and to name the culled parts.
struct RenderTiming {
	uint32_t counts = 0;
	uint32_t samples = 0;
	uint32_t maxCounts = 0;            ///< The slowest render.
	uint32_t culls = 0;                ///< Voices culled for CPU: killed, terminated or force-released (H16).
	const Sound* lastCulled = nullptr; ///< The Sound of the last voice culled, for its part's name; null once freed.
	// #448 (the card log on vs off, spec §17.2): the renders' distribution, the longest wait between two
	// renders, the most samples the output buffer had drained when a render began, and sample-streaming misses.
	uint32_t hist[7] = {};       ///< Renders under 250, 500, 750, 1000, 1250, 1500 µs, and the rest
	uint32_t gapMaxCounts = 0;   ///< The longest span from one render's start to the next's
	uint32_t gaps[6] = {};       ///< Spans between renders under 1, 2, 2.9 (the TX buffer), 5, 10 ms, and the rest
	uint32_t drainedMax = 0;     ///< The most samples waiting in the routine (of the 128-sample TX buffer)
	uint32_t lateClusters = 0;   ///< A playing voice's next Cluster wasn't loaded in time (the sound cuts)
	uint32_t unloadedStarts = 0; ///< A voice's first Cluster wasn't loaded when it started or jumped
};
extern RenderTiming renderTiming;

/// The audio engine's render of `numSamples`, which started at `start` (getSystemTicks()), has ended.
void recordRender(uint32_t start, size_t numSamples);
/// #448: the routine found `drained` samples waiting before it doubled them.
void recordDrained(size_t drained);
/// #448: a Cluster a voice needed wasn't loaded: `playing` (mid-sound, it cuts) or at the voice's start.
void clusterMissed(bool playing);
/// The audio engine culled one of `sound`'s voices for CPU.
void voiceCulled(const Sound& sound);

} // namespace TestPort

#endif
