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

#include "gui/menu_item/generative/compose_screen.h"
#include "definitions.h"
#include "generative/compose/compose_screen.h"
#include "generative/compose/song_size.h"
#include "generative/core/genre.h"
#include "generative/core/lane_names.h"
#include "generative/core/pitch.h"
#include "generative/firmware/compose_edits.h"
#include "generative/firmware/compose_job.h"
#include "generative/firmware/current_recipe.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/queue_text.h"
#include "gui/context_menu/generative.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/genre_rows.h"
#include "gui/menu_item/generative/row_value.h"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include "storage/flash_storage.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

namespace deluge::gui::menu_item::generative {

namespace gen = deluge::generative;
namespace compose = deluge::generative::compose;
using gen::JobOutcome;
using gen::Lane;
using gen::Macro;
using gen::Scale;
using gen::firmware::currentRecipe;

namespace {

gen::Recipe& recipe() {
	return currentRecipe.recipe;
}

/// The Current Recipe's parts: 0 before one exists.
int32_t parts() {
	return currentRecipe.exists && recipe().genre != nullptr ? gen::partCount(*recipe().genre) : 0;
}

/// Test builds: the Current Recipe as the screen edits it, after each edit (`why`) and when the screen shows
/// (opened, or shown again when a context menu or the working modal over it closes): macros in points.
void reportRecipe(const char* why) {
#if ENABLE_SYSEX_TEST
	if (!currentRecipe.exists) {
		TEST_EVENT("gen_compose_recipe %s none", why);
		return;
	}
	const gen::Recipe& r = recipe();
	char code[gen::perform::kSeedCodeChars + 1];
	gen::perform::seedCode(currentRecipe.seed, code);
	const gen::Takes& t = currentRecipe.takes;
	// Two lines: one would pass the test port's event length.
	TEST_EVENT("gen_compose_recipe %s genre=%s tempo=%ld swing=%d key=%d scale=%d macros=%ld,%ld,%ld,%ld", why,
	           r.genre->name, static_cast<long>(r.tempo), r.swing, r.keyRoot, static_cast<int>(r.scale),
	           static_cast<long>(gen::macroToPoints(r.macros.value[0])),
	           static_cast<long>(gen::macroToPoints(r.macros.value[1])),
	           static_cast<long>(gen::macroToPoints(r.macros.value[2])),
	           static_cast<long>(gen::macroToPoints(r.macros.value[3])));
	TEST_EVENT("gen_compose_seed %s seed=%s takes=%d,%d,%d,%d,%d,%d length=%ld", why, code, t.take[0], t.take[1],
	           t.take[2], t.take[3], t.take[4], t.take[5], static_cast<long>(gen::firmware::composeLength()));
#endif
}

/// Test builds: a sound row's pick, after it changed.
void reportPick(const char* row, const gen::SoundPath& path) {
#if ENABLE_SYSEX_TEST
	TEST_EVENT("gen_compose_pick %s path=%s", row, path.empty() ? "-" : path.c_str());
#endif
}

/// The value rows' room: what's left of a row after its name (`name`), in characters.
int32_t charsAfter(std::string_view name) {
	const int32_t used =
	    kTextSpacingX + static_cast<int32_t>(name.size()) * kTextSpacingX + kValueGap + kValueRightMargin;
	return (OLED_MAIN_WIDTH_PIXELS - used) / kTextSpacingX;
}

// ---- The screen ----

class ComposeScreen final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] std::string_view getTitle() const override {
		if (!currentRecipe.exists) {
			return l10n::getView(l10n::String::STRING_FOR_COMPOSE);
		}
		compose::composeTitle(currentRecipe.recipe, title_);
		return title_;
	}
	/// openComposeScreen() opens a session. gen_compose_screen opened comes once per session: the sound editor also
	/// begins the screen again, with no item it came back from, when it regains focus (a context menu or the working
	/// modal over it closing), which reports gen_compose_recipe shown instead.
	void openSession() { sessionOpen_ = false; }
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		if (navigatedBackwardFrom == nullptr) {
			if (!sessionOpen_) {
				sessionOpen_ = true;
				reportRecipe("opened");
				TEST_EVENT("gen_compose_screen opened");
			}
			else {
				reportRecipe("shown");
			}
		}
		Submenu::beginSession(navigatedBackwardFrom);
	}
	/// As MenuItem::renderOLED(), with the title side-scrolling when it's wider than the screen, as a long song name
	/// does in the stock session view.
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
	mutable char title_[compose::kTitleChars + 1] = {};
	bool sessionOpen_ = false;
};

