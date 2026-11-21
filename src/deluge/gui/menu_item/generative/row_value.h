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
// row's arrow.

#pragma once

#include "gui/menu_item/menu_item.h"
#include "hid/display/oled.h"
#include <cstdint>
#include <cstring>

namespace deluge::gui::menu_item::generative {

constexpr int32_t kValueGap = 4; ///< pixels between the row's name and its value
constexpr int32_t kValueRightMargin = 3;

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
