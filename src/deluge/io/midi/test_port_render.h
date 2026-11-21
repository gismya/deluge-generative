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
	// The audio output's margin (spec §20, #449): at each entry to the audio routine, the audio the TX ring still held
	// (io/midi/output_margin.h). Render starts alone don't show it, as one routine call may copy audio rendered
	// earlier.
	uint32_t routineCalls = 0;
	uint32_t underruns = 0;              ///< Entries after the ring ran dry: the codec replayed old samples.
	int32_t marginMinCounts = INT32_MAX; ///< The least audio left at an entry; negative once it ran dry.
	uint32_t margins[4] = {};            ///< Entries with under 0.5, 1 and 2 ms left, and the rest (not underruns).
	int32_t dmaDriftMax = 0;             ///< The DMA's largest move off the timer at an entry, in samples (dmaDrift()).
	/// The task that finished last before each underrun's entry: the one that kept the routine away.
	struct LateTask {
		const char* name = nullptr;
		uint32_t underruns = 0;
		uint32_t runMaxCounts = 0; ///< Its longest run before an underrun.
	};
	LateTask lateTasks[4] = {};
	uint32_t renderGapMaxCounts = 0; ///< The longest span from one render's start to the next's.
	uint32_t renderGaps[6] = {};     ///< Spans between render starts under 1, 2, 2.9, 5, 10 ms, and the rest.
};
extern RenderTiming renderTiming;

/// The audio engine's render of `numSamples`, which started at `start` (getSystemTicks()), has ended.
void recordRender(uint32_t start, size_t numSamples);
/// The audio routine starts, at entry to AudioEngine::routine() (spec §20, the audio output's margin).
/// `dmaFrame`: the frame of the TX ring the DMA reads.
void audioRoutineEntered(uint32_t dmaFrame);
/// The audio routine returns, with `queuedSamples` (txSamplesQueued()) in the TX ring and the DMA at `dmaFrame`.
void audioRoutineLeft(uint32_t queuedSamples, uint32_t dmaFrame);
/// The scheduler's task `name` ran for `counts` (getSystemTicks()), for the underruns' culprit.
void taskRan(const char* name, uint32_t counts);
/// The audio engine culled one of `sound`'s voices for CPU.
void voiceCulled(const Sound& sound);

} // namespace TestPort

#endif
