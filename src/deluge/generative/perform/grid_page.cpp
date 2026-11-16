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

#include "generative/perform/grid_page.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/keyboard_page.h"

namespace deluge::generative::perform {

bool shiftOpensSoundEditor(GridPage page, int32_t x, int32_t row) {
	switch (page) {
	case GridPage::Keyboard:
		return keyboardPad(x, row) != KeyboardPad::Fill;
	case GridPage::Clip:
		return row != kActionsRow || (x != kNewSoundPad && x != kSoundEditorPad);
	case GridPage::Song:
		break;
	}
	return true;
}

} // namespace deluge::generative::perform
