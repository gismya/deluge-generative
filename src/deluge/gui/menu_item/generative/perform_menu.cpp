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
#include "generative/firmware/restart_began.h"
#include "generative/firmware/transitions.h"
#include "generative/perform/capture.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/glide.h"
#include "generative/perform/queue_text.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/generative/genre_rows.h"
#include "gui/menu_item/generative/regenerate_page.h"
#include "gui/menu_item/generative/row_value.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/browser/perform_sound_browser.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/views/perform_view.h"
#include "hid/display/oled.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace deluge::gui::menu_item::generative {

using deluge::generative::Key;
using deluge::generative::kNumScales;
using deluge::generative::Scale;
using deluge::generative::SteeringLog;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::performAdapter;
using deluge::generative::firmware::restartBegan;
using deluge::generative::firmware::transitions;
namespace perform = deluge::generative::perform;
using perform::PressOutcome;

namespace {

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
	return {performAdapter.playing(), performAdapter.steering().pieces(), presses};
}
Basis keyBasis() {
	return basisNow(performAdapter.steering().keyPresses());
}
Basis takeBasis(int32_t part) {
	return basisNow(performAdapter.steering().takePresses(part));
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
	editedKey = performAdapter.steering().keyAhead();
	editedBasis = keyBasis();
	return true;
}

/// Queues `key` from the editor. Refused, the editor keeps the key it had (and the popup says why).
void queueEditedKey(Key key) {
	const PressOutcome outcome = performAdapter.steering().queueKeyChange(key);
	refusedWithPopup(outcome);
	if (outcome == PressOutcome::ChangePending) {
		return;
	}
	if (outcome == PressOutcome::Accepted) {
		editedKey = key;
	}
	editedBasis = keyBasis();
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
	void readCurrentValue() override { setValue(performAdapter.steering().glide()); }
	void writeCurrentValue() override {
		performAdapter.steering().setGlide(getValue<perform::GlideLength>());
		TEST_EVENT("gen_glide %d", static_cast<int>(performAdapter.steering().glide()));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	static const char* shown() {
		return l10n::get(kGlideNames[static_cast<int32_t>(performAdapter.steering().glide())]);
	}
};

// ---- BROWSER PREVIEW ----

/// Indexed by perform::BrowserPreview.
constexpr l10n::String kPreviewNames[] = {
    l10n::String::STRING_FOR_OFF,
    l10n::String::STRING_FOR_WHEN_STOPPED,
    l10n::String::STRING_FOR_ALWAYS,
};
static_assert(std::size(kPreviewNames) == static_cast<size_t>(perform::BrowserPreview::Always) + 1);

/// BROWSER PREVIEW: whether the preset and lane browsers play the file under the cursor (spec §13.3), kept until
/// power-off.
class BrowserPreviewItem final : public Selection {
public:
	using Selection::Selection;
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		for (l10n::String name : kPreviewNames) {
			options.push_back(l10n::getView(name));
		}
		return options;
	}
	void readCurrentValue() override { setValue(PerformSoundBrowser::previewSetting()); }
	void writeCurrentValue() override {
		PerformSoundBrowser::setPreviewSetting(getValue<perform::BrowserPreview>());
		TEST_EVENT("gen_browser_preview_setting %d", static_cast<int>(PerformSoundBrowser::previewSetting()));
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shown()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shown(), yPixel); }

