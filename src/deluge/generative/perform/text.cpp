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

#include "generative/perform/text.h"
#include <cstdint>

namespace deluge::generative::perform {

Text& Text::add(const char* text) {
	return add(text, INT32_MAX);
}

Text& Text::add(const char* text, int32_t count) {
	while (count-- > 0 && *text != '\0' && length_ + 1 < out_.size()) {
		out_[length_++] = *text++;
	}
	out_[length_] = '\0';
	return *this;
}

Text& Text::add(int32_t number) {
	char digits[12];
	int32_t n = sizeof(digits);
	digits[--n] = '\0';
	uint32_t value = number < 0 ? 0U - static_cast<uint32_t>(number) : static_cast<uint32_t>(number);
	do {
		digits[--n] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	if (number < 0) {
		digits[--n] = '-';
	}
	return add(digits + n);
}

} // namespace deluge::generative::perform