/// A row with no value and no arrow: REGENERATE, NEW SEED, BUILD.
class ActionRow : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	int32_t getSubmenuItemTypeRenderLength() override { return 0; }
	void renderSubmenuItemTypeForOled(int32_t /*yPixel*/) override {}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return currentRecipe.exists;
	}
};

/// Back to the Compose screen from a submenu under it, once a roll there has ended.
void backToScreen() {
	while (isUIOpen(&soundEditor) && soundEditor.getCurrentMenuItem() != &composeScreen
	       && soundEditor.navigationDepth > 0) {
		soundEditor.goUpOneLevel();
	}
	if (isUIOpen(&soundEditor)) {
		renderUIsForOled();
	}
}

/// A roll's end: the screen shows the Recipe as it is now.
void rollEnded(JobOutcome outcome) {
	if (outcome == JobOutcome::Done) {
		reportRecipe("rolled");
	}
	if (isUIOpen(&soundEditor)) {
		renderUIsForOled();
	}
}

void refusedBusy() {
	display->displayPopup(l10n::get(l10n::String::STRING_FOR_GENERATIVE_BUSY));
}

// ---- FROM ▸ ----

/// A genre of FROM's GENRE ▸: picking it rolls a fresh Current Recipe (spec §14.1), and the screen shows it.
class ComposeGenreItem final : public MenuItem {
public:
	explicit ComposeGenreItem(GenreSlot slot) : slot_(slot) {}
	[[nodiscard]] std::string_view getName() const override { return genreLabel(slot_); }
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return genreAt(slot_) != nullptr;
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		const auto ended = [](JobOutcome outcome) {
			if (outcome == JobOutcome::Done) {
				backToScreen();
			}
			rollEnded(outcome);
		};
		const gen::Genre* genre = genreAt(slot_);
		if (genre == nullptr || !gen::firmware::beginGenreRoll(*genre, ended)) {
			refusedBusy();
		}
		return NO_NAVIGATION;
	}

private:
	GenreSlot slot_;
};

PLACE_SDRAM_BSS GenreMenuItems<ComposeGenreItem> composeGenres{l10n::String::STRING_FOR_GENRE};

/// FROM ▸: the sources a Recipe comes from (spec §14.1). Its value is the Recipe's genre.
class FromMenu final : public Submenu {
public:
	using Submenu::Submenu;
	int32_t getSubmenuItemTypeRenderLength() override { return valueAndArrowRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValueAndArrow(*this, shown(), yPixel); }

private:
	static const char* shown() { return currentRecipe.exists ? recipe().genre->oledName : ""; }
};

// ---- REGENERATE ----

class RegenerateRow final : public ActionRow {
public:
	using ActionRow::ActionRow;
	MenuItem* selectButtonPress() override {
		if (context_menu::regenerateRecipe.setupAndCheckAvailability()) {
			openUI(&context_menu::regenerateRecipe);
		}
		return NO_NAVIGATION;
	}
};

// ---- SEED + TAKES ▸ ----