private:
	static const char* shown() {
		return l10n::get(kPreviewNames[static_cast<int32_t>(PerformSoundBrowser::previewSetting())]);
	}
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
		const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
		return genre != nullptr && part_ < deluge::generative::partCount(*genre) ? genre->parts[part_].name : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return part_ < performAdapter.steering().rosterParts();
	}
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::generative::kMaxTakes; }
	void readCurrentValue() override { refreshShown(); }
	void selectEncoderAction(int32_t offset) override {
		refreshShown();
		// The row as the CLIP page reads it, with the take this editor shows as the waiting one: its own press may
		// still be in the steering queue, not yet waiting in the core.
		perform::TakeRow row = performAdapter.steering().takeRow(part_);
		row.waiting = getValue() - 1 != row.playing ? getValue() - 1 : -1;
		PressOutcome outcome = PressOutcome::NothingToDo;
		const int32_t take = performAdapter.steering().stepTake(static_cast<uint8_t>(part_), row, offset, &outcome);
		basis_ = takeBasis(part_);
		refusedWithPopup(outcome);
		if (take < 0) { // none that way, or refused
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
			const perform::TakeRow row = performAdapter.steering().takeRow(part_);
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

// ---- OCTAVES ----

/// A pitched part's octave shift, -2 to +2 (spec §7.3, §13.6): a turn steps from the shift it shows, as a SHIFT + Y
/// turn does. It shows the shift as it goes on (PerformSteering::octaveAhead()), read afresh at each draw: a press in
/// the steering queue, the shift waiting for its bar line, or the one playing. Drums and FX have no row.
class OctaveItem final : public Integer {
public:
	explicit OctaveItem(int32_t part) : Integer(l10n::String::STRING_FOR_OCTAVES), part_(part) {}
	[[nodiscard]] std::string_view getName() const override {
		const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
		return genre != nullptr && part_ < deluge::generative::partCount(*genre) ? genre->parts[part_].name : "";
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return performAdapter.steering().takesOctave(part_);
	}
	// Only the shifts the part takes: those that keep every note of it a MIDI note (spec §7.3).
	[[nodiscard]] int32_t getMinValue() const override { return performAdapter.steering().octavesOf(part_).lowest; }
	[[nodiscard]] int32_t getMaxValue() const override { return performAdapter.steering().octavesOf(part_).highest; }
	void readCurrentValue() override { setValue(performAdapter.steering().octaveAhead(part_)); }
	void selectEncoderAction(int32_t offset) override {
		if (PerformView::octaveEdgePopup(part_, offset)) {
			return;
		}
		PressOutcome outcome = PressOutcome::NothingToDo;
		const int32_t shift = performAdapter.steering().stepOctave(static_cast<uint8_t>(part_), offset, &outcome);
		refusedWithPopup(outcome);
		if (shift == deluge::generative::kNoOctaveWaiting) { // none that way, or nothing to do
			return;
		}
		TEST_EVENT("gen_press octave part=%ld shift=%ld menu", static_cast<long>(part_), static_cast<long>(shift));
		setValue(shift);
		Number::selectEncoderAction(offset);
	}
	int32_t getSubmenuItemTypeRenderLength() override { return valueRenderLength(shownText()); }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override { drawRowValue(shownText(), yPixel); }

private:
	const char* shownText() {
		const int32_t shift = performAdapter.steering().octaveAhead(part_);
		snprintf(text_, sizeof(text_), shift > 0 ? "+%ld" : "%ld", static_cast<long>(shift));
		return text_;
	}
	int32_t part_;
	char text_[4] = {};
};

// ---- VARIANTS (spec §3.6, §13.6) ----

/// A variant set's row in Perform's VARIANTS ▸: its OLED name and the variant playing now (a Drop may have moved it). A
/// live set's row opens a plain list of its variants, on the one going on (variantAhead()): a pick queues for the next
/// Drop, `VARIANT DROP` in QUEUE, and the variant playing cancels it, as BACK does. A set that isn't live shows
/// AT REGEN when pushed: a Regenerate draws it.
class PerformVariantRow final : public Selection {
public:
	explicit PerformVariantRow(int32_t set) : Selection(l10n::String::STRING_FOR_VARIANTS), set_(set) {}
	[[nodiscard]] std::string_view getName() const override { return relevant() ? set().oledName : ""; }
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return relevant();
	}
	deluge::vector<std::string_view> getOptions(OptType /*optType*/) override {
		deluge::vector<std::string_view> options;
		if (relevant()) {
			for (const deluge::generative::Variant& variant : set().variants) {
				options.push_back(variant.oledName);
			}
		}
		return options;
	}
	bool shouldEnterSubmenu() override { return relevant() && set().live; }
	MenuItem* selectButtonPress() override {
		if (relevant() && !set().live) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_AT_REGEN));
			TEST_EVENT("gen_variants_live set=%s at_regen", set().name);
			return NO_NAVIGATION;
		}
		return Selection::selectButtonPress();
	}
	void readCurrentValue() override { setValue(std::max<int32_t>(performAdapter.steering().variantAhead(set_), 0)); }
	void writeCurrentValue() override {
		const PressOutcome outcome = performAdapter.steering().pressVariant(static_cast<uint8_t>(set_), getValue());
		refusedWithPopup(outcome);
		TEST_EVENT("gen_press variant set=%s variant=%s outcome=%d", set().name, set().variants[getValue()].name,
		           static_cast<int>(outcome));
	}
	int32_t getSubmenuItemTypeRenderLength() override {
		return set().live ? valueAndArrowRenderLength(shown()) : valueRenderLength(shown());
	}
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		if (set().live) {
			drawRowValueAndArrow(*this, shown(), yPixel);
		}
		else {
			drawRowValue(shown(), yPixel);
		}
	}
	/// The row's set and what it shows, for VARIANTS ▸'s open event.
	[[nodiscard]] const char* shownName() const { return shown(); }

