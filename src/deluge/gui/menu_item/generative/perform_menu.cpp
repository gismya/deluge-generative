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

#include "gui/menu_item/generative/perform_menu.h"
#include "definitions.h"
#include "generative/core/genre.h"
#include "generative/core/pitch.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/glide.h"
#include "generative/perform/queue_text.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/views/perform_view.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include <cstdio>
#include <cstring>

namespace deluge::gui::menu_item::generative {

using deluge::generative::Key;
using deluge::generative::kNumScales;
using deluge::generative::Scale;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
namespace perform = deluge::generative::perform;

namespace {

/// A row's value, right-aligned after its name in a submenu, as the Compose screen's rows show theirs (spec §14.4).
constexpr int32_t kValueRightMargin = 3;
int32_t valueRenderLength(const char* value) {
	return 4 + static_cast<int32_t>(strlen(value)) * kTextSpacingX + kValueRightMargin;
}
void drawRowValue(const char* value, int32_t yPixel) {
	deluge::hid::display::OLED::main.drawStringAlignRight(value, yPixel, kTextSpacingX, kTextSpacingY,
	                                                      OLED_MAIN_WIDTH_PIXELS - kValueRightMargin);
}

// ---- KEY / SCALE ----

/// The key the editor shows, from the key ahead when the editor opens. Each turn changes it and queues it whole, so
/// a turn of KEY right after one of SCALE keeps the new scale even before the core has drained the first change.
Key edited;

void queueEdited() {
	PerformView::popupIfRefused(performAdapter.queueKeyChange(edited));
}

class KeyScaleSubmenu final : public Submenu {
public:
	using Submenu::Submenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		if (navigatedBackwardFrom == nullptr) {
			edited = performAdapter.keyAhead(); // opened, not back from KEY or SCALE
		}
		Submenu::beginSession(navigatedBackwardFrom);
	}
};

/// KEY: the key root, C to B.
class KeyItem final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (uint8_t root = 0; root < 12; root++) {
			options.push_back(perform::keyName(root));
		}
		return options;
	}
	void readCurrentValue() override { setValue(edited.root); }
	void writeCurrentValue() override {
		edited.root = static_cast<uint8_t>(getValue());
		queueEdited();
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(perform::keyName(edited.root)); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(perform::keyName(edited.root), yPixel); }
};

/// SCALE: the scale, in the Scale order.
class ScaleItem final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (int32_t scale = 0; scale < kNumScales; scale++) {
			options.push_back(perform::scaleName(static_cast<Scale>(scale)));
		}
		return options;
	}
	void readCurrentValue() override { setValue(edited.scale); }
	void writeCurrentValue() override {
		edited.scale = getValue<Scale>();
		queueEdited();
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(perform::scaleName(edited.scale)); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		drawRowValue(perform::scaleName(edited.scale), yPixel);
	}
};

// ---- GLIDE ----

/// Indexed by perform::GlideLength.
constexpr l10n::String kGlideNames[] = {
    l10n::String::STRING_FOR_GLIDE_INSTANT,   l10n::String::STRING_FOR_GLIDE_BEAT,
    l10n::String::STRING_FOR_GLIDE_BAR,       l10n::String::STRING_FOR_GLIDE_TWO_BARS,
    l10n::String::STRING_FOR_GLIDE_FOUR_BARS, l10n::String::STRING_FOR_GLIDE_PHRASE,
};
static_assert(std::size(kGlideNames) == static_cast<size_t>(perform::GlideLength::Phrase) + 1);

/// GLIDE: the Perform setting tempo and swing glides take (spec §13.3), kept until power-off.
class GlideItem final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (l10n::String name : kGlideNames) {
			options.push_back(l10n::getView(name));
		}
		return options;
	}
	void readCurrentValue() override { setValue(performAdapter.glide); }
	void writeCurrentValue() override {
		performAdapter.glide = getValue<perform::GlideLength>();
		TEST_EVENT("gen_glide %d", static_cast<int>(performAdapter.glide));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	static const char* shown() { return l10n::get(kGlideNames[static_cast<int32_t>(performAdapter.glide)]); }
};