/// SEED + TAKES, titled with the seed code (spec §6.5): `SEED 0Y8438B`.
class SeedTakesMenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return currentRecipe.exists;
	}
	[[nodiscard]] std::string_view getTitle() const override {
		char code[gen::perform::kSeedCodeChars + 1];
		gen::perform::seedCode(currentRecipe.seed, code);
		snprintf(title_, sizeof(title_), "%s %s", l10n::get(l10n::String::STRING_FOR_SEED), code);
		return title_;
	}

private:
	mutable char title_[24] = {};
};

/// NEW SEED: a fresh seed, takes at 0 (spec §6.5).
class NewSeedRow final : public ActionRow {
public:
	using ActionRow::ActionRow;
	MenuItem* selectButtonPress() override {
		gen::firmware::newSeed();
		reportRecipe("seed");
		renderUIsForOled(); // the title's seed code
		return NO_NAVIGATION;
	}
};

/// A part's take, 1–16 as shown (spec §6.6): the stock number editor.
class TakeRow final : public Integer {
public:
	explicit TakeRow(int32_t part) : Integer(l10n::String::STRING_FOR_SEED_AND_TAKES), part_(part) {}
	[[nodiscard]] std::string_view getName() const override {
		return part_ < parts() ? recipe().genre->parts[part_].name : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return part_ < parts();
	}
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return gen::kMaxTakes; }
	void readCurrentValue() override { setValue(currentRecipe.takes.take[part_] + 1); }
	void writeCurrentValue() override {
		currentRecipe.takes.take[part_] = static_cast<uint8_t>(getValue() - 1);
		reportRecipe("take");
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	const char* shown() {
		snprintf(text_, sizeof(text_), "%d", currentRecipe.takes.take[part_] + 1);
		return text_;
	}
	int32_t part_;
	char text_[4] = {};
};

// ---- TEMPO, SWING, LENGTH: the stock big-number editor; the macros: the continuous one ----

/// A number row that reads and writes the Current Recipe, its value shown on the row.
template <typename Base>
class RecipeNumber : public Base {
public:
	using Base::Base;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return currentRecipe.exists;
	}
	void readCurrentValue() override { this->setValue(read()); }
	void writeCurrentValue() override {
		write(this->getValue());
		reportRecipe(event());
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

protected:
	[[nodiscard]] virtual int32_t read() const = 0;
	virtual void write(int32_t value) = 0;
	[[nodiscard]] virtual const char* event() const = 0;
	/// The number shown for `value`, on the row and in the editor.
	[[nodiscard]] virtual int32_t displayed(int32_t value) const { return value; }
	int32_t getDisplayValue() override { return displayed(this->getValue()); }
	/// The row's value text, from read().
	virtual const char* shown() {
		snprintf(text_, sizeof(text_), "%ld", static_cast<long>(displayed(read())));
		return text_;
	}
	char text_[8] = {};
};

/// TEMPO in whole BPM.
class TempoRow final : public RecipeNumber<Integer> {
public:
	using RecipeNumber::RecipeNumber;
	[[nodiscard]] int32_t getMinValue() const override { return gen::kMinTempo / 100; }
	[[nodiscard]] int32_t getMaxValue() const override { return gen::kMaxTempo / 100; }

protected:
	[[nodiscard]] int32_t read() const override { return compose::tempoBpm(recipe().tempo); }
	void write(int32_t value) override { recipe().tempo = value * 100; }
	[[nodiscard]] const char* event() const override { return "tempo"; }
};

/// SWING: the Recipe's amount, shown as stock's swing popup shows it, 50 for straight (1–99).
class SwingRow final : public RecipeNumber<Integer> {
public:
	using RecipeNumber::RecipeNumber;
	[[nodiscard]] int32_t getMinValue() const override { return -gen::kMaxSwing; }
	[[nodiscard]] int32_t getMaxValue() const override { return gen::kMaxSwing; }

protected:
	[[nodiscard]] int32_t read() const override { return recipe().swing; }
	void write(int32_t value) override { recipe().swing = static_cast<int8_t>(value); }
	[[nodiscard]] const char* event() const override { return "swing"; }
	[[nodiscard]] int32_t displayed(int32_t value) const override { return value + 50; }
};

