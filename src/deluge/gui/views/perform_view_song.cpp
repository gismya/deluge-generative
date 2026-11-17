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

// PerformView's SONG page (spec §13.5): the Arc map and its scroll, and the sidebar (mute, solo, Sections, HOLD,
// End).

#include "gui/views/perform_view_internal.h"

namespace {

// The sidebar (spec §13.5), laid out in generative/perform/sidebar.h. The spec counts rows from the top; firmware pad
// y 0 is the bottom row.
constexpr int32_t kMuteColumn = kDisplayWidth + deluge::generative::perform::kSidebarMuteColumn;

constexpr int32_t kSectionColumn = kDisplayWidth + deluge::generative::perform::kSidebarSectionColumn;

/// AUDITION/SECTION: row 5 holds; row 8 is End.
constexpr int32_t kHoldPadY = rowFromY(deluge::generative::perform::kSidebarHoldRow);

/// The End pad. Held this long, it arms End.
constexpr int32_t kEndPadY = rowFromY(deluge::generative::perform::kSidebarEndRow);

constexpr uint32_t kEndHoldSamples = kSampleRate * 6 / 10;

/// Row 7, Energy against the centre: green at it (within this many points), blue below, orange above.
constexpr int32_t kEnergyAtCentre = 2;

constexpr RGB kEnergyAt = RGB(0, 120, 40);

constexpr RGB kEnergyBelow = RGB(40, 90, 255);

constexpr RGB kEnergyAbove = RGB(255, 130, 0);

/// A colour at `amount` of `range`, from a sixth of it up.
constexpr RGB scaled(RGB colour, int32_t amount, int32_t range) {
	const int32_t level = std::clamp<int32_t>(amount * 256 / range, 40, 256);
	return RGB(colour.r * level / 256, colour.g * level / 256, colour.b * level / 256);
}

} // namespace

