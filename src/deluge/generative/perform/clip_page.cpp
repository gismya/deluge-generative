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

#include "generative/perform/clip_page.h"
#include "generative/perform/text.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

/// The highest take made, the one waiting included.
int32_t topTake(const TakeRow& row) {
	return std::max({row.newest, row.playing, row.waiting});
}

} // namespace

TakeLook takeLook(const TakeRow& row, int32_t take) {
	if (take == row.waiting && row.waiting != row.playing) {
		return TakeLook::Queued;
	}
	if (take == row.playing) {
		return TakeLook::Playing;
	}
	const int32_t top = topTake(row);
	return take <= top ? TakeLook::Earlier : take == top + 1 ? TakeLook::New : TakeLook::Off;
}

bool takeTappable(const TakeRow& row, int32_t take) {
	return take >= 0 && take < kMaxTakes && takeLook(row, take) != TakeLook::Off;
}

int32_t steppedTake(const TakeRow& row, int32_t direction) {
	const int32_t from = row.waiting >= 0 ? row.waiting : row.playing;
	const int32_t to = from + (direction < 0 ? -1 : 1);
	// Forward never skips past the next new take, which is one past the newest.
	return to >= 0 && to < kMaxTakes ? to : -1;
}

void takeLabel(const TakeRow& row, bool frozen, char (&out)[kTakeLabelChars + 1]) {
	Text text(out);
	text.add("T").add(row.playing + 1).add("/").add(topTake(row) + 1).add(frozen ? "*" : "");
}

void seedCode(uint32_t seed, char (&out)[kSeedCodeChars + 1]) {
	constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	for (int32_t i = kSeedCodeChars - 1; i >= 0; i--) {
		out[i] = kAlphabet[seed & 31U];
		seed >>= 5;
	}
	out[kSeedCodeChars] = '\0';
}

} // namespace deluge::generative::perform
