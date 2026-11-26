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

// A value right-aligned after a row's name in a Generative submenu (spec §13.6, §14.4), as stock draws a submenu
// row's arrow, and a menu title that side-scrolls when it's too wide.

#pragma once

#include "generative/perform/menu_rows.h"
#include "gui/menu_item/menu_item.h"
#include "hid/display/oled.h"
#include <cstdint>
#include <cstring>
#include <string_view>

namespace deluge::gui::menu_item::generative {

/// Pixels between the row's name and its value, and after it: the portable row layout's (perform/menu_rows.h), which
/// regenerate_page_spec checks the OLED names against. The asserts keep that layout the stock one.
constexpr int32_t kValueGap = deluge::generative::perform::kRowValueGap;
constexpr int32_t kValueRightMargin = deluge::generative::perform::kRowValueRightMargin;
static_assert(deluge::generative::perform::kMenuCharPixels == kTextSpacingX,
              "menu_rows.h's character advance is the stock menu font's");
static_assert(deluge::generative::perform::kMenuIconPixels == kSubmenuIconSpacingX,
              "menu_rows.h's icon width is the stock submenu arrow's");
static_assert(deluge::generative::perform::kMenuWidthPixels == OLED_MAIN_WIDTH_PIXELS,
              "menu_rows.h's row width is the OLED's");

/// Draws a menu screen's `title` in the bold title font with its separator, as MenuItem::renderOLED() does, but
/// side-scrolling when it's wider than the screen, as a long song name does in the stock session view.
inline void drawScrollingTitle(std::string_view title) {
	deluge::hid::display::oled_canvas::Canvas& canvas = deluge::hid::display::OLED::main;
	constexpr int32_t kTitleY = OLED_MAIN_TOPMOST_PIXEL + 1;
	canvas.drawString(title, 0, kTitleY, kTextTitleSpacingX, kTextTitleSizeY);
	canvas.drawHorizontalLine(kScreenTitleSeparatorY, 0, OLED_MAIN_WIDTH_PIXELS - 1);
	if (canvas.getStringWidthInPixels(title.data(), kTextTitleSizeY) > OLED_MAIN_WIDTH_PIXELS) {
		deluge::hid::display::OLED::setupSideScroller(1, title, 0, OLED_MAIN_WIDTH_PIXELS, kTitleY,
		                                              kTitleY + kTextTitleSizeY, kTextTitleSpacingX, kTextTitleSizeY,
		                                              false);
	}
	deluge::hid::display::OLED::markChanged();
}

/// The pixels a row's `value` takes at its right, gap and margin included.
inline int32_t valueRenderLength(const char* value) {
	return kValueGap + static_cast<int32_t>(strlen(value)) * kTextSpacingX + kValueRightMargin;
}

/// Draws a row's `value` right-aligned, ending `rightMargin` pixels from the screen's edge.
inline void drawRowValue(const char* value, int32_t yPixel, int32_t rightMargin = kValueRightMargin) {
	deluge::hid::display::OLED::main.drawStringAlignRight(value, yPixel, kTextSpacingX, kTextSpacingY,
	                                                      OLED_MAIN_WIDTH_PIXELS - rightMargin);
}

/// The pixels a submenu row with both a value and the stock arrow takes at its right.
inline int32_t valueAndArrowRenderLength(const char* value) {
	return valueRenderLength(value) + kSubmenuIconSpacingX + kValueGap;
}

/// Draws `value` right-aligned before the stock submenu arrow, then the arrow.
inline void drawRowValueAndArrow(MenuItem& item, const char* value, int32_t yPixel) {
	drawRowValue(value, yPixel, OLED_MAIN_WIDTH_PIXELS - item.getSubmenuItemTypeRenderIconStart() + kValueGap);
	deluge::hid::display::OLED::main.drawGraphicMultiLine(deluge::hid::display::OLED::submenuArrowIcon,
	                                                      item.getSubmenuItemTypeRenderIconStart(), yPixel,
	                                                      kSubmenuIconSpacingX);
}

} // namespace deluge::gui::menu_item::generative
