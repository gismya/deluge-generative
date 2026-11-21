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

#include "generative/firmware/card_text.h"
#include "fatfs/fatfs.hpp"
#include "generative/firmware/card_sounds.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <cstddef>

namespace deluge::generative::firmware {

CardText readCardText(const char* path, int32_t maxBytes, CardTextReader& reader, Error& error) {
	error = StorageManager::initSD();
	if (error != Error::NONE) {
		return CardText::Unreadable;
	}
	auto opened = FatFS::File::open(path, FA_READ);
	if (!opened) {
		// No file, or one FatFs can't open (a bad name, DENIED): missing. Only the card failing is an error.
		const FatFS::Error openError = opened.error();
		if (cardFailed(openError)) {
			error = fatfsErrorToDelugeError(openError);
			return CardText::Unreadable;
		}
		return CardText::Missing;
	}
	// One byte more than the largest file read, to tell a file that fills the buffer from one that overflows it.
	const int32_t bufferBytes = maxBytes + 1;
	auto* buffer = static_cast<std::byte*>(GeneralMemoryAllocator::get().allocLowSpeed(bufferBytes));
	if (buffer == nullptr) {
		error = Error::INSUFFICIENT_RAM;
		return CardText::Unreadable;
	}
	size_t length = 0;
	bool unread = false;
	while (length < static_cast<size_t>(bufferBytes)) {
		auto read = opened->read({buffer + length, static_cast<size_t>(bufferBytes) - length});
		if (!read) {
			// As for the open: only the card failing is an error; a file FatFs can't read through is unreadable.
			const FatFS::Error readError = read.error();
			if (cardFailed(readError)) {
				error = fatfsErrorToDelugeError(readError);
			}
			unread = true;
			break;
		}
		if (read->empty()) {
			break;
		}
		length += read->size();
		AudioEngine::routineWithClusterLoading();
	}
	const bool whole = !unread && length <= static_cast<size_t>(maxBytes);
	if (whole) {
		reader.read({reinterpret_cast<const char*>(buffer), length});
	}
	delugeDealloc(buffer);
	return whole ? CardText::Read : CardText::Unreadable;
}

} // namespace deluge::generative::firmware
