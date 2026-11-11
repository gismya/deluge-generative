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

#include "generative/perform/sidebar.h"
#include "generative/perform/text.h"

namespace deluge::generative::perform {

void composeSidebar(const SidebarInput& in, Sidebar& out) {
	out = {};
	const Generator* piece = in.piece;
	using Look = SidebarCell::Look;

	for (int32_t p = 0; p < in.parts; p++) {
		SidebarCell& cell = out.cell[p][kSidebarMuteColumn];
		cell.look = Look::Active;
		if (piece != nullptr) {
			// Solo first: a soloed-out muted part stays red (spec §13.5).
			cell.look = piece->partSoloed(p)                           ? Look::Solo
			            : piece->partMuted(p)                          ? Look::Muted
			            : piece->anySoloed() || piece->partSilenced(p) ? Look::Dim
			                                                           : Look::Active;
			cell.blink = piece->mutePending(p) || piece->soloPending(p);
		}
	}

	for (int32_t row = 0; row < kSidebarForceRowCount; row++) {
		const SectionKind kind = kSidebarForceRows[row];
		out.cell[row][kSidebarSectionColumn] = {
		    .look = Look::Section,
		    .kind = kind,
		    .bright = in.sounding && in.playing == kind,
		    .blink = piece != nullptr && piece->sectionForced(kind),
		};
	}
	out.cell[kSidebarHoldRow][kSidebarSectionColumn] = {
	    .look = Look::Hold,
	    .bright = piece != nullptr && piece->arc().held(),
	};
	// Bright while held or while the Outro plays; queued, it blinks at the level it has (the prototype's).
	out.cell[kSidebarEndRow][kSidebarSectionColumn] = {
	    .look = Look::Section,
	    .kind = SectionKind::Outro,
	    .bright = in.endHeld || (in.sounding && in.playing == SectionKind::Outro),
	    .blink = piece != nullptr && piece->endQueued(),
	};
}

void describeSidebar(const Sidebar& sidebar, char (&out)[kSidebarTextChars + 1]) {
	Text text(out);
	text.add("mute=");
	for (const auto& row : sidebar.cell) {
		const SidebarCell& cell = row[kSidebarMuteColumn];
		constexpr const char* kLetters[] = {"", "a", "m", "s", "d"}; // Look order
		if (cell.look == SidebarCell::Look::Off) {
			break; // past the roster
		}
		text.add(kLetters[static_cast<int32_t>(cell.look)]).add(cell.blink ? "*" : "");
	}
	constexpr const char* kForceLetters[] = {"G", "B", "U"}; // kSidebarForceRows order
	const char* force = "-";
	for (int32_t row = 0; row < kSidebarForceRowCount; row++) {
		force = sidebar.cell[row][kSidebarSectionColumn].blink ? kForceLetters[row] : force;
	}
	text.add(" force=").add(force);
	text.add(" hold=").add(sidebar.cell[kSidebarHoldRow][kSidebarSectionColumn].bright ? "1" : "0");
	text.add(" end=").add(sidebar.cell[kSidebarEndRow][kSidebarSectionColumn].blink ? "q" : "-");
}

} // namespace deluge::generative::perform
