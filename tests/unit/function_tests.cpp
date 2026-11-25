#include "CppUTest/TestHarness.h"
#include "util/const_functions.h"
#include "util/functions.h"

TEST_GROUP(FunctionTests){};

TEST(FunctionTests, mod) {
	CHECK_EQUAL(0, mod(-3, 3));
	CHECK_EQUAL(1, mod(-2, 3));
	CHECK_EQUAL(2, mod(-1, 3));
	CHECK_EQUAL(0, mod(0, 3));
	CHECK_EQUAL(1, mod(1, 3));
	CHECK_EQUAL(2, mod(2, 3));
	CHECK_EQUAL(0, mod(3, 3));
}

// Both read samples from the card. On a 64-bit host, ARM's `rev` asm doesn't apply: they must still swap.
TEST(FunctionTests, swapEndianness32) {
	CHECK_EQUAL(0x44332211u, swapEndianness32(0x11223344u));
	CHECK_EQUAL(0x000000FFu, swapEndianness32(0xFF000000u));
	CHECK_EQUAL(0x80000001u, swapEndianness32(0x01000080u));
}

TEST(FunctionTests, swapEndianness2x16) {
	CHECK_EQUAL(0x22114433u, swapEndianness2x16(0x11223344u));
	CHECK_EQUAL(0x00FFFF00u, swapEndianness2x16(0xFF0000FFu));
	CHECK_EQUAL(0x80000001u, swapEndianness2x16(0x00800100u));
}