/// LENGTH (spec §14.4): the target length, 1:00–20:00 in 30 s steps, in the big-number editor as m:ss. The value is
/// the length in steps.
class LengthRow final : public RecipeNumber<Integer> {
public:
	using RecipeNumber::RecipeNumber;
	[[nodiscard]] int32_t getMinValue() const override {
		return compose::kMinLengthSeconds / compose::kLengthStepSeconds;
	}
	[[nodiscard]] int32_t getMaxValue() const override {
		return compose::kMaxLengthSeconds / compose::kLengthStepSeconds;
	}
	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override {
		char text[compose::kLengthChars + 1];
		compose::lengthText(getValue() * compose::kLengthStepSeconds, text);
		hid::display::OLED::main.drawStringCentred(text, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth, textHeight);
	}

protected:
	[[nodiscard]] int32_t read() const override { return gen::firmware::composeLength() / compose::kLengthStepSeconds; }
	void write(int32_t value) override { gen::firmware::setComposeLength(value * compose::kLengthStepSeconds); }
	[[nodiscard]] const char* event() const override { return "length"; }
	const char* shown() override {
		compose::lengthText(read() * compose::kLengthStepSeconds, text_);
		return text_;
	}
};

/// A macro, 0–50 (spec §3.3): the stock continuous editor, number and bar. A turn sets the macro to the points shown.
class MacroRow final : public RecipeNumber<IntegerContinuous> {
public:
	MacroRow(l10n::String name, Macro macro) : RecipeNumber(name), macro_(macro) {}
	[[nodiscard]] int32_t getMaxValue() const override { return gen::kMacroPoints; }

protected:
	[[nodiscard]] int32_t read() const override { return gen::macroToPoints(recipe().macros[macro_]); }
	void write(int32_t value) override { recipe().macros.set(macro_, gen::macroPoints(value)); }
	[[nodiscard]] const char* event() const override { return "macro"; }

private:
	Macro macro_;
};

// ---- KEY, SCALE: edited in place ----

/// How long the edited value shows, then hides, while it blinks (ms).
constexpr int32_t kBlinkMs = 300;

/// A row edited in place (spec §14.4): pushed, it becomes the sound editor's item but draws the Compose screen with
/// its own value blinking. SELECT turns it, and push or BACK finishes (back to the screen, on this row).
///
/// A blink redraws only the value's box: a whole render clears the image, which restarts the title's and the row's
/// side-scrollers, and at every 300 ms blink they would never move.
template <typename Field>
class InPlaceRow final : public MenuItem {
public:
	using MenuItem::MenuItem;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return currentRecipe.exists;
	}
	void beginSession(MenuItem* /*navigatedBackwardFrom*/) override {
		display->setNextTransitionDirection(0); // in place: no slide
		hidden_ = false;
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kBlinkMs);
	}
	void endSession() override {
		uiTimerManager.unsetTimer(TimerName::UI_SPECIFIC);
		hidden_ = false;
		MenuItem::endSession();
	}
	ActionResult timerCallback() override {
		hidden_ = !hidden_;
		if (drawnY_ >= 0) {
			drawValueBox();
		}
		else {
			renderUIsForOled();
		}
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kBlinkMs);
		return ActionResult::DEALT_WITH;
	}
	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			Buttons::selectButtonPressUsedUp = true; // a turn while pushed isn't a push, as in stock's Value
		}
		const int32_t value = std::clamp<int32_t>(Field::get(recipe()) + offset, 0, Field::kOptions - 1);
		Field::set(recipe(), value);
		reportRecipe(Field::kEvent);
		hidden_ = false; // the new value shows at once
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kBlinkMs);
		renderUIsForOled();
	}
	MenuItem* selectButtonPress() override { return nullptr; } // push finishes: back to the screen
	void renderOLED() override {
		drawnY_ = -1;
		composeScreen.renderOLED();
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		const bool editing = soundEditor.getCurrentMenuItem() == this;
		if (editing) {
			drawnY_ = yPixel; // the selected row: Submenu inverts it after this
		}
		if (!(hidden_ && editing)) {
			drawRowValue(shown(), yPixel);
		}
	}

