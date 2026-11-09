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

// Popups Perform's view and menus share.

#pragma once

#include "gui/l10n/l10n.h"
#include "hid/display/display.h"

namespace deluge::gui {

/// Shows the stock popup if the full steering queue refused a press (spec §2.6).
inline void popupIfRefused(bool accepted) {
	if (!accepted) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_QUEUE_FULL));
	}
}

} // namespace deluge::gui
