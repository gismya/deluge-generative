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
#include "generative/firmware/recipe_files.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/compose_screen.h"
#include "gui/menu_item/generative/perform_from.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"

namespace deluge::gui::context_menu {

Generative generativeMenu{};
DiscardSong discardSong{};
LeavePerform leavePerform{};
RegenerateRecipe regenerateRecipe{};

ActionResult GenerativeContextMenu::padAction(int32_t x, int32_t y, int32_t on) {
	// As ContextMenu's, without waiting for the card routine to end: any pad closes the menu.
	if (on && uiModeAllowsActions()) {
		display->setNextTransitionDirection(-1);
		close();
	}
	return ActionResult::DEALT_WITH;
}

// ---- GENERATIVE ----

char const* Generative::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_GENERATIVE);
}

std::span<char const*> Generative::getOptions() {
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_PERFORM),
	                                l10n::get(l10n::String::STRING_FOR_COMPOSE)};
	return {options, std::size(options)};
}

bool Generative::acceptCurrentOption() {
	// PERFORM opens PERFORM FROM and COMPOSE opens COMPOSE FROM, full screen, in place of this menu.
	const bool perform = currentOption == 0;
	close();
	if (soundEditor.setup(nullptr, perform ? &menu_item::generative::performFromMenu
	                                       : &menu_item::generative::composeFromMenu)) {
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

namespace {

/// Perform entry ended, in the runner's task (spec §14.3).
void entryEnded(generative::JobOutcome outcome, Error error) {
	generativeModalUI.jobEnded();
	if (outcome == generative::JobOutcome::Done) {
		changeRootUI(&performView);
		TEST_EVENT("gen_perform_entered %s", generative::firmware::currentRecipe.recipe.genre->name);
	}
	else if (outcome == generative::JobOutcome::Failed) {
		display->displayError(error); // the player's song is untouched
	}
}

} // namespace

bool DiscardSong::acceptCurrentOption() {
	if (compose_) {
		close();
		menu_item::generative::buildComposeRecipe();
		return true;
	}
	if (source_.file != nullptr) {
		generative::firmware::showVersionNotice(*source_.file); // PERFORM FROM's FILE... (spec §16.2)
	}
	// Back to the session view first, so nothing above it outlives the song it was opened over.
	soundEditor.exitCompletely();

	// The build is a modal runner job: the player's song plays on under the working animation, and BACK cancels.
	generativeModalUI.open(generative::firmware::performAdapter.entryJob());
	if (!generative::firmware::performAdapter.beginEntry(source_, entryEnded)) {
		generativeModalUI.jobEnded();
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_GENERATIVE_BUSY));
	}
	return true;
}

// ---- REGENERATE? (the Compose screen's) ----

char const* RegenerateRecipe::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_REGENERATE_QMARK);
}

std::span<char const*> RegenerateRecipe::getOptions() {
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_OK)};
	return {options, 1};
}

bool RegenerateRecipe::acceptCurrentOption() {
	close();
	menu_item::generative::regenerateComposeRecipe();
	return true;
}

// ---- The leave menu ----

bool LeavePerform::setupAndCheckAvailability() {
	using generative::perform::LeaveOption;
	using enum l10n::String;
	generative::perform::leaveMenu(generative::firmware::performAdapter.steeringLog(), menu_);
	for (int32_t i = 0; i < menu_.count; i++) {
		static constexpr l10n::String kNames[] = {
		    STRING_FOR_MARKED_AND_LAST, STRING_FOR_MARKED_ONLY, STRING_FOR_MARKED, STRING_FOR_LAST,
		    STRING_FOR_NOTHING,         STRING_FOR_LEAVE,       STRING_FOR_CANCEL};
		static_assert(std::size(kNames) == static_cast<size_t>(LeaveOption::Cancel) + 1);
		options_[i] = l10n::get(kNames[static_cast<int32_t>(menu_.options[i])]);
	}
	currentOption = 0; // it starts on the first option (spec §13.8)
	scrollPos = 0;
	return true;
}

char const* LeavePerform::getTitle() {
	return menu_.title;
}

std::span<char const*> LeavePerform::getOptions() {
	return {options_, static_cast<size_t>(menu_.count)};
}

bool LeavePerform::acceptCurrentOption() {
	using generative::perform::LeaveOption;
	const LeaveOption option = menu_.options[currentOption];
	if (option == LeaveOption::Cancel) {
		return false; // back to Perform, which never stopped
	}
	close();
	performView.leave(option);
	return true;
}

} // namespace deluge::gui::context_menu