private:
	[[nodiscard]] bool relevant() const {
		const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
		return genre != nullptr && set_ < static_cast<int32_t>(genre->variantSets.size());
	}
	[[nodiscard]] const deluge::generative::VariantSet& set() const {
		return performAdapter.steering().rosterGenre()->variantSets[set_];
	}
	[[nodiscard]] const char* shown() const {
		const int32_t playing = performAdapter.steering().variantPlaying(set_);
		return playing >= 0 ? set().variants[playing].oledName : "";
	}
	int32_t set_;
};

/// VARIANTS ▸: a row per variant set of the genre playing, shown only when it has sets.
class PerformVariantsMenu final : public Submenu {
public:
	using Submenu::Submenu;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
		return genre != nullptr && !genre->variantSets.empty();
	}
	void beginSession(MenuItem* navigatedBackwardFrom) override;
};

// ---- CANCEL CHANGE, GENRE, REGENERATE (spec §13.6) ----

/// Opens the Regenerate and genre-change page over the menu (spec §13.6): TRANSITION, VARIANTS ▸ and GO. On
/// `genre`, a genre change, or on the genre playing, a Regenerate.
MenuItem* confirmTransition(const deluge::generative::Genre* genre) {
	if (genre != nullptr) {
		openRegeneratePage(*genre);
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
		TEST_EVENT("gen_menu_cancel_change %s", cancelled ? "cancelled" : "refused");
		soundEditor.exitCompletely();
		return NO_NAVIGATION;
	}
};

/// REGENERATE: a genre change to the genre playing (spec §9.7), through the page.
class RegenerateItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override { return confirmTransition(currentRecipe.recipe.genre); }
};

/// RESTART ▸ AS IT IS (spec §13.2, §13.6): as X+PLAY, the piece starts again at the next bar line from the Current
/// Recipe as it is, or at once while stopped. The menu closes, so the popup and the queue show on the Perform page.
class RestartAsItIsItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		soundEditor.exitCompletely();
		performAdapter.pressRestart();
		return NO_NAVIGATION;
	}
};

/// RESTART ▸ AS IT BEGAN (spec §13.2, §13.6): the piece starts again as it began, from its header in the steering
/// log, once the sounds changed since have preloaded: at the next bar line, or at once while stopped. The menu closes.
class RestartAsItBeganItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		soundEditor.exitCompletely();
		restartBegan.press();
		return NO_NAVIGATION;
	}
};

