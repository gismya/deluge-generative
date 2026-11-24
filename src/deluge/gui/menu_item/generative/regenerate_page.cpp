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
/// A LOAD's file and name while its page is open, or nullptr.
const gen::RecipeFile* loadFile = nullptr;
const gen::perform::RecipeName* loadName = nullptr;
/// The page was just opened: its next session starts on GO. A session begun again later (a popup over it closing)
/// keeps the row it was on.
bool freshOpen = false;

/// The sets the VARIANTS rows show, and the mask per set they edit: the page's, or the Current Recipe's on the Compose
/// screen. A VARIANTS menu points it at its own as it begins.
struct VariantsTarget {
	const gen::Genre* genre = nullptr;
	uint8_t* allowed = nullptr; ///< kMaxVariantSets masks
};
VariantsTarget target;
/// The set whose multi-select is open.
int32_t openSet = 0;

[[nodiscard]] int32_t setCount() {
	return target.genre != nullptr ? static_cast<int32_t>(target.genre->variantSets.size()) : 0;
}

/// What set `s`'s row shows (spec §13.6): RANDOM, the one variant's name, or `n OF m`.
const char* allowedText(const gen::VariantSet& set, uint8_t allowed, char (&text)[12]) {
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

/// Whether the page opened on the genre playing: a Regenerate.
[[nodiscard]] bool regenerates() {
	return loadFile == nullptr && page.genre() == currentRecipe.recipe.genre;
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
		target.allowed[openSet] = gen::perform::toggledAllowed(set(), mask(), variant_);
		setValue(gen::perform::allows(mask(), variant_)); // the last tick stays
		char text[12];
		TEST_EVENT("gen_variants set=%s allowed=%u shows=%s", set().name, static_cast<unsigned>(mask()),
		           allowedText(set(), mask(), text));
	}

private:
	[[nodiscard]] bool relevant() const {
		return openSet < setCount() && variant_ < static_cast<int32_t>(set().variants.size());
	}
	static const gen::VariantSet& set() { return target.genre->variantSets[openSet]; }
	static uint8_t mask() { return gen::allowedIn(set(), target.allowed[openSet]); }
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
	int32_t getSubmenuItemTypeRenderLength() override { return valueAndArrowRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValueAndArrow(*this, shown(), yPixel); }

private:
	const char* shown() { return allowedText(target.genre->variantSets[set_], target.allowed[set_], text_); }
	int32_t set_;
	char text_[12] = {};
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
				char text[12];
				const gen::VariantSet& set = target.genre->variantSets[s];
				TEST_EVENT("gen_variants_row %s %s", set.oledName, allowedText(set, target.allowed[s], text));
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
		const gen::RecipeFile* file = loadFile;
		const gen::perform::RecipeName* name = loadName;
		const bool regenerate = regenerates();
		soundEditor.exitCompletely();
		const Transitions::Request request = gen::firmware::pressShowingWork([&] {
			return file != nullptr ? gen::firmware::transitions.load(*file, *name, choice.style)
			                       : gen::firmware::transitions.change(*genre, choice.style, choice.narrowed);
		});
		refusedWithPopup(gen::perform::pressOutcome(request));
		TEST_EVENT("gen_confirm %s style=%d result=%d working=%d allowed=%u",
		           file != nullptr ? "load"
		           : regenerate    ? "regenerate"
		                           : genre->name,
		           static_cast<int>(choice.style), static_cast<int>(request),
		           gen::firmware::workingAnimationShown() ? 1 : 0, static_cast<unsigned>(choice.narrowed.allowed[0]));
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
		if (regenerates()) {
			return l10n::getView(l10n::String::STRING_FOR_REGENERATE);
		}
		// The `?` is punctuation, as in STRING_FOR_REGENERATE_QMARK's text: no language this firmware ships puts the
		// name anywhere else.
		const char* name = loadFile != nullptr && !loadName->empty() ? loadName->text : page.genre()->oledName;
		const size_t length = std::min(std::strlen(name), sizeof(title_) - 2);
		std::memcpy(title_, name, length);
		title_[length] = '?';
		title_[length + 1] = 0;
		return title_;
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		if (navigatedBackwardFrom == nullptr && freshOpen) {
			freshOpen = false;
			navigatedBackwardFrom = go_; // SELECT, SELECT is as quick as the old confirm
			const std::string_view title = getTitle();
			TEST_EVENT("gen_regen_page open title=%.*s style=%d variants=%d", static_cast<int>(title.size()),
			           title.data(), static_cast<int>(page.style()), page.showsVariants() ? 1 : 0);
		}
		Submenu::beginSession(navigatedBackwardFrom);
	}
	void renderOLED() override {
		hid::display::oled_canvas::Canvas& canvas = hid::display::OLED::main;
		const std::string_view title = getTitle();
		constexpr int32_t kTitleY = OLED_MAIN_TOPMOST_PIXEL + 1;
		canvas.drawString(title, 0, kTitleY, kTextTitleSpacingX, kTextTitleSizeY);
		canvas.drawHorizontalLine(kScreenTitleSeparatorY, 0, OLED_MAIN_WIDTH_PIXELS - 1);
		if (canvas.getStringWidthInPixels(title.data(), kTextTitleSizeY) > OLED_MAIN_WIDTH_PIXELS) {
			hid::display::OLED::setupSideScroller(1, title, 0, OLED_MAIN_WIDTH_PIXELS, kTitleY,
			                                      kTitleY + kTextTitleSizeY, kTextTitleSpacingX, kTextTitleSizeY,
			                                      false);
		}
		hid::display::OLED::markChanged();
		drawPixelsForOled();
	}

private:
	MenuItem* go_;
	/// `<OLED NAME>?` or a LOAD's `<NAME>?`: the longest Recipe name (longer than any genre's), the `?` and the NUL.
	mutable char title_[gen::perform::kMaxRecipeNameChars + 2] = {};
};

VariantsTarget pageTarget() {
	return {page.genre(), page.narrowed().allowed};
}
bool pageShowsVariants() {
	return page.showsVariants();
}
VariantsTarget composeTarget() {
	return {currentRecipe.recipe.genre, currentRecipe.recipe.variants.allowed};
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
PLACE_SDRAM_BSS GoRow goRow{l10n::String::STRING_FOR_GO};
// OPTIONS ▸ (keeping MIDI instruments, the reload wait) goes between VARIANTS and GO when it's built (ADR 0008).
PLACE_SDRAM_BSS PageMenu pageMenu{l10n::String::STRING_FOR_REGENERATE, {&transitionRow, &pageVariants, &goRow}, &goRow};

} // namespace

MenuItem& composeVariantsMenu = composeVariants;

void openRegeneratePage(const gen::Genre& genre) {
	loadFile = nullptr;
	loadName = nullptr;
	page.open(genre, false);
	freshOpen = true;
	soundEditor.enterSubmenu(&pageMenu);
}

void openLoadPage(const gen::RecipeFile& file, const gen::perform::RecipeName& name) {
	loadFile = &file;
	loadName = &name;
	page.open(*file.recipe.genre, true);
	freshOpen = true;
	if (soundEditor.setup(nullptr, &pageMenu)) {
		display->setNextTransitionDirection(1);
		openUI(&soundEditor);
	}
}

} // namespace deluge::gui::menu_item::generative
