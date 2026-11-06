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

#include "generative/firmware/current_recipe.h"
#include "generative/core/draw.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/cycle_count.h"
#include "storage/storage_manager.h"

namespace deluge::generative::firmware {

CurrentRecipe currentRecipe{};

uint32_t freshEntropy() {
	// The timer's low bits change every few cycles, so two calls a few instructions apart still differ.
	static uint32_t calls = 0;
	return deluge::generative::detail::mix(cycleCount() ^ (++calls * 0x85ebca6bU));
}

Error CurrentRecipe::rollFromGenre(const Genre& genre) {
	Recipe rolled = newRecipe(genre, freshEntropy());
	CardFolders folders;
	if (!rollSounds(rolled, freshEntropy(), folders)) {
		Error error = StorageManager::initSD();
		return error != Error::NONE ? error : Error::SD_CARD;
	}
	recipe = rolled;
	seed = freshEntropy();
	takes = Takes{};
	exists = true;
	return Error::NONE;
}

} // namespace deluge::generative::firmware
