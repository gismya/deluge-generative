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

// The sidebar (spec §13.5) as portable cells: MUTE/LAUNCH rows 1–6 mute or solo the parts, AUDITION/SECTION rows 1–3
// force Groove, Break or Build, row 5 holds the Arc and row 8 is End. The Perform view turns the cells into colours
// and applies the blink phase; this decides what each pad shows, and the test port's gen_sidebar text is made from
// the same cells.

#pragma once

#include "generative/core/generator.h"
#include <cstdint>

namespace deluge::generative::perform {

/// The sidebar's columns from the left, and its rows counted from the top from 0, as the spec counts them.
constexpr int32_t kSidebarMuteColumn = 0;
constexpr int32_t kSidebarSectionColumn = 1;
/// AUDITION/SECTION rows 1–3 force these.
constexpr SectionKind kSidebarForceRows[] = {SectionKind::Groove, SectionKind::Break, SectionKind::Build};
constexpr int32_t kSidebarForceRowCount = sizeof(kSidebarForceRows) / sizeof(kSidebarForceRows[0]);
constexpr int32_t kSidebarHoldRow = 4;
constexpr int32_t kSidebarEndRow = 7;

/// What the sidebar reads, from task context between ticks (spec §2.8).
struct SidebarInput {
	/// The piece playing, or nullptr while stopped: every start is fresh, with nothing muted, queued or held.
	const Generator* piece = nullptr;
	/// Parts in the roster.
	int32_t parts = 0;
	/// A Section has been heard, and `playing` is it.
	bool sounding = false;
	SectionKind playing = SectionKind::Intro;
	/// The End pad is held down.
	bool endHeld = false;
};

struct SidebarCell {
	enum class Look : uint8_t {
		Off,
		Active,  ///< a part that sounds
		Muted,   ///< a muted part
		Solo,    ///< a soloed part
		Dim,     ///< a part silenced by the Arc, or soloed out
		Section, ///< a force pad or End, in `kind`'s colour
		Hold,    ///< the Arc hold
	};
	Look look = Look::Off;
	/// Section cells: the kind the pad forces; End is the Outro.
	SectionKind kind = SectionKind::Intro;
	/// Section cells: the kind plays now, or the End pad is held. Hold: the Arc is held.
	bool bright = false;
	/// A press waits for its line: the pad blinks.
	bool blink = false;
	bool operator==(const SidebarCell&) const = default;
};

struct Sidebar {
	static constexpr int32_t kRows = 8;
	static constexpr int32_t kColumns = 2;
	/// Rows from the top, 0 = row 1.
	SidebarCell cell[kRows][kColumns];
};

void composeSidebar(const SidebarInput& in, Sidebar& out);

/// The test port's gen_sidebar text, the longest being 38 characters.
constexpr int32_t kSidebarTextChars = 47;
/// The sidebar as text: a letter per part (a active, m muted, s solo, d dim), followed by * while a press waits; the
/// forced Section (G, B, U or -); the hold; End (q while queued). "mute=am*a force=B hold=0 end=-".
void describeSidebar(const Sidebar& sidebar, char (&out)[kSidebarTextChars + 1]);

} // namespace deluge::generative::perform
