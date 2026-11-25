/*
 * Copyright © 2020-2023 Synthstrom Audible Limited
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

#include "util/container/array/ordered_resizeable_array.h"
#include <cstdint>

// This class extends OrderedResizeableArrayWith32bitKey so that if we wish, we can still use that class's single-word
// search functions, where we only care about the first word.

class OrderedResizeableArrayWithMultiWordKey : public OrderedResizeableArrayWith32bitKey {
public:
	OrderedResizeableArrayWithMultiWordKey(int32_t newElementSize = sizeof(uint32_t) * 2, int32_t newNumWordsInKey = 2);
	OrderedResizeableArrayWithMultiWordKey(int32_t newElementSize, int32_t newNumWordsInKey,
	                                       int32_t newMaxNumEmptySpacesToKeep, int32_t newNumExtraSpacesToAllocate);
	int32_t searchMultiWord(uint32_t* __restrict__ keyWords, int32_t comparison, int32_t rangeBegin = 0,
	                        int32_t rangeEnd = -1);
	int32_t searchMultiWordExact(uint32_t* __restrict__ keyWords, int32_t* getIndexToInsertAt = nullptr,
	                             int32_t rangeBegin = 0);
	int32_t insertAtKeyMultiWord(uint32_t* __restrict__ keyWords, int32_t rangeBegin = 0, int32_t rangeEnd = -1);
	bool deleteAtKeyMultiWord(uint32_t* __restrict__ keyWords);
	void testSequentiality(char const* errorCode);

#if UINTPTR_MAX > UINT32_MAX
	/// Where a pointer is wider than a key word, these hide OrderedResizeableArray::search(): they search the first key
	/// word alone, compared unsigned as searchMultiWord() compares it here. On the Deluge, search() is the base
	/// class's.
	int32_t search(int32_t firstWord, int32_t comparison, int32_t rangeBegin, int32_t rangeEnd) {
		return searchFirstWordUnsigned((uint32_t)firstWord, false, rangeBegin, rangeEnd) + comparison;
	}
	int32_t search(int32_t firstWord, int32_t comparison, int32_t rangeBegin = 0) {
		return search(firstWord, comparison, rangeBegin, numElements);
	}
#endif

	/// The first index from `rangeBegin` whose first key word is past `pointerWord`, a pointer's low 32 bits: the end
	/// of that pointer's run of elements.
	[[gnu::always_inline]] int32_t searchPastPointerWord(uint32_t pointerWord, int32_t rangeBegin) {
		if constexpr (kUnsignedKeyWords) {
			return searchFirstWordUnsigned(pointerWord, true, rangeBegin, numElements);
		}
		else {
			// The next address a 4-byte-aligned object could have.
			return search((int32_t)(pointerWord + 4), GREATER_OR_EQUAL, rangeBegin);
		}
	}

	int32_t numWordsInKey;

	/// On the Deluge, key words compare as the signed difference of the two words, which orders addresses only while
	/// they are below 0x80000000 and within 2 GiB of each other, as all of the Deluge's RAM is. Where a pointer is
	/// wider than a key word, its low word can be anything, so key words compare unsigned.
	static constexpr bool kUnsignedKeyWords = sizeof(void*) > sizeof(uint32_t);

private:
	/// The first index in [rangeBegin, rangeEnd) whose first key word is at least `firstWord` (`past`: more than).
	int32_t searchFirstWordUnsigned(uint32_t firstWord, bool past, int32_t rangeBegin, int32_t rangeEnd) {
		while (rangeBegin != rangeEnd) {
			int32_t proposedIndex = (rangeBegin + rangeEnd) >> 1;
			uint32_t wordHere = *(uint32_t*)getElementAddress(proposedIndex);
			if (wordHere < firstWord || (past && wordHere == firstWord)) {
				rangeBegin = proposedIndex + 1;
			}
			else {
				rangeEnd = proposedIndex;
			}
		}
		return rangeBegin;
	}
};
