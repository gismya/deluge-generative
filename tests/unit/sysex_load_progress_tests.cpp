#include "CppUTest/TestHarness.h"

#include "io/midi/sysex_load_progress.h"
#include <cstdint>

using deluge::io::midi::loadProgressPad;

TEST_GROUP(SysexLoadProgress){};

// A buffer of 1000 packets, the size of a ~500 KB firmware.
constexpr uint32_t kBufSize = 512 * 1000;

TEST(SysexLoadProgress, FirstPacketLightsFirstPad) {
	CHECK_EQUAL(0, loadProgressPad(0, kBufSize));
}

TEST(SysexLoadProgress, HalfwayLightsMiddleOfGrid) {
	// 144 pads, half of the buffer: pad 72 = row 4, column 0.
	CHECK_EQUAL(72, loadProgressPad(512 * 500, kBufSize));
}

TEST(SysexLoadProgress, LastPacketStaysOnLastPad) {
	// The last packet starts 512 bytes before the end of the buffer.
	CHECK_EQUAL(143, loadProgressPad(kBufSize - 512, kBufSize));
}

TEST(SysexLoadProgress, LastPacketOfRealFirmwareStaysOnGrid) {
	// The 3560-packet upload from #55: the old formula put its last packets on row 8.
	constexpr uint32_t kRealBufSize = 512 * 3560;
	CHECK_EQUAL(143, loadProgressPad(512 * 3559, kRealBufSize));
}

TEST(SysexLoadProgress, BufferSmallerThan64KStaysOnGrid) {
	// The old formula subtracted 0xffff from the size, which wraps for small buffers.
	constexpr uint32_t kSmallBufSize = 512 * 4;
	CHECK_EQUAL(0, loadProgressPad(0, kSmallBufSize));
	CHECK_EQUAL(108, loadProgressPad(512 * 3, kSmallBufSize));
}

TEST(SysexLoadProgress, PositionPastBufferIsClamped) {
	CHECK_EQUAL(143, loadProgressPad(kBufSize * 2, kBufSize));
}
