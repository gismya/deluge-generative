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

#include "gui/menu_item/generative/regenerate_page.h"
#include "definitions.h"
#include "generative/core/genre.h"
#include "generative/core/recipe_file.h"
#include "generative/core/variants.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/transitions.h"
#include "generative/firmware/working_animation.h"
#include "generative/perform/recipe_name.h"
#include "generative/perform/regenerate_page.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/row_value.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace deluge::gui::menu_item::generative {

namespace gen = deluge::generative;
using gen::firmware::currentRecipe;
using gen::perform::AllowedShown;

namespace {

// ---- What the page and the VARIANTS rows edit ----

PLACE_SDRAM_BSS gen::perform::RegeneratePage page;

/// What the firmware keeps beside the page while it's open. Whether it's a LOAD is the page's (page.load()).
struct PageOpening {
	/// A LOAD's file and name, or nullptr.
	const gen::RecipeFile* file = nullptr;
	const gen::perform::RecipeName* name = nullptr;
	/// Opened on the genre playing: a Regenerate, titled REGENERATE. Decided as it opens, so a pending change to the
	/// same genre landing meanwhile leaves the title, and what GO reports, as the player saw them.
	bool regenerate = false;
	/// Just opened: its next session starts on GO. A session begun again later (a popup over it closing) keeps the
	/// row it was on.
	bool fresh = false;
};
PageOpening opening;

/// What a row's value text needs: RANDOM's l10n text, a variant's OLED name or `n OF m`, and the NUL.
constexpr size_t kAllowedTextChars = 12;

/// The sets the VARIANTS rows show, and the mask per set they edit: the page's, or the Current Recipe's on the Compose
/// screen. A VARIANTS menu points it at its own as it begins.
struct VariantsTarget {
	const gen::Genre* genre = nullptr;
	uint8_t* allowed = nullptr;    ///< kMaxVariantSets masks; nullptr for Perform's
	gen::Recipe* recipe = nullptr; ///< The Recipe whose masks they are (the Compose screen's), with picks to keep in
	                               ///< them; nullptr for the page's, which has none
	/// Perform's VARIANTS ▸ (spec §13.6): the ticks are the playing piece's as they go on (maskAhead()), a tick turned
	/// queues them for the next Drop, and a set that isn't live opens nothing (AT REGEN).
	bool perform = false;
};
VariantsTarget target;
/// The set whose multi-select is open.
int32_t openSet = 0;

[[nodiscard]] int32_t setCount() {
	return target.genre != nullptr ? static_cast<int32_t>(target.genre->variantSets.size()) : 0;
}

/// Whether Perform's target names another genre than the roster playing: a genre change landed while the menu was
/// open, so its set indices no longer name the same sets (#508 R2).
[[nodiscard]] bool performTargetStale() {
	return target.perform && target.genre != gen::firmware::performAdapter.steering().rosterGenre();
}

/// Set `s`'s ticks as its row and toggles show them.
[[nodiscard]] uint8_t maskOf(int32_t s) {
	if (performTargetStale()) {
		return gen::allowedIn(target.genre->variantSets[s], 0); // shown as RANDOM; a tick queues nothing
	}
	return target.perform ? gen::firmware::performAdapter.steering().maskAhead(s)
	                      : gen::allowedIn(target.genre->variantSets[s], target.allowed[s]);
}

/// What set `s`'s row shows (spec §13.6): RANDOM, the one variant's name, or `n OF m`.
const char* allowedText(const gen::VariantSet& set, uint8_t allowed, char (&text)[kAllowedTextChars]) {
	const AllowedShown shown = gen::perform::allowedShown(set, allowed);
	switch (shown.kind) {
	case AllowedShown::Kind::Random:
		return l10n::get(l10n::String::STRING_FOR_RANDOM);
	case AllowedShown::Kind::One:
		return set.variants[shown.variant].oledName;
	case AllowedShown::Kind::Subset:
		snprintf(text, sizeof(text), l10n::get(l10n::String::STRING_FOR_VARIANTS_SUBSET), shown.count, shown.of);
		return text;
	}
	return "";
}

// ---- TRANSITION ----

/// In TransitionStyle order.
constexpr l10n::String kStyleNames[] = {l10n::String::STRING_FOR_TRANSITION_DROP,
                                        l10n::String::STRING_FOR_TRANSITION_FADE,
                                        l10n::String::STRING_FOR_TRANSITION_CUT};
static_assert(std::size(kStyleNames) == gen::kNumTransitionStyles);

/// TRANSITION: DROP, FADE or CUT (spec §9.7), starting on the incoming genre's transition entry. A stock selection,
/// its value shown right-aligned on the page.
class TransitionRow final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (l10n::String name : kStyleNames) {
			options.push_back(l10n::getView(name));
		}
		return options;
	}
	void readCurrentValue() override { setValue(page.style()); }
	void writeCurrentValue() override {
		page.setStyle(getValue<gen::TransitionStyle>());
		TEST_EVENT("gen_regen_page style=%d", static_cast<int>(page.style()));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	static const char* shown() { return l10n::get(kStyleNames[static_cast<int32_t>(page.style())]); }
};

