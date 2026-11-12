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
#include "generative/firmware/transitions.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/perform_from.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"

namespace deluge::gui::context_menu {

Generative generativeMenu{};
DiscardSong discardSong{};
LeavePerform leavePerform{};
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
	if (genre_ == nullptr) {
		return false;
	}
	// Back to the session view first, so nothing above it outlives the song it was opened over.
	soundEditor.exitCompletely();

	// The build is a modal runner job: the player's song plays on under the working animation, and BACK cancels.
	generativeModalUI.open(true);
	if (!generative::firmware::performAdapter.beginEntry(*genre_, entryEnded)) {
		generativeModalUI.jobEnded();
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_GENERATIVE_BUSY));
	}
	return true;
}

// ---- LEAVE PERFORM? ----

char const* LeavePerform::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_LEAVE_PERFORM_QMARK);
}

std::span<char const*> LeavePerform::getOptions() {
	static char const* options[] = {l10n::get(l10n::String::STRING_FOR_LEAVE),
	                                l10n::get(l10n::String::STRING_FOR_CANCEL)};
	return {options, 2};
}

bool LeavePerform::acceptCurrentOption() {
	if (currentOption != 0) {
		return false; // CANCEL: back to Perform, which never stopped
	}
	close();
	performView.leave();
	return true;
}

// ---- REGENERATE? ----

char const* TransitionConfirm::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_REGENERATE_QMARK);
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
	// The incoming genre's transition entry is highlighted (spec §9.7): Drop for every M1 genre, which none sets
	// otherwise. Hard cut is never a default.
	currentOption = static_cast<int32_t>(generative::TransitionStyle::Drop);
	scrollPos = currentOption;
	return true;
}

bool TransitionConfirm::acceptCurrentOption() {
	using generative::firmware::Transitions;
	const auto style = static_cast<generative::TransitionStyle>(currentOption);
	// Back to the Perform page, which shows the working animation while the incoming roster loads (spec §12.10).
	soundEditor.exitCompletely();
	display->displayLoadingAnimationText(l10n::get(l10n::String::STRING_FOR_LOADING));
	const Transitions::Request request = generative::firmware::transitions.regenerate(style);
	if (request != Transitions::Request::Started) {
		display->removeWorkingAnimation();
	}
	showPressPopup(generative::perform::pressOutcome(request));
	TEST_EVENT("gen_confirm regenerate style=%d result=%d", static_cast<int>(style), static_cast<int>(request));
	return true;
}

} // namespace deluge::gui::context_menu