/// RESET PART BIAS: the focused part's Part biases back to 0, or every part's with nothing focused (spec §3.3,
/// §13.6). It closes the menu, as REGENERATE's GO does, so the faders and rings show the result at once.
class ResetPartBiasItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		const int32_t part = performView.focusedPart();
		performAdapter.steering().resetBias(part);
		TEST_EVENT("gen_menu_reset_bias part=%ld", static_cast<long>(part));
		soundEditor.exitCompletely();
		return NO_NAVIGATION;
	}
};

// ---- CAPTURE ----

/// The CAPTURE menu's rows (spec §13.6, §17.4): the pieces in the log, newest first, each numbered in set order with
/// its genre and MARKED or -. A press toggles the piece's mark: the current one's as RECORD does, an earlier one's
/// until the next piece begins, which throws an unmarked one away. Only the newest kCaptureRows are listed.
constexpr int32_t kCaptureRows = 16;

class CaptureRowItem final : public MenuItem {
public:
	explicit CaptureRowItem(int32_t row) : MenuItem(l10n::String::STRING_FOR_CAPTURE), row_(row) {}
	[[nodiscard]] std::string_view getName() const override {
		read();
		return shown_.label;
	}
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return piece() >= 0;
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override {
		performAdapter.pressMark(piece());
		renderUIsForOled();
		return NO_NAVIGATION;
	}
	int32_t getSubmenuItemTypeRenderLength() override {
		read();
		return valueRenderLength(shown_.value);
	}
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		read();
		drawRowValue(shown_.value, yPixel);
	}

private:
	/// The piece this row shows, or -1: row 0 is the newest.
	[[nodiscard]] int32_t piece() const { return performAdapter.steeringLog().pieces() - 1 - row_; }
	void read() const { perform::captureRow(performAdapter.steeringLog(), piece(), shown_); }
	int32_t row_;
	mutable perform::CaptureRow shown_;
};

/// CAPTURE's only row before any piece: NOTHING PLAYED, which does nothing.
class NothingPlayedItem final : public MenuItem {
public:
	using MenuItem::MenuItem;
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return performAdapter.steeringLog().pieces() == 0;
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override { return NO_NAVIGATION; }
};

/// A genre of GENRE ▸ (genre_rows.h). The genre playing is labelled `= REGEN`, and picking it regenerates; picking
/// another is a genre change (spec §9.7, §13.6). Slots with no genre (a parent's row, or past the last) hide
/// themselves.
class TransitionGenreItem final : public MenuItem {
public:
	explicit TransitionGenreItem(GenreSlot slot) : slot_(slot) {}
	[[nodiscard]] std::string_view getName() const override { return genreLabel(slot_); }
	[[nodiscard]] std::string_view getTitle() const override { return getName(); }
	[[nodiscard]] bool isRelevant(ModControllableAudio* /*modControllable*/, int32_t /*whichThing*/) const override {
		return genreAt(slot_) != nullptr;
	}
	bool shouldEnterSubmenu() override { return false; }
	MenuItem* selectButtonPress() override { return confirmTransition(genreAt(slot_)); }
	int32_t getSubmenuItemTypeRenderLength() override { return playing() ? valueRenderLength(regen()) : 0; }
	void renderSubmenuItemTypeForOled(int32_t yPixel) override {
		if (playing()) {
			drawRowValue(regen(), yPixel);
		}
	}

private:
	[[nodiscard]] bool playing() const { return genreAt(slot_) == currentRecipe.recipe.genre; }
	static const char* regen() { return l10n::get(l10n::String::STRING_FOR_REGEN_MARK); }
	GenreSlot slot_;
};

