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

#include "gui/context_menu/generative.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/perform_from.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"

namespace deluge::gui::context_menu {

Generative generativeMenu{};
DiscardSong discardSong{};

// ---- GENERATIVE ----

char const* Generative::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_GENERATIVE);
}

std::span<char const*> Generative::getOptions() {
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_PERFORM)};
	return {options, 1};
}

bool Generative::acceptCurrentOption() {
	// PERFORM opens PERFORM FROM, full screen, in place of this menu.
	close();
	if (soundEditor.setup(nullptr, &menu_item::generative::performFromMenu)) {
		openUI(&soundEditor);
	}
	return true;
}

// ---- DISCARD SONG? ----

char const* DiscardSong::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_DISCARD_SONG_QMARK);
}

std::span<char const*> DiscardSong::getOptions() {
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_OK)};
	return {options, 1};
}

bool DiscardSong::acceptCurrentOption() {
	if (genre_ == nullptr) {
		return false;
	}
	// Back to the session view first, so nothing above it outlives the song it was opened over.
	soundEditor.exitCompletely();

	display->displayLoadingAnimationText(l10n::get(l10n::String::STRING_FOR_LOADING));
	Error error = generative::firmware::currentRecipe.rollFromGenre(*genre_);
	if (error == Error::NONE) {
		error = generative::firmware::performAdapter.enter();
	}
	display->removeLoadingAnimation();
	if (error != Error::NONE) {
		display->displayError(error); // the player's song is untouched
		return true;
	}
	changeRootUI(&performView);
	TEST_EVENT("gen_perform_entered %s", genre_->name);
	return true;
}

} // namespace deluge::gui::context_menu