private:
	const char* shown() const { return Field::name(Field::get(recipe()), charsAfter(getName())); }

	/// The value's box on the row the last render drew, shown or hidden, inverted as the selected row is.
	void drawValueBox() {
		hid::display::oled_canvas::Canvas& canvas = hid::display::OLED::main;
		const int32_t minX = OLED_MAIN_WIDTH_PIXELS - valueRenderLength(shown()) + kValueGap;
		const int32_t maxX = OLED_MAIN_WIDTH_PIXELS - kValueRightMargin - 1;
		const int32_t maxY = drawnY_ + kTextSpacingY - 1; // the row's highlight: Submenu's yPixel to yPixel + 8
		canvas.clearAreaExact(minX, drawnY_, maxX, maxY);
		if (!hidden_) {
			drawRowValue(shown(), drawnY_);
		}
		if (FlashStorage::accessibilityMenuHighlighting != MenuHighlighting::NO_INVERSION) {
			canvas.invertArea(minX, maxX - minX + 1, drawnY_, maxY);
		}
		hid::display::OLED::markChanged();
	}

	bool hidden_ = false;
	int32_t drawnY_ = -1; ///< the row's y in the last render, -1 before one
};

/// KEY: the root, C to B.
struct KeyField {
	static constexpr int32_t kOptions = 12;
	static constexpr const char* kEvent = "key";
	static int32_t get(const gen::Recipe& r) { return r.keyRoot; }
	static void set(gen::Recipe& r, int32_t value) { r.keyRoot = static_cast<uint8_t>(value); }
	static const char* name(int32_t value, int32_t /*fitChars*/) {
		return gen::perform::keyName(static_cast<uint8_t>(value));
	}
};
/// SCALE: in the Scale order, the short name when the full one doesn't fit.
struct ScaleField {
	static constexpr int32_t kOptions = gen::kNumScales;
	static constexpr const char* kEvent = "scale";
	static int32_t get(const gen::Recipe& r) { return static_cast<int32_t>(r.scale); }
	static void set(gen::Recipe& r, int32_t value) { r.scale = static_cast<Scale>(value); }
	static const char* name(int32_t value, int32_t fitChars) {
		return compose::scaleRowName(static_cast<Scale>(value), fitChars);
	}
};

// ---- SOUNDS ▸ ----

/// The longest SOUNDS row: a part or lane name, a space and the pick's name.
constexpr int32_t kSoundRowChars = 12 + 1 + compose::kPickChars;

/// The Kit part whose lane list is open.
int32_t openKitPart = 0;

/// A SOUNDS row's target: a synth part's preset (`part` >= 0), or one lane's sample.
struct SoundTarget {
	int32_t part = -1;
	Lane lane = Lane::Kick;

	[[nodiscard]] const char* name() const {
		return part >= 0 ? recipe().genre->parts[part].name : gen::laneName(lane);
	}
	[[nodiscard]] const gen::SoundPath& pick() const {
		return part >= 0 ? currentRecipe.sounds.part[part] : currentRecipe.sounds.lane[static_cast<int32_t>(lane)];
	}
};

/// The row a re-roll started from, for its end (one roll runs at a time).
SoundTarget rolling;

