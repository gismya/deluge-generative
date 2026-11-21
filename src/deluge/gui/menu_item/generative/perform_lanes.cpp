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

#include "gui/menu_item/generative/perform_lanes.h"
#include "generative/compose/compose_screen.h"
#include "generative/core/genre.h"
#include "generative/core/lane_names.h"
#include "generative/core/palette.h"
#include "generative/core/palette_file.h"
#include "generative/firmware/current_recipe.h"
#include "generative/perform/sound_browser.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace deluge::gui::menu_item::generative {

namespace gen = deluge::generative;
namespace compose = deluge::generative::compose;
using gen::Lane;
using gen::firmware::currentRecipe;
using gen::perform::BrowseKind;

namespace {

/// The part the lane list shows, and the lane whose SAMPLE / SYNTH PRESET choice is open.
int32_t listPart = -1;
Lane kindLane = Lane::None;

/// The list's part's lanes, or none when the Current Recipe has no such part.
std::span<const gen::LaneSpec> listLanes() {
	const gen::Genre* genre = currentRecipe.recipe.genre;
	if (genre == nullptr || listPart < 0 || listPart >= static_cast<int32_t>(genre->parts.size())) {
		return {};
	}
	return genre->parts[listPart].lanes;
}

/// RE-ROLL ALL: the whole part's new sound, every lane re-rolled, hand-chosen ones too (spec §13.5).
class ReRollAllItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		const int32_t part = listPart;
		soundEditor.exitCompletely(); // Perform is on top for the press's popup
		performView.reRollPart(part);
		return NO_NAVIGATION;
	}
};

/// The longest lane row: a lane name, a space and the pick's name.
constexpr int32_t kLaneRowChars = 12 + 1 + compose::kPickChars;

/// A lane's row, `KICK 909 Kick`: the lane browser, or for an FX lane that takes a preset the SAMPLE / SYNTH PRESET
/// choice. The pick is part of the row's name, so a long row side-scrolls while selected, as a stock row does.
class LaneRow final : public MenuItem {
public:
	explicit LaneRow(int32_t index) : index_(index) {}
	[[nodiscard]] std::string_view getName() const override {
		const std::optional<Lane> lane = this->lane();
		if (!lane) {
			return "";
		}
		char pick[compose::kPickChars + 1];
		compose::pickName(currentRecipe.sounds.lane[static_cast<int32_t>(*lane)], pick);
		snprintf(name_, sizeof(name_), "%s %s", gen::laneName(*lane), pick);
		return name_;
	}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return lane().has_value();
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override;

private:
	/// The lane this row shows, or none: past the part's lanes.
	[[nodiscard]] std::optional<Lane> lane() const {
		const auto lanes = listLanes();
		if (index_ >= static_cast<int32_t>(lanes.size())) {
			return std::nullopt;
		}
		return lanes[index_].lane;
	}

	int32_t index_;
	mutable char name_[kLaneRowChars + 1] = {};
};

/// SAMPLE or SYNTH PRESET for an FX lane that takes a preset: the lane browser on that kind.
class KindItem final : public MenuItem {
public:
	KindItem(l10n::String name, BrowseKind kind) : MenuItem(name), kind_(kind) {}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		performView.openLaneBrowser(listPart, kindLane, kind_);
		return NO_NAVIGATION;
	}

private:
	BrowseKind kind_;
};

/// The SAMPLE / SYNTH PRESET choice, titled with its lane's name.
class KindMenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] std::string_view getTitle() const override {
		return kindLane != Lane::None ? gen::laneName(kindLane) : "";
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		// Each lane's choice starts on SAMPLE: the row it was left on was another lane's.
		Submenu::beginSession(navigatedBackwardFrom != nullptr ? navigatedBackwardFrom : items.front());
	}
};

/// The lane list, titled with its part's name. Opened, it reports its rows to the test port.
class LaneListMenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] std::string_view getTitle() const override {
		const gen::Genre* genre = currentRecipe.recipe.genre;
		return genre != nullptr && !listLanes().empty() ? genre->parts[listPart].name : "";
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
#if ENABLE_SYSEX_TEST
		if (navigatedBackwardFrom == nullptr) {
			char rows[160] = "reroll";
			size_t used = strlen(rows);
			for (const gen::LaneSpec& lane : listLanes()) {
				const bool preset = gen::laneTakesPreset(currentRecipe.palette(), lane.lane);
				used += snprintf(rows + used, sizeof(rows) - std::min(used, sizeof(rows)), ",%s%s",
				                 gen::lanePaletteName(lane.lane), preset ? "+preset" : "");
			}
			TEST_EVENT("gen_lane_list part=%ld rows=%s", static_cast<long>(listPart), rows);
		}
#endif
		// Opened, it starts on RE-ROLL ALL: the row it was left on may be another part's lane.
		Submenu::beginSession(navigatedBackwardFrom != nullptr ? navigatedBackwardFrom : items.front());
	}
};

PLACE_SDRAM_BSS KindItem sampleItem{l10n::String::STRING_FOR_SAMPLE, BrowseKind::Sample};
PLACE_SDRAM_BSS KindItem presetItem{l10n::String::STRING_FOR_SYNTH_PRESET, BrowseKind::Preset};
PLACE_SDRAM_BSS KindMenu kindMenu{l10n::String::STRING_FOR_SAMPLE, {&sampleItem, &presetItem}};

MenuItem* LaneRow::selectButtonPress() {
	const std::optional<Lane> lane = this->lane();
	if (!lane) {
		return NO_NAVIGATION;
	}
	if (gen::laneTakesPreset(currentRecipe.palette(), *lane)) {
		kindLane = *lane;
		return &kindMenu; // SAMPLE or SYNTH PRESET first (spec §13.5)
	}
	performView.openLaneBrowser(listPart, *lane, BrowseKind::Sample);
	return NO_NAVIGATION;
}

PLACE_SDRAM_BSS ReRollAllItem reRollAllItem{l10n::String::STRING_FOR_REROLL_ALL};
static_assert(gen::kMaxLanesPerPart == 11, "one LaneRow per lane slot below");
PLACE_SDRAM_BSS LaneRow laneRows[gen::kMaxLanesPerPart] = {
    LaneRow{0}, LaneRow{1}, LaneRow{2}, LaneRow{3}, LaneRow{4},  LaneRow{5},
    LaneRow{6}, LaneRow{7}, LaneRow{8}, LaneRow{9}, LaneRow{10},
};
PLACE_SDRAM_BSS LaneListMenu laneListMenuItem{
    l10n::String::STRING_FOR_SOUNDS,
    {&reRollAllItem, &laneRows[0], &laneRows[1], &laneRows[2], &laneRows[3], &laneRows[4], &laneRows[5], &laneRows[6],
     &laneRows[7], &laneRows[8], &laneRows[9], &laneRows[10]},
};

} // namespace

Submenu& laneListMenu = laneListMenuItem;

void setLaneListPart(int32_t part) {
	listPart = part;
	kindLane = Lane::None;
}

} // namespace deluge::gui::menu_item::generative