// ---- SEED + TAKES ----

/// SEED + TAKES, titled with the seed code (spec §6.5): `SEED 1A2B3C4`.
class SeedTakesSubmenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] std::string_view getTitle() const override {
		char code[perform::kSeedCodeChars + 1];
		perform::seedCode(currentRecipe.seed, code);
		snprintf(title_, sizeof(title_), "%s %s", l10n::get(l10n::String::STRING_FOR_SEED), code);
		return title_;
	}

private:
	mutable char title_[24] = {};
};

/// A part's take, 1–16 as shown: a turn steps from the take it shows, back or forward, as a Y turn does from the
/// waiting take (forward past the newest makes a new take). It shows the waiting take, or else the one playing.
class TakeItem final : public Integer {
public:
	explicit TakeItem(int32_t part) : Integer(l10n::String::STRING_FOR_SEED_AND_TAKES), part_(part) {}
	[[nodiscard]] std::string_view getName() const override {
		const deluge::generative::Genre* genre = PerformView::rosterGenre();
		return genre != nullptr && part_ < deluge::generative::partCount(*genre) ? genre->parts[part_].name : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return part_ < PerformView::rosterParts();
	}
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::generative::kMaxTakes; }
	void readCurrentValue() override { setValue(shownTake() + 1); }
	void selectEncoderAction(int32_t offset) override {
		// The row as the CLIP page reads it, with the take this editor shows as the waiting one: a press still in the
		// steering queue isn't waiting in the core yet.
		perform::TakeRow row = PerformView::takeRow(part_);
		row.waiting = getValue() - 1 != row.playing ? getValue() - 1 : -1;
		const int32_t take = perform::steppedTake(row, offset);
		if (take < 0) {
			return;
		}
		setValue(take + 1);
		Number::selectEncoderAction(offset);
	}
	void writeCurrentValue() override {
		const int32_t take = getValue() - 1;
		PerformView::popupIfRefused(performAdapter.pressTake(static_cast<uint8_t>(part_), take));
		TEST_EVENT("gen_press take part=%ld take=%ld menu", static_cast<long>(part_), static_cast<long>(take));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shownText()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shownText(), yPixel); }

private:
	[[nodiscard]] int32_t shownTake() const {
		const perform::TakeRow row = PerformView::takeRow(part_);
		return row.waiting >= 0 ? row.waiting : row.playing;
	}
	const char* shownText() {
		snprintf(text_, sizeof(text_), "%ld", static_cast<long>(shownTake() + 1));
		return text_;
	}
	int32_t part_;
	char text_[4] = {};
};

PLACE_SDRAM_BSS KeyItem keyItem{l10n::String::STRING_FOR_KEY};
PLACE_SDRAM_BSS ScaleItem scaleItem{l10n::String::STRING_FOR_SCALE};
PLACE_SDRAM_BSS GlideItem glideItem{l10n::String::STRING_FOR_GLIDE};

static_assert(deluge::generative::kMaxParts == 6, "one TakeItem per part slot below");
PLACE_SDRAM_BSS TakeItem takeItems[deluge::generative::kMaxParts] = {TakeItem{0}, TakeItem{1}, TakeItem{2},
                                                                     TakeItem{3}, TakeItem{4}, TakeItem{5}};
PLACE_SDRAM_BSS SeedTakesSubmenu seedTakesMenu{
    l10n::String::STRING_FOR_SEED_AND_TAKES,
    {&takeItems[0], &takeItems[1], &takeItems[2], &takeItems[3], &takeItems[4], &takeItems[5]},
};

PLACE_SDRAM_BSS KeyScaleSubmenu keyScaleMenuItem{l10n::String::STRING_FOR_KEY_SCALE, {&keyItem, &scaleItem}};

} // namespace

Submenu& keyScaleMenu = keyScaleMenuItem;

PLACE_SDRAM_BSS Submenu performMenu{l10n::String::STRING_FOR_PERFORM, {&keyScaleMenuItem, &glideItem, &seedTakesMenu}};

} // namespace deluge::gui::menu_item::generative
