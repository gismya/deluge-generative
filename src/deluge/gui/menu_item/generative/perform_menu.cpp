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
#include "generative/firmware/transitions.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/glide.h"
#include "generative/perform/queue_text.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace deluge::gui::menu_item::generative {

using deluge::generative::Key;
using deluge::generative::kNumScales;
using deluge::generative::Scale;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using deluge::generative::firmware::transitions;
namespace perform = deluge::generative::perform;

namespace {

/// A row's value, right-aligned after its name in a submenu, as the Compose screen's rows show theirs (spec §14.4).
constexpr int32_t kValueGap = 4; ///< pixels between the row's name and its value
constexpr int32_t kValueRightMargin = 3;
int32_t valueRenderLength(const char* value) {
	return kValueGap + static_cast<int32_t>(strlen(value)) * kTextSpacingX + kValueRightMargin;
}
void drawRowValue(const char* value, int32_t yPixel) {
	deluge::hid::display::OLED::main.drawStringAlignRight(value, yPixel, kTextSpacingX, kTextSpacingY,
	                                                      OLED_MAIN_WIDTH_PIXELS - kValueRightMargin);
}

/// What a menu's held value was read under. A stop drops what was queued, a start begins from the Recipe, and a press
/// of the same kind from anywhere (a Y turn under the menu, a BACK tap) moves what waits, so the value is read again
/// once this changes. The menu's own presses record the basis after them, so a press still in the steering queue
/// keeps the value it set.
struct Basis {
	bool playing = false;
	uint32_t pieces = UINT32_MAX; ///< never a count, so the first read always loads
	uint32_t presses = 0;         ///< the presses of the value's own kind
	bool operator==(const Basis&) const = default;
};
Basis basisNow(uint32_t presses) {
	return {performAdapter.playing(), performAdapter.pieces(), presses};
}
Basis keyBasis() {
	return basisNow(performAdapter.keyPresses());
}
Basis takeBasis(int32_t part) {
	return basisNow(performAdapter.takePresses(part));
}

// ---- KEY / SCALE ----

/// The key the editor shows, and what it was read under. Each turn changes it and queues it whole, so a turn of KEY
/// right after one of SCALE keeps the new scale even before the core has drained the first change.
Key editedKey;
Basis editedBasis;

/// Rereads the edited key from the key ahead if the basis moved since (or `always`). @return whether it did.
bool refreshEditedKey(bool always = false) {
	if (!always && editedBasis == keyBasis()) {
		return false;
	}
	editedKey = performAdapter.keyAhead();
	editedBasis = keyBasis();
	return true;
}

/// Queues `key` from the editor. Refused, the editor keeps the key it had (and the popup says why).
void queueEditedKey(Key key) {
	if (refusedWhileChangePending()) {
		return;
	}
	const bool accepted = performAdapter.queueKeyChange(key);
	if (accepted) {
		editedKey = key;
	}
	editedBasis = keyBasis();
	popupIfRefused(accepted);
}

class KeyScaleSubmenu final : public Submenu {
public:
	using Submenu::Submenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		refreshEditedKey(navigatedBackwardFrom == nullptr); // opened, rather than back from KEY or SCALE
		Submenu::beginSession(navigatedBackwardFrom);
	}
};

/// KEY's field of the edited key: the root, C to B.
struct RootField {
	static constexpr int32_t kOptions = 12;
	static int32_t get(Key key) { return key.root; }
	static Key with(Key key, int32_t value) { return {static_cast<uint8_t>(value), key.scale}; }
	static const char* name(int32_t value) { return perform::keyName(static_cast<uint8_t>(value)); }
};
/// SCALE's: the scale, in the Scale order.
struct ScaleField {
	static constexpr int32_t kOptions = kNumScales;
	static int32_t get(Key key) { return static_cast<int32_t>(key.scale); }
	static Key with(Key key, int32_t value) { return {key.root, static_cast<Scale>(value)}; }
	static const char* name(int32_t value) { return perform::scaleName(static_cast<Scale>(value)); }
};

