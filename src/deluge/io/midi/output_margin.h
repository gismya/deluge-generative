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

#include <cstdint>

namespace deluge::io::midi {

/// Frames still to play in the codec's TX ring: how far the audio routine's write position `writeAddr` leads the
/// DMA's read position `dmaAddr`, in frames of `bytesPerFrame`, round a ring of `ringFrames` (a power of two). The
/// routine stops writing when it reaches the DMA's position, so equal positions are a full ring. A DMA that has lapped
/// the write position can't be told apart from a lead: only OutputMargin's timing sees that.
constexpr uint32_t txSamplesQueued(uint32_t dmaAddr, uint32_t writeAddr, uint32_t bytesPerFrame, uint32_t ringFrames) {
	const uint32_t free = ((dmaAddr - writeAddr) / bytesPerFrame) & (ringFrames - 1);
	return free == 0 ? ringFrames : ringFrames - free;
}

/// Generative mode (spec §20, the audio output's margin): how close the audio output came to running dry. When
/// AudioEngine::routine() returns, the TX ring holds some audio the DMA has yet to play; nothing tops it up until the
/// routine runs again. So the margin at the next entry is that audio's length less the time away, and a negative margin
/// means the DMA ran out of new audio and replayed old samples: an underrun. The audio rendered ahead into the
/// rendering buffer doesn't count, as only the routine copies it into the ring.
class OutputMargin {
public:
	/// `ticksPerSampleQ8`: system timer ticks a sample, in Q8.
	constexpr explicit OutputMargin(uint32_t ticksPerSampleQ8) : ticksPerSampleQ8_(ticksPerSampleQ8) {}

	/// Whether the routine has returned once, so entered() has a margin to give.
	[[nodiscard]] constexpr bool primed() const { return primed_; }

	/// The routine starts at `now` (system ticks): the margin left, in system ticks. Only once primed().
	[[nodiscard]] constexpr int32_t entered(uint32_t now) const {
		return static_cast<int32_t>(queuedTicks_ - (now - leftAt_));
	}

	/// How far the DMA, now at frame `dmaFrame` of a ring of `ringFrames`, has moved beyond what the system timer says
	/// since the routine returned, in frames (negative: behind). Near 0 when the timer and the codec's clock agree, so
	/// a margin entered() gives is real time the routine was away. Only once primed().
	[[nodiscard]] constexpr int32_t dmaDrift(uint32_t now, uint32_t dmaFrame, uint32_t ringFrames) const {
		const uint32_t expected =
		    static_cast<uint32_t>(((uint64_t{now - leftAt_} << 8) + ticksPerSampleQ8_ / 2) / ticksPerSampleQ8_);
		const uint32_t diff = (dmaFrame - leftDmaFrame_ - expected) & (ringFrames - 1);
		return diff < ringFrames / 2 ? static_cast<int32_t>(diff)
		                             : static_cast<int32_t>(diff) - static_cast<int32_t>(ringFrames);
	}

	/// The routine returns at `now`, with `queuedSamples` (txSamplesQueued()) still to play and the DMA at frame
	/// `dmaFrame`.
	constexpr void left(uint32_t now, uint32_t queuedSamples, uint32_t dmaFrame = 0) {
		leftAt_ = now;
		queuedTicks_ = (queuedSamples * ticksPerSampleQ8_) >> 8;
		leftDmaFrame_ = dmaFrame;
		primed_ = true;
	}

private:
	uint32_t ticksPerSampleQ8_;
	uint32_t leftAt_ = 0;
	uint32_t queuedTicks_ = 0;
	uint32_t leftDmaFrame_ = 0;
	bool primed_ = false;
};

} // namespace deluge::io::midi
