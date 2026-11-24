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

// The stock menu rows' layout in pixels (spec §13.5's OLED rules): where a row's name, value and icon go, so the
// harness can check OLED names fit a row (regenerate_page_spec). The firmware's rows draw with these
// (gui/menu_item/generative/row_value.h), which asserts they match the stock constants.

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

/// The stock menu rows' font (the OLED's 7 px Apple ][ font, monospaced): a character's advance, its gap included.
constexpr int32_t kMenuCharPixels = 6;
constexpr int32_t kMenuWidthPixels = 128;
/// A row's name starts one character in (Submenu::drawSubmenuItemsForOled()).
constexpr int32_t kMenuNameInset = kMenuCharPixels;
/// Pixels between a row's name and its value, and after the value.
constexpr int32_t kRowValueGap = 4;
constexpr int32_t kRowValueRightMargin = 3;
/// The stock submenu arrow's and checkbox's width (kSubmenuIconSpacingX), and its gap from the right edge
/// (MenuItem::getSubmenuItemTypeRenderIconStart()).
constexpr int32_t kMenuIconPixels = 7;
constexpr int32_t kMenuIconRightMargin = 3;

/// Whether a row whose name has `nameChars` characters and whose value `valueChars` fits a 128 px row without being
/// cut off: with `icon` the stock arrow (or a toggle's checkbox) at its right too, after the value's gap.
constexpr bool menuRowFits(int32_t nameChars, int32_t valueChars, bool icon) {
	int32_t right = 0;
	if (valueChars > 0) {
		right += kRowValueGap + valueChars * kMenuCharPixels + kRowValueRightMargin;
	}
	if (icon) {
		right +=
		    valueChars > 0 ? kMenuIconPixels + kRowValueGap : kRowValueGap + kMenuIconPixels + kMenuIconRightMargin;
	}
	return kMenuNameInset + nameChars * kMenuCharPixels <= kMenuWidthPixels - right;
}

} // namespace deluge::generative::perform