/// KEY or SCALE: one field of the edited key. Each turn queues the edited key with the field changed.
template <typename Field>
class KeyFieldItem final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (int32_t value = 0; value < Field::kOptions; value++) {
			options.push_back(Field::name(value));
		}
		return options;
	}
	void readCurrentValue() override {
		refreshEditedKey();
		setValue(Field::get(editedKey));
	}
	void selectEncoderAction(int32_t offset) override {
		if (refreshEditedKey()) {
			setValue(Field::get(editedKey)); // the turn moves from the key ahead now
		}
		Selection::selectEncoderAction(offset);
	}
	void writeCurrentValue() override {
		queueEditedKey(Field::with(editedKey, getValue()));
		setValue(Field::get(editedKey));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	static const char* shown() {
		refreshEditedKey();
		return Field::name(Field::get(editedKey));
	}
};
using KeyItem = KeyFieldItem<RootField>;
using ScaleItem = KeyFieldItem<ScaleField>;

// ---- GLIDE ----

/// Indexed by perform::GlideLength.
constexpr l10n::String kGlideNames[] = {
    l10n::String::STRING_FOR_GLIDE_INSTANT,   l10n::String::STRING_FOR_GLIDE_BEAT,
    l10n::String::STRING_FOR_GLIDE_BAR,       l10n::String::STRING_FOR_GLIDE_TWO_BARS,
    l10n::String::STRING_FOR_GLIDE_FOUR_BARS, l10n::String::STRING_FOR_GLIDE_PHRASE,
};
static_assert(std::size(kGlideNames) == static_cast<size_t>(perform::GlideLength::Phrase) + 1);

/// GLIDE: the Perform setting tempo and swing glides take (spec §13.3), kept until power-off. Nothing else sets it.
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

/// SEED + TAKES, titled with the seed code (spec §6.5): `SEED 1A2B3C4`. Opened, its rows read their takes again.
class SeedTakesSubmenu final : public Submenu {
public:
	using Submenu::Submenu;
	void beginSession(MenuItem* navigatedBackwardFrom) override;
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
		const deluge::generative::Genre* genre = performAdapter.rosterGenre();
		return genre != nullptr && part_ < deluge::generative::partCount(*genre) ? genre->parts[part_].name : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return part_ < performAdapter.rosterParts();
	}
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::generative::kMaxTakes; }
	void readCurrentValue() override { refreshShown(); }
	void selectEncoderAction(int32_t offset) override {
		refreshShown();
		if (refusedWhileChangePending()) {
			return;
		}
		// The row as the CLIP page reads it, with the take this editor shows as the waiting one: its own press may
		// still be in the steering queue, not yet waiting in the core.
		perform::TakeRow row = performAdapter.takeRow(part_);
		row.waiting = getValue() - 1 != row.playing ? getValue() - 1 : -1;
		bool accepted = true;
		const int32_t take = performAdapter.stepTake(static_cast<uint8_t>(part_), row, offset, &accepted);
		basis_ = takeBasis(part_);
		popupIfRefused(accepted);
		if (take < 0 || !accepted) {
			return;
		}
		TEST_EVENT("gen_press take part=%ld take=%ld menu", static_cast<long>(part_), static_cast<long>(take));
		setValue(take + 1);
		Number::selectEncoderAction(offset);
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shownText()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shownText(), yPixel); }
	/// Forgets what the row read, so it reads its take again: SEED + TAKES opened afresh.
	void forget() { basis_ = Basis{}; }

private:
	/// Reads the take again, the waiting one or else the one playing, if the basis moved since this row last read or
	/// pressed it.
	void refreshShown() {
		if (basis_ != takeBasis(part_)) {
			const perform::TakeRow row = performAdapter.takeRow(part_);
			setValue((row.waiting >= 0 ? row.waiting : row.playing) + 1);
			basis_ = takeBasis(part_);
		}
	}
	const char* shownText() {
		refreshShown();
		snprintf(text_, sizeof(text_), "%ld", static_cast<long>(getValue()));
		return text_;
	}
	int32_t part_;
	Basis basis_;
	char text_[4] = {};
};

// ---- CANCEL CHANGE, GENRE, REGENERATE (spec §13.6) ----

/// Opens the transition confirm over the menu (spec §13.6): BUILD INTO A DROP, FADE THROUGH or HARD CUT AT PHRASE.
MenuItem* confirmTransition() {
	if (context_menu::transitionConfirm.setupAndCheckAvailability()) {
		openUI(&context_menu::transitionConfirm);
	}
	return NO_NAVIGATION;
}

