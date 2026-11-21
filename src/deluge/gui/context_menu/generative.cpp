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
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/compose_screen.h"
#include "gui/menu_item/generative/perform_from.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <cstring>

namespace deluge::gui::context_menu {

Generative generativeMenu{};
DiscardSong discardSong{};
LeavePerform leavePerform{};
RegenerateRecipe regenerateRecipe{};
TransitionConfirm transitionConfirm{};

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

// ---- REGENERATE?, or the genre changed to ----

bool TransitionConfirm::regenerates() const {
	return file_ == nullptr && genre_ == generative::firmware::currentRecipe.recipe.genre;
}

char const* TransitionConfirm::getTitle() {
	if (regenerates()) {
		return l10n::get(l10n::String::STRING_FOR_REGENERATE_QMARK);
	}
	// A genre change: the incoming genre's full name, asked (`MELODIC TECHNO?`); a LOAD: the file's name (one too long
	// to remember has none, and shows its genre's). The `?` is punctuation, as in STRING_FOR_REGENERATE_QMARK's text:
	// no language this firmware ships puts the name anywhere else.
	const char* name = file_ != nullptr && !name_->empty() ? name_->text : genre_->oledName;
	const size_t length = std::min(std::strlen(name), sizeof(title_) - 2);
	std::memcpy(title_, name, length);
	title_[length] = '?';
	title_[length + 1] = 0;
	return title_;
}

std::span<char const*> TransitionConfirm::getOptions() {
	// In TransitionStyle order.
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_BUILD_INTO_A_DROP),
	                                l10n::get(l10n::String::STRING_FOR_FADE_THROUGH),
	                                l10n::get(l10n::String::STRING_FOR_HARD_CUT_AT_PHRASE)};
	static_assert(std::size(options) == generative::kNumTransitionStyles);
	return {options, std::size(options)};
}

bool TransitionConfirm::setupAndCheckAvailability() {
	if (genre_ == nullptr) {
		return false;
	}
	// The incoming genre's transition entry is highlighted (spec §9.7): the playing genre's for a Regenerate, which
	// genre_ is then. Hard cut is never a default.
	currentOption = static_cast<int32_t>(generative::perform::confirmStyle(*genre_));
	scrollPos = currentOption;
	return true;
}

bool TransitionConfirm::acceptCurrentOption() {
	using generative::firmware::Transitions;
	const auto style = static_cast<generative::TransitionStyle>(currentOption);
	const bool regenerate = regenerates();
	// Back to the Perform page, which shows the working animation while the incoming roster loads (spec §12.10): out of
	// the Perform menu, or (a LOAD's, opened from the load browser) straight over Perform.
	if (isUIOpen(&soundEditor)) {
		soundEditor.exitCompletely();
	}
	else {
		close();
	}
	const Transitions::Request request = generative::firmware::pressShowingWork([&] {
		return file_ != nullptr ? generative::firmware::transitions.load(*file_, *name_, style)
		                        : generative::firmware::transitions.change(*genre_, style);
	});
	refusedWithPopup(generative::perform::pressOutcome(request));
	TEST_EVENT("gen_confirm %s style=%d result=%d working=%d",
	           file_ != nullptr ? "load"
	           : regenerate     ? "regenerate"
	                            : genre_->name,
	           static_cast<int>(style), static_cast<int>(request),
	           generative::firmware::workingAnimationShown() ? 1 : 0);
	return true;
}

} // namespace deluge::gui::context_menu