// ---- VARIANTS ▸ ----

/// A variant's row in its set's multi-select: a stock toggle, ticked when the set allows it. The last tick stays.
class VariantToggle final : public Toggle {
public:
	explicit VariantToggle(int32_t variant) : Toggle(l10n::String::STRING_FOR_VARIANTS), variant_(variant) {}
	[[nodiscard]] std::string_view getName() const override {
		return relevant() ? set().variants[variant_].oledName : "";
	}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return relevant();
	}
	void readCurrentValue() override { setValue(gen::perform::allows(mask(), variant_)); }
	void writeCurrentValue() override {
		if (getValue() == gen::perform::allows(mask(), variant_)) {
			return;
		}
		const uint8_t allowed = gen::perform::toggledAllowed(set(), mask(), variant_);
		if (target.perform) {
			// Queued for the next Drop (VARIANT DROP); refused while a transition is pending, the ticks stay. The last
			// tick, which stays, queues nothing.
			if (performTargetStale()) {
				TEST_EVENT("gen_variants_press set=%s stale", set().name);
			}
			else if (allowed != mask()) {
				const gen::perform::PressOutcome outcome =
				    gen::firmware::performAdapter.steering().pressVariantMask(openSet, allowed);
				refusedWithPopup(outcome);
				TEST_EVENT("gen_variants_press set=%s allowed=%u outcome=%d", set().name,
				           static_cast<unsigned>(allowed), static_cast<int>(outcome));
			}
		}
		else if (target.recipe != nullptr) {
			// The Compose screen's: a pick no longer allowed is drawn again within the ticks (spec §14.4).
			gen::perform::narrowSet(*target.recipe, openSet, allowed, gen::firmware::freshEntropy());
		}
		else {
			target.allowed[openSet] = allowed;
		}
		setValue(gen::perform::allows(mask(), variant_)); // the last tick stays
		char text[kAllowedTextChars];
		TEST_EVENT("gen_variants set=%s allowed=%u shows=%s", set().name, static_cast<unsigned>(mask()),
		           allowedText(set(), mask(), text));
		if (target.recipe != nullptr) {
			char picks[gen::perform::kPicksTextChars];
			gen::perform::picksText(*target.genre, target.recipe->variants, picks);
			TEST_EVENT("gen_compose_variants toggled %s", picks);
		}
	}

private:
	[[nodiscard]] bool relevant() const {
		return openSet < setCount() && variant_ < static_cast<int32_t>(set().variants.size());
	}
	static const gen::VariantSet& set() { return target.genre->variantSets[openSet]; }
	static uint8_t mask() { return maskOf(openSet); }
	int32_t variant_;
};

/// A set's row in VARIANTS ▸ (spec §13.6): its OLED name and what it allows, opening its multi-select, titled with
/// its name.
class VariantSetRow final : public Submenu {
public:
	VariantSetRow(int32_t set, std::span<MenuItem*> toggles)
	    : Submenu(l10n::String::STRING_FOR_VARIANTS, toggles), set_(set) {}
	[[nodiscard]] std::string_view getName() const override {
		return set_ < setCount() ? target.genre->variantSets[set_].oledName : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return set_ < setCount();
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		openSet = set_;
		Submenu::beginSession(navigatedBackwardFrom);
	}
	// A set that isn't live opens nothing in Perform (Submenu::selectButtonPress() is final): the sound editor asks
	// this before it enters the row, and the popup says why.
	MenuPermission checkPermissionToBeginSession(ModControllableAudio* modControllable, int32_t whichThing,
	                                             MultiRange** currentRange) override {
		if (!opens()) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_AT_REGEN));
			TEST_EVENT("gen_variants_live set=%s at_regen", target.genre->variantSets[set_].name);
			return MenuPermission::NO;
		}
		return Submenu::checkPermissionToBeginSession(modControllable, whichThing, currentRange);
	}
	int32_t getSubmenuItemTypeRenderLength() override {
		return opens() ? valueAndArrowRenderLength(shown()) : valueRenderLength(shown());
	}
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		if (opens()) {
			drawRowValueAndArrow(*this, shown(), yPixel);
		}
		else {
			drawRowValue(shown(), yPixel);
		}
	}

