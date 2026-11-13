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

// Text built without printf in the portable code (perform/): libc's would pull in its stdio in the firmware.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// Builds a string in a fixed buffer, cutting it short when full. No printf: this code builds in the firmware, where
/// libc's would pull in its stdio, and in the harness. Its adds are out of line (text.cpp): inlined at every use
/// they cost the firmware image about 2 KB.
class Text {
public:
	explicit Text(std::span<char> out) : out_(out) { out_[0] = '\0'; }
	Text& add(const char* text);
	/// At most `count` characters of `text`.
	Text& add(const char* text, int32_t count);
	Text& add(int32_t number);

private:
	std::span<char> out_;
	size_t length_ = 0;
};

} // namespace deluge::generative::perform