ActionResult PerformView::horizontalEncoderAction(int32_t offset) {
	using namespace deluge::hid::button;
	xUsed_ = Buttons::isButtonPressed(X_ENC) || xUsed_;
	if (arc_.scrollBy(arcMapInput(), offset)) {
		TEST_EVENT("gen_arc_scroll %ld", static_cast<long>(arc_.scroll()));
		refreshArcMap();
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::sidebarPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::hid::button;
	if (x == kSectionColumn && y == kEndPadY) {
		if (!endPadActs()) {
			return;
		}
		if (on) {
			// Held 0.6 s, it arms End; the pad brightens while held (spec §13.5).
			endHeld_ = true;
			endArmedThisHold_ = false;
			endPressedAt_ = AudioEngine::audioSampleTimer; // graphicsRoutine() arms it once held long enough
		}
		else if (endHeld_) {
			endHeld_ = false;
			if (!endArmedThisHold_) {
				// A tap cancels End while it waits (spec §13.5).
				refusedWithPopup(performAdapter.steering().cancelEnd());
				TEST_EVENT("gen_end tap");
			}
		}
		return;
	}
	if (!on) {
		return;
	}
	const bool now = Buttons::isShiftButtonPressed();
	if (x == kMuteColumn) {
		const int32_t part = rowFromY(y);
		if (part >= performAdapter.steering().rosterParts()) {
			return;
		}
		// X held + MUTE = solo, as stock (spec §13.5). That X's release doesn't return the Arc map to now.
		const bool solo = Buttons::isButtonPressed(X_ENC);
		xUsed_ = xUsed_ || solo;
		refusedWithPopup(solo ? performAdapter.steering().pressSolo(part, now)
		                      : performAdapter.steering().pressMute(part, now));
		TEST_EVENT("gen_press %s part=%ld now=%d", solo ? "solo" : "mute", static_cast<long>(part), now ? 1 : 0);
		return;
	}
	if (y == kHoldPadY) {
		refusedWithPopup(performAdapter.steering().pressHold());
		TEST_EVENT("gen_press hold");
		return;
	}
	const int32_t row = rowFromY(y);
	if (row < deluge::generative::perform::kSidebarForceRowCount) {
		const SectionKind kind = deluge::generative::perform::kSidebarForceRows[row];
		refusedWithPopup(performAdapter.steering().forceSection(kind));
		TEST_EVENT("gen_press force %s", deluge::generative::sectionName(kind));
	}
}

void PerformView::checkEndHold() {
	if (!endHeld_ || endArmedThisHold_) {
		return;
	}
	if (!endPadActs()) {
		return; // a hold an overlay cut short never arms
	}
	if (AudioEngine::audioSampleTimer - endPressedAt_ >= kEndHoldSamples) {
		endArmedThisHold_ = true;
		if (refusedWithPopup(performAdapter.steering().armEnd())) {
			TEST_EVENT("gen_end refused");
			return;
		}
		TEST_EVENT("gen_end armed held_ms=%ld",
		           static_cast<long>(TestPort::samplesToMs(AudioEngine::audioSampleTimer - endPressedAt_)));
	}
}

bool PerformView::endPadActs() {
	return getCurrentUI() == this;
}

void PerformView::releaseEndPad() {
	endHeld_ = false;
	endArmedThisHold_ = false;
}

namespace {
/// A sidebar cell's colour now: stock mute colours, the Section kinds' colours, queued ones blinking.
RGB sidebarColour(const deluge::generative::perform::SidebarCell& cell, bool blinkOff) {
	using namespace deluge::gui;
	using Look = deluge::generative::perform::SidebarCell::Look;
	if (cell.blink && blinkOff) {
		return colours::black;
	}
	// Stock colours: green active, red muted, blue solo, dim = silenced by the Arc or soloed out (spec §13.5).
	switch (cell.look) {
	case Look::Active:
		return menu_item::activeColourMenu.getRGB();
	case Look::Muted:
		return menu_item::stoppedColourMenu.getRGB();
	case Look::Solo:
		return menu_item::soloColourMenu.getRGB();
	case Look::Dim:
		return menu_item::activeColourMenu.getRGB().dull();
	case Look::Section:
		return sectionColour(cell.kind, cell.bright);
	case Look::Hold:
		return cell.bright ? colours::white : RGB::monochrome(40);
	case Look::Off:
		break;
	}
	return colours::black;
}
#if ENABLE_SYSEX_TEST
char shownSidebarState[deluge::generative::perform::kSidebarTextChars + 1] = "";
#endif
} // namespace

void PerformView::refreshSidebar([[maybe_unused]] bool report) {
	using namespace deluge::generative::perform;
	static_assert(Sidebar::kRows == kDisplayHeight && Sidebar::kColumns == kSideBarWidth);
	const auto& output = performAdapter.output();
	SidebarInput in;
	in.piece = performAdapter.playingGenerator();
	in.parts = performAdapter.steering().rosterParts();
	in.sounding = in.piece != nullptr && output.sectionKnown;
	in.playing = output.section;
	in.endHeld = endHeld_;
	Sidebar composed;
	composeSidebar(in, composed);
#if ENABLE_SYSEX_TEST
	if (report) {
		char text[sizeof(shownSidebarState)];
		describeSidebar(composed, text);
		if (strcmp(text, shownSidebarState) != 0) {
			strcpy(shownSidebarState, text);
			TEST_EVENT("gen_sidebar %s", text);
		}
	}
#endif
	const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
	uint32_t changed = 0;
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		for (int32_t x = 0; x < kSideBarWidth; x++) {
			const RGB colour = sidebarColour(composed.cell[rowFromY(y)][x], blinkOff);
			if (colour != sidebar_[y][x]) {
				sidebar_[y][x] = colour;
				changed |= 1U << y;
			}
		}
	}
	// Redraw only changed rows: a full redraw costs 21–22 ms of UART (spec §13.5).
	if (changed != 0) {
		uiNeedsRendering(this, 0, changed);
	}
}

void PerformView::refreshPlan() {
#if ENABLE_SYSEX_TEST
	const uint32_t start = getSystemTicks();
#endif
	if (arc_.refreshPlan(performAdapter.playingGenerator(), transitions.starts())) {
		TEST_EVENT("gen_arcplan first=%ld us=%lu", static_cast<long>(arc_.plan().first),
		           static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000000)));
	}
}

deluge::generative::perform::ArcMapInput PerformView::arcMapInput() const {
	deluge::generative::perform::ArcMapInput in;
	in.piece = performAdapter.playingGenerator();
	in.clock = std::max<int32_t>(performAdapter.clock(), 0);
	in.focus = static_cast<int8_t>(focusedPart());
	in.history = &performAdapter.history();
	return in;
}