private:
	/// Whether its multi-select opens: always, but in Perform only for a live set (spec §13.6).
	[[nodiscard]] bool opens() const { return !target.perform || target.genre->variantSets[set_].live; }
	const char* shown() { return allowedText(target.genre->variantSets[set_], maskOf(set_), text_); }
	int32_t set_;
	char text_[kAllowedTextChars] = {};
};

/// VARIANTS ▸: a row per set of `target_()`'s genre. The page's and the Compose screen's are two of these over the
/// same rows.
class VariantsMenu final : public Submenu {
public:
	VariantsMenu(l10n::String name, std::span<MenuItem*> rows, VariantsTarget (*targetOf)(), bool (*shows)())
	    : Submenu(name, rows), targetOf_(targetOf), shows_(shows) {}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return shows_();
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		target = targetOf_();
		if (navigatedBackwardFrom == nullptr) {
			TEST_EVENT("gen_variants_menu open sets=%ld", static_cast<long>(setCount()));
			for (int32_t s = 0; s < setCount(); s++) {
				char text[kAllowedTextChars];
				const gen::VariantSet& set = target.genre->variantSets[s];
				TEST_EVENT("gen_variants_row %s %s", set.oledName, allowedText(set, maskOf(s), text));
			}
		}
		Submenu::beginSession(navigatedBackwardFrom);
	}

private:
	VariantsTarget (*targetOf_)();
	bool (*shows_)();
};

// ---- GO ----

/// GO: the page's choice starts the transition (spec §9.7): a genre change or Regenerate rolled within the narrowed
/// sets, or the LOAD. The menu closes back to the Perform page, which shows the working animation while the incoming
/// roster loads; one refused (Committed or Retiring, §12.9) shows CHANGE PENDING.
class GoRow final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	int32_t getSubmenuItemTypeRenderLength() override { return 0; }
	void renderSubmenuItemTypeForOled(int32_t /*yPixel*/) override {}
	MenuItem* selectButtonPress() override {
		using gen::firmware::Transitions;
		const gen::perform::RegeneratePage::Choice choice = page.go();
		const gen::Genre* genre = page.genre();
		const PageOpening opened = opening;
		const bool load = page.load();
		soundEditor.exitCompletely();
		const Transitions::Request request = gen::firmware::pressShowingWork([&] {
			return load ? gen::firmware::transitions.load(*opened.file, *opened.name, choice.style)
			            : gen::firmware::transitions.change(*genre, choice.style, choice.narrowed);
		});
		refusedWithPopup(gen::perform::pressOutcome(request));
		char masks[gen::perform::kPicksTextChars];
		gen::perform::masksText(*genre, choice.narrowed, masks);
		TEST_EVENT("gen_confirm %s style=%d result=%d working=%d allowed=%s",
		           load                ? "load"
		           : opened.regenerate ? "regenerate"
		                               : genre->name,
		           static_cast<int>(choice.style), static_cast<int>(request),
		           gen::firmware::workingAnimationShown() ? 1 : 0, masks);
		return NO_NAVIGATION;
	}
};

// ---- The page ----

/// The page: titled REGENERATE for a Regenerate, `<GENRE>?` for a genre change and `<NAME>?` for a LOAD (its genre's
/// when the file has no name). A title wider than the screen (a long file name) side-scrolls, as the Compose
/// screen's does.
class PageMenu final : public Submenu {
public:
	PageMenu(l10n::String name, std::initializer_list<MenuItem*> items, MenuItem* go) : Submenu(name, items), go_(go) {}
	[[nodiscard]] std::string_view getTitle() const override {
		if (opening.regenerate) {
			return l10n::getView(l10n::String::STRING_FOR_REGENERATE);
		}
		// The `?` is punctuation, as in STRING_FOR_REGENERATE_QMARK's text: no language this firmware ships puts the
		// name anywhere else.
		const char* name = page.load() && !opening.name->empty() ? opening.name->text : page.genre()->oledName;
		const size_t length = std::min(std::strlen(name), sizeof(title_) - 2);
		std::memcpy(title_, name, length);
		title_[length] = '?';
		title_[length + 1] = 0;
		return title_;
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		if (navigatedBackwardFrom == nullptr && opening.fresh) {
			opening.fresh = false;
			navigatedBackwardFrom = go_; // SELECT, SELECT is as quick as the old confirm
			const std::string_view title = getTitle();
			TEST_EVENT("gen_regen_page open title=%.*s style=%d variants=%d", static_cast<int>(title.size()),
			           title.data(), static_cast<int>(page.style()), page.showsVariants() ? 1 : 0);
		}
		Submenu::beginSession(navigatedBackwardFrom);
	}
	void renderOLED() override {
		drawScrollingTitle(getTitle());
		drawPixelsForOled();
	}

private:
	MenuItem* go_;
	/// `<OLED NAME>?` or a LOAD's `<NAME>?`: the longest Recipe name (longer than any genre's), the `?` and the NUL.
	mutable char title_[gen::perform::kMaxRecipeNameChars + 2] = {};
};