/// A SOUNDS row, `BASS 017 ACID LINE` or `KICK 909 Kick` (spec §14.4): push re-rolls its pick from the palette,
/// SHIFT+push goes back to the previous pick. The pick is part of the row's name, with no right-aligned value, so a
/// long row is cut off and side-scrolls while selected, as a stock row does.
class SoundRow : public MenuItem {
public:
	[[nodiscard]] std::string_view getName() const override {
		const std::optional<SoundTarget> target = this->target();
		if (!target) {
			return "";
		}
		char pick[compose::kPickChars + 1];
		compose::pickName(target->pick(), pick);
		snprintf(name_, sizeof(name_), "%s %s", target->name(), pick);
		return name_;
	}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return target().has_value();
	}
	bool shouldEnterSubmenu() override { return false; }
	int32_t getSubmenuItemTypeRenderLength() override { return 0; }
	void renderSubmenuItemTypeForOled(int32_t /*yPixel*/) override {}
	MenuItem* selectButtonPress() override {
		const std::optional<SoundTarget> target = this->target();
		if (!target) {
			return NO_NAVIGATION;
		}
		if (Buttons::isShiftButtonPressed()) {
			const bool back = target->part >= 0 ? gen::firmware::previousPartPick(target->part)
			                                    : gen::firmware::previousLanePick(target->lane);
			if (back) {
				reportPick(target->name(), target->pick());
				renderUIsForOled();
			}
			else {
				display->displayPopup(l10n::get(l10n::String::STRING_FOR_NO_PREVIOUS_SOUND));
				TEST_EVENT("gen_compose_pick %s none", target->name());
			}
			return NO_NAVIGATION;
		}
		rolling = *target;
		const auto ended = [](JobOutcome outcome) {
			if (outcome == JobOutcome::Done) {
				reportPick(rolling.name(), rolling.pick());
			}
			rollEnded(outcome);
		};
		const bool started = target->part >= 0 ? gen::firmware::beginPartRoll(target->part, ended)
		                                       : gen::firmware::beginLaneRoll(target->lane, ended);
		if (!started) {
			refusedBusy();
		}
		return NO_NAVIGATION;
	}

protected:
	/// What the row re-rolls, or none: the row isn't relevant.
	[[nodiscard]] virtual std::optional<SoundTarget> target() const = 0;

private:
	mutable char name_[kSoundRowChars + 1] = {};
};

/// A synth part's row.
class SynthSoundRow final : public SoundRow {
public:
	explicit SynthSoundRow(int32_t part) : part_(part) {}

protected:
	[[nodiscard]] std::optional<SoundTarget> target() const override {
		if (part_ >= parts() || !recipe().genre->parts[part_].synth()) {
			return std::nullopt;
		}
		return SoundTarget{.part = part_};
	}

private:
	int32_t part_;
};

/// A lane of the open Kit part: as a synth part's row, one sample per lane.
class LaneSoundRow final : public SoundRow {
public:
	explicit LaneSoundRow(int32_t index) : index_(index) {}

protected:
	/// The lane this row shows, or none: past the open part's lanes.
	[[nodiscard]] std::optional<SoundTarget> target() const override {
		if (openKitPart >= parts()) {
			return std::nullopt;
		}
		const auto lanes = recipe().genre->parts[openKitPart].lanes;
		if (index_ >= static_cast<int32_t>(lanes.size())) {
			return std::nullopt;
		}
		return SoundTarget{.lane = lanes[index_].lane};
	}

private:
	int32_t index_;
};

static_assert(gen::kMaxLanesPerPart == 11, "one LaneSoundRow per lane slot below");
PLACE_SDRAM_BSS LaneSoundRow laneRows[gen::kMaxLanesPerPart] = {
    LaneSoundRow{0}, LaneSoundRow{1}, LaneSoundRow{2}, LaneSoundRow{3}, LaneSoundRow{4},  LaneSoundRow{5},
    LaneSoundRow{6}, LaneSoundRow{7}, LaneSoundRow{8}, LaneSoundRow{9}, LaneSoundRow{10},
};

