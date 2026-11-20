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

// Generative mode's context menus (spec §14.1, §14.3, §13.8), all stock context menus:
//   GENERATIVE, opened by SHIFT+SONG in session view over the session view: PERFORM, COMPOSE;
//   DISCARD SONG?, asked every time before Generative mode replaces the song: OK;
//   REGENERATE? (the Compose screen's), from its REGENERATE: OK;
//   LEAVE PERFORM?, opened by holding BACK in Perform: LEAVE, CANCEL (M4 adds Capture's keep options);
//   REGENERATE?, from the Perform menu's REGENERATE or GENRE, and a LOAD while playing: BUILD INTO A DROP, FADE
//   THROUGH, HARD CUT AT PHRASE.

#pragma once

#include "generative/core/recipe_file.h"
#include "generative/firmware/entry_source.h"
#include "generative/perform/genre_menu.h"
#include "generative/perform/recipe_name.h"
#include "gui/context_menu/context_menu.h"

namespace deluge::generative {
struct Genre;
}

namespace deluge::gui::context_menu {

/// A context menu whose presses act at once, inside a card wait too: a Generative handler never returns
/// REMIND_ME_OUTSIDE_CARD_ROUTINE (spec §12.10). None of their options touches the card from the handler: card work
/// goes to the runner.
class GenerativeContextMenu : public ContextMenu {
public:
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override {
		return ContextMenu::buttonAction(b, on, false);
	}
	ActionResult padAction(int32_t x, int32_t y, int32_t on) override;
};

class Generative final : public GenerativeContextMenu {
public:
	Generative() = default;
	bool canSeeViewUnderneath() override { return true; }
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;
};

class DiscardSong final : public GenerativeContextMenu {
public:
	DiscardSong() = default;
	/// PERFORM FROM's pick (spec §14.1, §14.3): OK enters Perform on `source`'s Recipe.
	void setPerform(const generative::firmware::EntrySource& source) {
		source_ = source;
		compose_ = false;
	}
	/// BUILD on the Compose screen (spec §14.5): OK builds the Current Recipe at the Compose screen's LENGTH.
	void setCompose() {
		source_ = {};
		compose_ = true;
	}
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;

private:
	generative::firmware::EntrySource source_;
	bool compose_ = false;
};

/// REGENERATE? on the Compose screen (spec §14.4): OK rolls the Current Recipe again, as REGENERATE does (§9.7).
class RegenerateRecipe final : public GenerativeContextMenu {
public:
	RegenerateRecipe() = default;
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;
};

class LeavePerform final : public GenerativeContextMenu {
public:
	LeavePerform() = default;
	bool canSeeViewUnderneath() override { return true; }
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool acceptCurrentOption() override;
};

/// The transition confirm (spec §9.7, §13.6): how a genre change, Regenerate or LOAD plays. It starts on the incoming
/// genre's transition entry, and its choice starts the transition's preload.
class TransitionConfirm final : public GenerativeContextMenu {
public:
	TransitionConfirm() = default;
	/// The genre to change to: the one playing for a Regenerate. Set before setupAndCheckAvailability().
	void setGenre(const generative::Genre* genre) {
		genre_ = genre;
		file_ = nullptr;
		name_ = nullptr;
	}
	/// A LOAD of `file`, named `name` (spec §16.5): titled with the name, asked. Both must outlive the menu.
	void setLoad(const generative::RecipeFile* file, const generative::perform::RecipeName* name) {
		genre_ = file->recipe.genre;
		file_ = file;
		name_ = name;
	}
	bool canSeeViewUnderneath() override { return true; }
	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool setupAndCheckAvailability() override;
	bool acceptCurrentOption() override;

private:
	[[nodiscard]] bool regenerates() const;
	const generative::Genre* genre_ = nullptr;
	const generative::RecipeFile* file_ = nullptr; ///< A LOAD's file, or nullptr
	const generative::perform::RecipeName* name_ = nullptr;
	/// `<OLED NAME>?` or a LOAD's `<NAME>?`: the longest Recipe name (longer than any genre's, which genre_data_spec
	/// checks), the `?` and the NUL.
	char title_[deluge::generative::perform::kMaxRecipeNameChars + 2] = {};
};

extern Generative generativeMenu;
extern DiscardSong discardSong;
extern LeavePerform leavePerform;
extern RegenerateRecipe regenerateRecipe;
extern TransitionConfirm transitionConfirm;

} // namespace deluge::gui::context_menu
