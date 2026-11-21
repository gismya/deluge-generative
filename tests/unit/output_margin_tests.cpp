#include "CppUTest/TestHarness.h"

#include "io/midi/output_margin.h"
#include <cstdint>

using deluge::io::midi::OutputMargin;
using deluge::io::midi::txSamplesQueued;

TEST_GROUP(OutputMargin){};

// The Deluge's TX ring: 128 frames of 8 bytes (two 32-bit channels).
constexpr uint32_t kFrame = 8;
constexpr uint32_t kRing = 128;
constexpr uint32_t kBase = 0x2000'0000;

TEST(OutputMargin, QueuedIsTheWriteLeadOverTheDma) {
	// The DMA reads frame 10, the routine has written up to frame 40: 30 frames still to play.
	CHECK_EQUAL(30, txSamplesQueued(kBase + 10 * kFrame, kBase + 40 * kFrame, kFrame, kRing));
}

TEST(OutputMargin, QueuedWrapsRoundTheRing) {
	// The write position has wrapped past the end of the ring and the DMA hasn't yet.
	CHECK_EQUAL(20, txSamplesQueued(kBase + 120 * kFrame, kBase + 12 * kFrame, kFrame, kRing));
}

TEST(OutputMargin, WriteMeetingTheDmaIsAFullRing) {
	// doSomeOutputting() stops when the write position reaches the DMA's: the ring is full.
	CHECK_EQUAL(128, txSamplesQueued(kBase + 64 * kFrame, kBase + 64 * kFrame, kFrame, kRing));
}

// 755.78 system ticks a sample (33.33 MHz / 44.1 kHz) in Q8.
constexpr uint32_t kTicksPerSampleQ8 = (uint64_t{33'330'000} << 8) / 44'100;

TEST(OutputMargin, NoMarginBeforeTheFirstExit) {
	OutputMargin margin{kTicksPerSampleQ8};
	CHECK_FALSE(margin.primed());
}

TEST(OutputMargin, MarginIsWhatWasQueuedLessTheTimeAway) {
	OutputMargin margin{kTicksPerSampleQ8};
	margin.left(1'000, 128); // a full ring: 128 samples, 96,740 ticks (2.90 ms)
	CHECK_TRUE(margin.primed());
	// Back 1 ms later: 1.90 ms of audio was still queued.
	CHECK_EQUAL(96'740 - 33'330, margin.entered(1'000 + 33'330));
}

TEST(OutputMargin, ComingBackAfterTheQueueRanOutIsANegativeMargin) {
	OutputMargin margin{kTicksPerSampleQ8};
	margin.left(1'000, 64); // 48,370 ticks queued
	CHECK_EQUAL(48'370 - 66'660, margin.entered(1'000 + 66'660));
}

TEST(OutputMargin, MarginSurvivesTheTimerWrapping) {
	OutputMargin margin{kTicksPerSampleQ8};
	margin.left(UINT32_MAX - 1'000, 128);
	CHECK_EQUAL(96'740 - 34'331, margin.entered(33'330));
}
