#pragma once
#include "definitions_cxx.hpp"
#include <cstring>

extern "C" {
#include "util/cfunctions.h"
}

char halfByteToHexChar(uint8_t thisHalfByte);
void intToHex(uint32_t number, char* output, int32_t numChars = 8);
uint32_t hexToInt(char const* string);
uint32_t hexToIntFixedLength(char const* __restrict__ hexChars, int32_t length);

/// A string buffer with utility functions to append and format contents.
/// does not handle allocation
class StringBuf {
	// Not templated to optimize binary size.
public:
	StringBuf(char* buf, size_t capacity) : capacity_(capacity), buf_(buf) { memset(buf_, '\0', capacity_); }
	void append(std::string_view str);
	void append(char c) { ::strncat(buf_, &c, 1); }
	void removeSpaces() {
		size_t removed = 0;
		// upto size, not below it -- we want the null as well
		for (size_t i = 0; i <= size(); i++) {
			if (isspace(buf_[i])) {
				removed++;
			}
			else {
				buf_[i - removed] = buf_[i];
			}
		}
	}
	void clear() { buf_[0] = 0; }
	void truncate(size_t newSize) {
		if (newSize < capacity_) {
			buf_[newSize] = '\0';
		}
	}

	// TODO: Validate buffer size. These can overflow
	void appendInt(int i, int minChars = 1) { intToString(i, buf_ + size(), minChars); }
	void appendHex(int i, int minChars = 1) { intToHex(i, buf_ + size(), minChars); }
	void appendFloat(float f, int32_t minDecimals, int32_t maxDecimals) {
		floatToString(f, buf_ + size(), minDecimals, maxDecimals);
	}

	[[nodiscard]] char* data() { return buf_; }
	[[nodiscard]] const char* data() const { return buf_; }
	[[nodiscard]] const char* c_str() const { return buf_; }

	[[nodiscard]] size_t capacity() const { return capacity_; }
	[[nodiscard]] size_t size() const { return std::strlen(buf_); }
	[[nodiscard]] size_t length() const { return std::strlen(buf_); }

	[[nodiscard]] bool empty() const { return buf_[0] == '\0'; }

	bool operator==(const char* rhs) const { return std::strcmp(buf_, rhs) == 0; }
	bool operator==(const StringBuf& rhs) const { return std::strcmp(buf_, rhs.c_str()) == 0; }

	operator std::string_view() const { return std::string_view{buf_}; }

private:
	size_t capacity_;
	char* buf_;
};

/// Define a `StringBuf` that uses an array placed on the stack.
#if defined(__clang__)
// clang rejects an initializer on a variable-length array, and two callers size theirs at run time. GCC keeps the
// initializer so the firmware's code doesn't change.
#define DEF_STACK_STRING_BUF(name, capacity)                                                                           \
	char name##__buf[capacity];                                                                                        \
	memset(name##__buf, 0, sizeof(name##__buf));                                                                       \
	StringBuf name = {name##__buf, capacity}
#else
#define DEF_STACK_STRING_BUF(name, capacity)                                                                           \
	char name##__buf[capacity] = {0};                                                                                  \
	StringBuf name = {name##__buf, capacity}
#endif
