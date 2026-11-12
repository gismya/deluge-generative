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

#include "generative/perform/press_outcome.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"

namespace deluge::gui {

/// Whether a steering press was refused (generative::perform::refused()), showing its popup if so (spec §2.6, §12.9,
/// §12.10): QUEUE FULL, CHANGE PENDING or BUSY. An accepted press, or one with nothing to do, shows nothing.
/// `if (refusedWithPopup(...)) return;`: the caller goes no further.
inline bool refusedWithPopup(generative::perform::PressOutcome outcome) {
	using generative::perform::PressOutcome;
	l10n::String text;
	switch (outcome) {
	case PressOutcome::QueueFull:
		text = l10n::String::STRING_FOR_QUEUE_FULL;
		break;
	case PressOutcome::ChangePending:
		text = l10n::String::STRING_FOR_CHANGE_PENDING;
		break;
	case PressOutcome::Busy:
		text = l10n::String::STRING_FOR_GENERATIVE_BUSY;
		break;
	case PressOutcome::Accepted:
	case PressOutcome::NothingToDo:
	default:
		return false;
	}
	display->displayPopup(l10n::get(text));
	return true;
}

} // namespace deluge::gui