/// CANCEL CHANGE: shown only while a transition can still be cancelled (Preloading, or Queued before its line).
class CancelChangeItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return transitions.cancellable();
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		const bool cancelled = transitions.cancel();
		TEST_EVENT("gen_menu_cancel_change %s", cancelled ? "cancelled" : "too_late");
		soundEditor.exitCompletely();
		return NO_NAVIGATION;
	}
};

/// REGENERATE: a genre change to the genre playing (spec §9.7), through the confirm.
class RegenerateItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override { return confirmTransition(); }
};

/// A row of GENRE ▸, for allGenres()[index]. The genre playing is labelled `= REGEN`, and picking it regenerates.
/// M1 has no genre change, so the other genres' rows hide themselves (M2).
class TransitionGenreItem final : public MenuItem {
public:
	explicit TransitionGenreItem(int32_t index) : index_(index) {}
	[[nodiscard]] std::string_view getName() const override {
		const auto* g = genre();
		return g != nullptr ? g->oledName : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return genre() != nullptr && genre() == currentRecipe.recipe.genre;
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override { return confirmTransition(); }
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(regen()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(regen(), yPixel); }

private:
	[[nodiscard]] const deluge::generative::Genre* genre() const {
		auto genres = deluge::generative::allGenres();
		return index_ < static_cast<int32_t>(genres.size()) ? genres[index_] : nullptr;
	}
	static const char* regen() { return l10n::get(l10n::String::STRING_FOR_REGEN_MARK); }
	int32_t index_;
};

PLACE_SDRAM_BSS CancelChangeItem cancelChangeItem{l10n::String::STRING_FOR_CANCEL_CHANGE};
PLACE_SDRAM_BSS RegenerateItem regenerateItem{l10n::String::STRING_FOR_REGENERATE};
static_assert(deluge::generative::kMaxGenres == 5, "one TransitionGenreItem per genre slot below");
PLACE_SDRAM_BSS TransitionGenreItem transitionGenreItems[deluge::generative::kMaxGenres] = {
    TransitionGenreItem{0}, TransitionGenreItem{1}, TransitionGenreItem{2}, TransitionGenreItem{3},
    TransitionGenreItem{4}};
PLACE_SDRAM_BSS Submenu transitionGenreMenu{
    l10n::String::STRING_FOR_GENRE,
    {&transitionGenreItems[0], &transitionGenreItems[1], &transitionGenreItems[2], &transitionGenreItems[3],
     &transitionGenreItems[4]},
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

void SeedTakesSubmenu::beginSession(MenuItem* navigatedBackwardFrom) {
	if (navigatedBackwardFrom == nullptr) {
		// Opened, rather than back from a row: what the rows read last time may be another piece's or Recipe's.
		for (TakeItem& take : takeItems) {
			take.forget();
		}
	}
	Submenu::beginSession(navigatedBackwardFrom);
}

PLACE_SDRAM_BSS KeyScaleSubmenu keyScaleMenuItem{l10n::String::STRING_FOR_KEY_SCALE, {&keyItem, &scaleItem}};

} // namespace

Submenu& keyScaleMenu = keyScaleMenuItem;

PLACE_SDRAM_BSS Submenu performMenu{
    l10n::String::STRING_FOR_PERFORM,
    {&cancelChangeItem, &transitionGenreMenu, &regenerateItem, &keyScaleMenuItem, &glideItem, &seedTakesMenu},
};

void refreshPerformMenu() {
	// Whether anything a Perform menu value reads under moved since the last frame.
	uint32_t presses = performAdapter.keyPresses();
	for (int32_t part = 0; part < deluge::generative::kMaxParts; part++) {
		presses += performAdapter.takePresses(part);
	}
	// CANCEL CHANGE shows only while a transition can be cancelled: its phase moves it too.
	presses += static_cast<uint32_t>(transitions.phase()) << 24;
	presses += transitions.cancellable() ? 1U << 28 : 0;
	static Basis shownBasis;
	if (shownBasis == basisNow(presses)) {
		return;
	}
	shownBasis = basisNow(presses);
	MenuItem* item = soundEditor.getCurrentMenuItem();
	bool ours = item == &performMenu || item == &transitionGenreMenu || item == &keyScaleMenuItem || item == &keyItem
	            || item == &scaleItem || item == &seedTakesMenu;
	for (TakeItem& take : takeItems) {
		ours = ours || item == &take;
	}
	if (ours) {
		item->readValueAgain();
	}
}

} // namespace deluge::gui::menu_item::generative