/// A Drums or FX part's row, `DRUMS: 8 LANES` ▸: its lane list, titled with the part's name.
class KitSoundsMenu final : public Submenu {
public:
	KitSoundsMenu(int32_t part, std::span<MenuItem*> lanes)
	    : Submenu(l10n::String::STRING_FOR_SOUNDS, lanes), part_(part) {}
	[[nodiscard]] std::string_view getName() const override {
		if (part_ >= parts()) {
			return "";
		}
		compose::kitRowName(recipe().genre->parts[part_], name_);
		return name_;
	}
	[[nodiscard]] std::string_view getTitle() const override {
		return part_ < parts() ? recipe().genre->parts[part_].name : "";
	}
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return part_ < parts() && !recipe().genre->parts[part_].synth();
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override {
		openKitPart = part_;
		Submenu::beginSession(navigatedBackwardFrom);
	}

private:
	int32_t part_;
	mutable char name_[compose::kRowChars + 1] = {};
};

/// SOUNDS ▸, `n PARTS`: per part slot a synth row then a Kit row, of which only one is relevant.
class SoundsMenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return currentRecipe.exists;
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueAndArrowRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValueAndArrow(*this, shown(), yPixel); }

private:
	const char* shown() {
		compose::partsText(parts(), text_);
		return text_;
	}
	char text_[compose::kRowChars + 1] = {};
};

// ---- BUILD ----

/// BUILD (spec §14.5): refused with the longest LENGTH that fits if even an empty Deluge can't hold the song, else
/// DISCARD SONG? → OK builds it.
class BuildRow final : public ActionRow {
public:
	using ActionRow::ActionRow;
	MenuItem* selectButtonPress() override {
		const int32_t longest = gen::firmware::composeTooLong(recipe(), gen::firmware::composeLength());
		if (longest >= 0) {
			gen::firmware::showComposeTooLong(longest);
			TEST_EVENT("gen_compose_build too_long max_s=%ld", static_cast<long>(longest));
			return NO_NAVIGATION;
		}
		context_menu::discardSong.setCompose();
		if (context_menu::discardSong.setupAndCheckAvailability()) {
			openUI(&context_menu::discardSong);
		}
		return NO_NAVIGATION;
	}
};

// ---- The items ----

PLACE_SDRAM_BSS FromMenu fromMenu{l10n::String::STRING_FOR_FROM, {&composeGenres.menu()}};
PLACE_SDRAM_BSS RegenerateRow regenerateRow{l10n::String::STRING_FOR_REGENERATE};

PLACE_SDRAM_BSS NewSeedRow newSeedRow{l10n::String::STRING_FOR_NEW_SEED};
static_assert(gen::kMaxParts == 6, "one TakeRow and one pair of SOUNDS rows per part slot below");
PLACE_SDRAM_BSS TakeRow takeRows[gen::kMaxParts] = {TakeRow{0}, TakeRow{1}, TakeRow{2},
                                                    TakeRow{3}, TakeRow{4}, TakeRow{5}};
PLACE_SDRAM_BSS SeedTakesMenu seedTakesMenu{
    l10n::String::STRING_FOR_SEED_AND_TAKES,
    {&newSeedRow, &takeRows[0], &takeRows[1], &takeRows[2], &takeRows[3], &takeRows[4], &takeRows[5]},
};

PLACE_SDRAM_BSS TempoRow tempoRow{l10n::String::STRING_FOR_TEMPO};
PLACE_SDRAM_BSS SwingRow swingRow{l10n::String::STRING_FOR_SWING};
PLACE_SDRAM_BSS InPlaceRow<KeyField> keyRow{l10n::String::STRING_FOR_KEY};
PLACE_SDRAM_BSS InPlaceRow<ScaleField> scaleRow{l10n::String::STRING_FOR_SCALE};
PLACE_SDRAM_BSS MacroRow energyRow{l10n::String::STRING_FOR_ENERGY, Macro::Energy};
PLACE_SDRAM_BSS MacroRow densityRow{l10n::String::STRING_FOR_DENSITY, Macro::Density};
PLACE_SDRAM_BSS MacroRow complexityRow{l10n::String::STRING_FOR_COMPLEXITY, Macro::Complexity};
PLACE_SDRAM_BSS MacroRow motionRow{l10n::String::STRING_FOR_MOTION, Macro::Motion};