VariantsTarget pageTarget() {
	return {.genre = page.genre(), .allowed = page.narrowed().allowed};
}
bool pageShowsVariants() {
	return page.showsVariants();
}
VariantsTarget composeTarget() {
	return {.genre = currentRecipe.recipe.genre,
	        .allowed = currentRecipe.recipe.variants.allowed,
	        .recipe = &currentRecipe.recipe};
}
VariantsTarget performTarget() {
	return {.genre = gen::firmware::performAdapter.steering().rosterGenre(), .perform = true};
}
bool performShowsVariants() {
	const gen::Genre* genre = gen::firmware::performAdapter.steering().rosterGenre();
	return genre != nullptr && !genre->variantSets.empty();
}
bool composeShowsVariants() {
	return currentRecipe.exists && currentRecipe.recipe.genre != nullptr
	       && !currentRecipe.recipe.genre->variantSets.empty();
}

static_assert(gen::kMaxVariants == 8, "one VariantToggle per variant below");
PLACE_SDRAM_BSS VariantToggle variantToggles[gen::kMaxVariants] = {VariantToggle{0}, VariantToggle{1}, VariantToggle{2},
                                                                   VariantToggle{3}, VariantToggle{4}, VariantToggle{5},
                                                                   VariantToggle{6}, VariantToggle{7}};
MenuItem* toggleList[gen::kMaxVariants] = {&variantToggles[0], &variantToggles[1], &variantToggles[2],
                                           &variantToggles[3], &variantToggles[4], &variantToggles[5],
                                           &variantToggles[6], &variantToggles[7]};
static_assert(gen::kMaxVariantSets == 8, "one VariantSetRow per set below");
PLACE_SDRAM_BSS VariantSetRow setRows[gen::kMaxVariantSets] = {
    VariantSetRow{0, toggleList}, VariantSetRow{1, toggleList}, VariantSetRow{2, toggleList},
    VariantSetRow{3, toggleList}, VariantSetRow{4, toggleList}, VariantSetRow{5, toggleList},
    VariantSetRow{6, toggleList}, VariantSetRow{7, toggleList}};
MenuItem* setRowList[gen::kMaxVariantSets] = {&setRows[0], &setRows[1], &setRows[2], &setRows[3],
                                              &setRows[4], &setRows[5], &setRows[6], &setRows[7]};

PLACE_SDRAM_BSS TransitionRow transitionRow{l10n::String::STRING_FOR_TRANSITION};
PLACE_SDRAM_BSS VariantsMenu pageVariants{l10n::String::STRING_FOR_VARIANTS, setRowList, pageTarget, pageShowsVariants};
PLACE_SDRAM_BSS VariantsMenu composeVariants{l10n::String::STRING_FOR_VARIANTS, setRowList, composeTarget,
                                             composeShowsVariants};
PLACE_SDRAM_BSS VariantsMenu performVariants{l10n::String::STRING_FOR_VARIANTS, setRowList, performTarget,
                                             performShowsVariants};
PLACE_SDRAM_BSS GoRow goRow{l10n::String::STRING_FOR_GO};
// OPTIONS ▸ (keeping MIDI instruments, the reload wait) goes between VARIANTS and GO when it's built (ADR 0008).
PLACE_SDRAM_BSS PageMenu pageMenu{l10n::String::STRING_FOR_REGENERATE, {&transitionRow, &pageVariants, &goRow}, &goRow};

} // namespace

MenuItem& composeVariantsMenu = composeVariants;
MenuItem& performVariantsMenu = performVariants;

bool showsVariants(const MenuItem* item) {
	bool ours = item == &pageVariants || item == &composeVariants || item == &performVariants;
	for (const VariantSetRow& row : setRows) {
		ours = ours || item == &row;
	}
	for (const VariantToggle& toggle : variantToggles) {
		ours = ours || item == &toggle;
	}
	return ours;
}

void openRegeneratePage(const gen::Genre& genre) {
	page.open(genre, false);
	opening = {.regenerate = &genre == currentRecipe.recipe.genre, .fresh = true};
	soundEditor.enterSubmenu(&pageMenu);
}

void openLoadPage(const gen::RecipeFile& file, const gen::perform::RecipeName& name) {
	page.open(*file.recipe.genre, true);
	opening = {.file = &file, .name = &name, .fresh = true};
	if (soundEditor.setup(nullptr, &pageMenu)) {
		display->setNextTransitionDirection(1);
		openUI(&soundEditor);
	}
}

} // namespace deluge::gui::menu_item::generative