PLACE_SDRAM_BSS CancelChangeItem cancelChangeItem{l10n::String::STRING_FOR_CANCEL_CHANGE};
PLACE_SDRAM_BSS RegenerateItem regenerateItem{l10n::String::STRING_FOR_REGENERATE};
PLACE_SDRAM_BSS RestartAsItIsItem restartAsItIsItem{l10n::String::STRING_FOR_AS_IT_IS};
PLACE_SDRAM_BSS RestartAsItBeganItem restartAsItBeganItem{l10n::String::STRING_FOR_AS_IT_BEGAN};
PLACE_SDRAM_BSS Submenu restartMenu{l10n::String::STRING_FOR_RESTART, {&restartAsItIsItem, &restartAsItBeganItem}};
PLACE_SDRAM_BSS ResetPartBiasItem resetPartBiasItem{l10n::String::STRING_FOR_RESET_PART_BIAS};
static_assert(kCaptureRows == 16, "one CaptureRowItem per row below");
PLACE_SDRAM_BSS CaptureRowItem captureRows[kCaptureRows] = {
    CaptureRowItem{0},  CaptureRowItem{1},  CaptureRowItem{2},  CaptureRowItem{3},
    CaptureRowItem{4},  CaptureRowItem{5},  CaptureRowItem{6},  CaptureRowItem{7},
    CaptureRowItem{8},  CaptureRowItem{9},  CaptureRowItem{10}, CaptureRowItem{11},
    CaptureRowItem{12}, CaptureRowItem{13}, CaptureRowItem{14}, CaptureRowItem{15}};
PLACE_SDRAM_BSS NothingPlayedItem nothingPlayedItem{l10n::String::STRING_FOR_NOTHING_PLAYED};
PLACE_SDRAM_BSS Submenu captureMenu{
    l10n::String::STRING_FOR_CAPTURE,
    {&nothingPlayedItem, &captureRows[0], &captureRows[1], &captureRows[2], &captureRows[3], &captureRows[4],
     &captureRows[5], &captureRows[6], &captureRows[7], &captureRows[8], &captureRows[9], &captureRows[10],
     &captureRows[11], &captureRows[12], &captureRows[13], &captureRows[14], &captureRows[15]},
};
PLACE_SDRAM_BSS GenreMenuItems<TransitionGenreItem> transitionGenres{l10n::String::STRING_FOR_GENRE};

PLACE_SDRAM_BSS KeyItem keyItem{l10n::String::STRING_FOR_KEY};
PLACE_SDRAM_BSS ScaleItem scaleItem{l10n::String::STRING_FOR_SCALE};
PLACE_SDRAM_BSS GlideItem glideItem{l10n::String::STRING_FOR_GLIDE};
PLACE_SDRAM_BSS BrowserPreviewItem browserPreviewItem{l10n::String::STRING_FOR_BROWSER_PREVIEW};

static_assert(deluge::generative::kMaxParts == 6, "one TakeItem per part slot below");
PLACE_SDRAM_BSS TakeItem takeItems[deluge::generative::kMaxParts] = {TakeItem{0}, TakeItem{1}, TakeItem{2},
                                                                     TakeItem{3}, TakeItem{4}, TakeItem{5}};
PLACE_SDRAM_BSS SeedTakesSubmenu seedTakesMenu{
    l10n::String::STRING_FOR_SEED_AND_TAKES,
    {&takeItems[0], &takeItems[1], &takeItems[2], &takeItems[3], &takeItems[4], &takeItems[5]},
};

static_assert(deluge::generative::kMaxVariantSets == 8, "one PerformVariantRow per set slot below");
PLACE_SDRAM_BSS PerformVariantRow variantRows[deluge::generative::kMaxVariantSets] = {
    PerformVariantRow{0}, PerformVariantRow{1}, PerformVariantRow{2}, PerformVariantRow{3},
    PerformVariantRow{4}, PerformVariantRow{5}, PerformVariantRow{6}, PerformVariantRow{7}};
PLACE_SDRAM_BSS PerformVariantsMenu variantsMenu{
    l10n::String::STRING_FOR_VARIANTS,
    {&variantRows[0], &variantRows[1], &variantRows[2], &variantRows[3], &variantRows[4], &variantRows[5],
     &variantRows[6], &variantRows[7]},
};