MenuItem* laneList[gen::kMaxLanesPerPart] = {&laneRows[0], &laneRows[1], &laneRows[2], &laneRows[3],
                                             &laneRows[4], &laneRows[5], &laneRows[6], &laneRows[7],
                                             &laneRows[8], &laneRows[9], &laneRows[10]};
PLACE_SDRAM_BSS SynthSoundRow synthRows[gen::kMaxParts] = {SynthSoundRow{0}, SynthSoundRow{1}, SynthSoundRow{2},
                                                           SynthSoundRow{3}, SynthSoundRow{4}, SynthSoundRow{5}};
PLACE_SDRAM_BSS KitSoundsMenu kitMenus[gen::kMaxParts] = {
    KitSoundsMenu{0, laneList}, KitSoundsMenu{1, laneList}, KitSoundsMenu{2, laneList},
    KitSoundsMenu{3, laneList}, KitSoundsMenu{4, laneList}, KitSoundsMenu{5, laneList},
};
PLACE_SDRAM_BSS SoundsMenu soundsMenu{
    l10n::String::STRING_FOR_SOUNDS,
    {&synthRows[0], &kitMenus[0], &synthRows[1], &kitMenus[1], &synthRows[2], &kitMenus[2], &synthRows[3], &kitMenus[3],
     &synthRows[4], &kitMenus[4], &synthRows[5], &kitMenus[5]},
};

PLACE_SDRAM_BSS LengthRow lengthRow{l10n::String::STRING_FOR_LENGTH};
PLACE_SDRAM_BSS BuildRow buildRow{l10n::String::STRING_FOR_BUILD};

PLACE_SDRAM_BSS ComposeScreen composeScreenItem{
    l10n::String::STRING_FOR_COMPOSE,
    {&fromMenu, &regenerateRow, &seedTakesMenu, &tempoRow, &swingRow, &keyRow, &scaleRow, &energyRow, &densityRow,
     &complexityRow, &motionRow, &soundsMenu, &lengthRow, &buildRow},
};

} // namespace

Submenu& composeScreen = composeScreenItem;

bool openComposeScreen() {
	if (rootUIIsGenerativePerform()) {
		return false;
	}
	composeScreenItem.openSession();
	if (!soundEditor.setup(nullptr, &composeScreen)) {
		return false;
	}
	return openUI(&soundEditor);
}

bool composeScreenShowing() {
	return getCurrentUI() == &soundEditor && soundEditor.menuItemNavigationRecord[0] == &composeScreen;
}

void buildComposeRecipe() {
	using gen::firmware::ComposeStart;
	const ComposeStart start = gen::firmware::beginCompose(nullptr, gen::firmware::composeLength(), nullptr);
	switch (start) {
	case ComposeStart::Started:
		return;
	case ComposeStart::RunnerBusy:
		refusedBusy();
		TEST_EVENT("gen_compose_build refused why=busy");
		return;
	default:
		// Can't happen: the screen doesn't open under Perform, and BUILD hides while no Recipe exists.
		display->displayError(Error::UNSPECIFIED);
		if (start == ComposeStart::PerformActive) {
			TEST_EVENT("gen_compose_build refused why=perform");
		}
		else {
			TEST_EVENT("gen_compose_build refused why=no_recipe");
		}
		return;
	}
}

void regenerateComposeRecipe() {
	if (!gen::firmware::beginRegenerate(rollEnded)) {
		refusedBusy();
	}
}

} // namespace deluge::gui::menu_item::generative
