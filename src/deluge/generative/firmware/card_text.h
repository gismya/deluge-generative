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

// Reading a small text file from the card whole, for the core's parsers (palette override files, Recipe files).

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>
#include <string_view>

namespace deluge::generative::firmware {

/// What readCardText() found.
enum class CardText : uint8_t {
	Read,       ///< The whole file, handed to the reader
	Missing,    ///< No file, or one FatFs can't open (a bad name, DENIED)
	Unreadable, ///< Larger than allowed, FatFs couldn't read through it, or the card failed (then `error` says so)
};

/// Takes the text readCardText() read. It lives only for the call.
class CardTextReader {
public:
	virtual void read(std::string_view text) = 0;

protected:
	~CardTextReader() = default;
};

/// Reads the file at `path` whole, if it holds at most `maxBytes`, into a buffer from the low-speed allocator, and
/// hands it to `reader`. `error` is set only when the card fails or RAM runs out, and Error::NONE otherwise. Audio
/// gets a turn between reads.
CardText readCardText(const char* path, int32_t maxBytes, CardTextReader& reader, Error& error);

} // namespace deluge::generative::firmware