RGB PerformView::arcMapColour(const ArcMapCell& cell, bool blinkOff) const {
	using namespace deluge::gui;
	RGB colour = colours::black;
	const bool planned = cell.when == ArcMapCell::When::Planned;
	switch (cell.look) {
	case ArcMapCell::Look::Off:
		break;
	case ArcMapCell::Look::Part: {
		// Bright = sounded, the now column flashing on each note, dim = planned to sound (spec §13.5).
		const auto* genre = performAdapter.playingGenerator() != nullptr
		                        ? performAdapter.playingGenerator()->recipe().genre
		                        : currentRecipe.recipe.genre;
		const RGB role =
		    genre != nullptr ? kRoleColours[static_cast<int32_t>(genre->parts[cell.part].role)] : colours::white;
		colour = cell.hit ? role : planned ? role.dim(4) : role.dim(1);
		break;
	}
	case ArcMapCell::Look::Muted:
		colour = RGB(30, 0, 0);
		break;
	case ArcMapCell::Look::Focus:
		colour = colours::white;
		break;
	case ArcMapCell::Look::Energy:
		colour = std::abs(cell.energy) < kEnergyAtCentre ? kEnergyAt
		         : cell.energy < 0                       ? scaled(kEnergyBelow, -cell.energy, 20)
		                                                 : scaled(kEnergyAbove, cell.energy, 14);
		colour = planned ? colour.dim(1) : colour;
		break;
	case ArcMapCell::Look::Section:
		colour = cell.when == ArcMapCell::When::Now ? sectionColour(cell.kind, true)
		         : planned                          ? sectionColour(cell.kind, cell.drop).dim(cell.drop ? 1 : 2)
		                                            : sectionColour(cell.kind, false);
		break;
	}
	if (cell.blink) {
		// A queued action blinks where it lands; on a dark pad it blinks grey.
		return blinkOff ? colours::black : colour == colours::black ? RGB::monochrome(40) : colour;
	}
	return colour;
}

void PerformView::refreshArcMap() {
	using namespace deluge::generative::perform;
	const ArcMapInput in = arcMapInput();
	// On every page: the flash window is sampled once per step, and the scroll stays inside what's known.
	arc_.sampleFlash(in.clock);
	if (arc_.clampScroll(in)) {
		TEST_EVENT("gen_arc_scroll %ld", static_cast<long>(arc_.scroll()));
	}
#if !ENABLE_SYSEX_TEST
	if (page_ != Page::Song) {
		return; // nothing reads the map
	}
#endif
	// Test builds compose the map on every page, to describe it for the probe scripts.
	ArcMap map;
	composeArcMap(in, arc_, map);
	if (page_ == Page::Song) {
		// Redrawn only on a change: at most each step (the now column's flash), each blink and each phrase line.
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		RGB pads[kDisplayHeight][kDisplayWidth];
		for (int32_t row = 0; row < ArcMap::kRows; row++) {
			const int32_t y = kDisplayHeight - 1 - row; // firmware y 0 is the bottom row
			for (int32_t x = 0; x < kDisplayWidth; x++) {
				pads[y][x] = arcMapColour(map.cell[row][x], blinkOff);
			}
		}
		showMainPads(pads);
	}
#if ENABLE_SYSEX_TEST
	{
		const deluge::generative::Genre* genre = in.piece != nullptr ? in.piece->recipe().genre : nullptr;
		const int32_t phrase = genre != nullptr ? in.clock / (genre->phraseSteps() * kTicksPerStep) : -1;
		if (phrase != redrawsPhrase_) {
			if (redrawsPhrase_ >= 0) {
				TEST_EVENT("gen_pad_redraws phrase=%ld main=%lu", static_cast<long>(redrawsPhrase_),
				           static_cast<unsigned long>(mainRedraws_));
			}
			redrawsPhrase_ = phrase;
			mainRedraws_ = 0;
		}
		char text[sizeof(shownSections)];
		describeArcMapSections(map, text);
		if (strcmp(text, shownSections) != 0) {
			strcpy(shownSections, text);
			TEST_EVENT("gen_arcmap %s", text);
		}
		const int32_t parts = genre != nullptr ? deluge::generative::partCount(*genre) : 0;
		for (int32_t p = 0; p < parts; p++) {
			char row[ArcMap::kColumns + 1];
			describeArcMapPart(map, p, row);
			if (strcmp(row, shownParts[p]) != 0) {
				strcpy(shownParts[p], row);
				TEST_EVENT("gen_arcrow %ld %s", static_cast<long>(p), row);
			}
		}
	}
#endif
}