void PerformVariantsMenu::beginSession(MenuItem* navigatedBackwardFrom) {
	if (navigatedBackwardFrom == nullptr) {
		const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
		const int32_t sets = genre != nullptr ? static_cast<int32_t>(genre->variantSets.size()) : 0;
		TEST_EVENT("gen_perform_variants open sets=%ld", static_cast<long>(sets));
		for (int32_t s = 0; s < sets; s++) {
			TEST_EVENT("gen_perform_variants_row %s %s live=%d", genre->variantSets[s].oledName,
			           variantRows[s].shownName(), genre->variantSets[s].live ? 1 : 0);
		}
	}
	Submenu::beginSession(navigatedBackwardFrom);
}

static_assert(deluge::generative::kMaxParts == 6, "one OctaveItem per part slot below");
PLACE_SDRAM_BSS OctaveItem octaveItems[deluge::generative::kMaxParts] = {OctaveItem{0}, OctaveItem{1}, OctaveItem{2},
                                                                         OctaveItem{3}, OctaveItem{4}, OctaveItem{5}};
PLACE_SDRAM_BSS Submenu octavesMenu{
    l10n::String::STRING_FOR_OCTAVES,
    {&octaveItems[0], &octaveItems[1], &octaveItems[2], &octaveItems[3], &octaveItems[4], &octaveItems[5]},
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
    {&cancelChangeItem, &transitionGenres.menu(), &regenerateItem, &variantsMenu, &keyScaleMenuItem, &glideItem,
     &seedTakesMenu, &octavesMenu, &resetPartBiasItem, &restartMenu, &captureMenu, &browserPreviewItem},
};

void refreshPerformMenu() {
	// Whether anything a Perform menu value reads under moved since the last frame.
	uint32_t presses = performAdapter.steering().keyPresses();
	for (int32_t part = 0; part < deluge::generative::kMaxParts; part++) {
		presses += performAdapter.steering().takePresses(part);
	}
	presses += performAdapter.steering().octavePresses();
	// VARIANTS' rows move with the variant playing: a Drop may change it.
	for (int32_t set = 0; set < deluge::generative::kMaxVariantSets; set++) {
		presses += static_cast<uint32_t>(performAdapter.steering().variantPlaying(set) + 1) << (3 * set);
	}
	// CANCEL CHANGE shows only while a transition can be cancelled: its phase moves it too.
	presses += static_cast<uint32_t>(transitions.phase()) << 24;
	// CAPTURE's rows move with the marks (RECORD under the menu), the pieces and each piece opened.
	const SteeringLog& log = performAdapter.steeringLog();
	presses += static_cast<uint32_t>(log.markedPieces()) << 8 | static_cast<uint32_t>(log.pieces()) << 16;
	presses += log.opened() << 20; // a new piece can drop one and add one, keeping both counts
	static Basis shownBasis;
	if (shownBasis == basisNow(presses)) {
		return;
	}
	shownBasis = basisNow(presses);
	MenuItem* item = soundEditor.getCurrentMenuItem();
	bool ours = item == &performMenu || item == &transitionGenres.menu() || item == &keyScaleMenuItem
	            || item == &keyItem || item == &scaleItem || item == &seedTakesMenu || item == &octavesMenu
	            || item == &captureMenu || item == &variantsMenu;
	for (TakeItem& take : takeItems) {
		ours = ours || item == &take;
	}
	for (OctaveItem& octave : octaveItems) {
		ours = ours || item == &octave;
	}
	for (PerformVariantRow& row : variantRows) {
		ours = ours || item == &row;
	}
	for (GenreGroup& group : transitionGenres.groups()) {
		ours = ours || item == &group; // its `= REGEN` row moves when a genre change starts
	}
	if (ours) {
		item->readValueAgain();
	}
}

} // namespace deluge::gui::menu_item::generative
