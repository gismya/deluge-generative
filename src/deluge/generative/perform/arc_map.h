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

// The SONG page (spec §13.5) as portable cells: the Arc map on the main pads and the ARC page under the OLED status
// line. The Perform view turns the cells into colours and pixels; this decides what each one shows.

#pragma once

#include "generative/core/arc_plan.h"
#include "generative/perform/phrase_history.h"
#include "generative/perform/queue_text.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// What the SONG page reads, from task context between ticks (spec §2.8).
struct ArcMapInput {
	bool playing = false;
	/// Generator ticks since step 0's grid line, as heard: the now column is the phrase it is in.
	int32_t clock = 0;
	int32_t phraseSteps = 8 * kStepsPerBar;
	/// Parts in the roster playing.
	int32_t parts = 0;
	/// Columns the map is scrolled by X: negative = back over what played (spec §13.5).
	int32_t scroll = 0;
	/// The part on the gold knobs, or -1.
	int8_t focus = -1;
	/// The now column flashes a part whose last note-on fell in [hitFrom, hitTo] (Generator ticks). The Perform view
	/// samples the window once per step, so the flash redraws the pads at most once per 16th (spec §13.5).
	int32_t hitFrom = 0;
	int32_t hitTo = -1;
	const PhraseHistory* history = nullptr;
	/// From Generator::planArc(), worked out only when a phrase line passes or the queue changes.
	const ArcPlan* plan = nullptr;
	/// The pending list, soonest first (queuedByTime()).
	std::span<const QueuedAction> queued;
};

struct ArcMapCell {
	enum class Look : uint8_t {
		Off,
		Part,   ///< `part` sounded (Played), sounds now (Now) or is planned to (Planned)
		Muted,  ///< planned, but `part` is muted there
		Focus,  ///< the now column of the part on the gold knobs
		Energy, ///< `energy` against the centre
		Section,
	};
	enum class When : uint8_t { Played, Now, Planned };
	Look look = Look::Off;
	When when = When::Played;
	uint8_t part = 0;
	SectionKind kind = SectionKind::Intro;
	/// Energy cells: effective Energy against the Recipe centre, in OLED points, rounded toward zero.
	int8_t energy = 0;
	/// A Section cell on an Arc Drop.
	bool drop = false;
	/// The now column: the part plays a note on the step being heard (it flashes live).
	bool hit = false;
	/// A queued action lands in this column: a part's mute, solo or take on its row, a force, End or key change on
	/// the Section row.
	bool blink = false;
	bool operator==(const ArcMapCell&) const = default;
};

struct ArcMap {
	static constexpr int32_t kColumns = 16;
	static constexpr int32_t kRows = 8;
	/// Column 4 is now (spec §13.5).
	static constexpr int32_t kNowColumn = 3;
	static constexpr int32_t kEnergyRow = 6;
	static constexpr int32_t kSectionRow = 7;
	/// Rows from the top as the spec counts them, 0 = row 1: rows 1–6 the parts, 7 Energy, 8 the Sections.
	ArcMapCell cell[kRows][kColumns];
};

void composeArcMap(const ArcMapInput& in, ArcMap& out);

/// How far X scrolls: back until the oldest phrase kept is in column 1 (one fewer than the ring holds, since the
/// next phrase's decisions take the oldest slot just before its line), ahead until the plan's last phrase is in
/// column 16. Never past now in either direction.
struct ArcMapRange {
	int32_t back = 0;
	int32_t ahead = 0;
};
ArcMapRange arcMapScrollRange(const ArcMapInput& in);

/// One phrase box on the ARC page.
struct ArcBox {
	char tag[3] = {}; ///< "GR", "BR"…; empty after the piece has stopped
	bool now = false;
	bool drop = false;
	bool blink = false; ///< a queued action lands in it
	bool ended = false;
	bool operator==(const ArcBox&) const = default;
};

/// The ARC page (spec §13.5): now and the next 7 phrases as boxes, and the Arc's next change with its countdown, as
/// "NEXT BREAK 6 BARS", "NEXT DROP 2 BARS" or "STOP 3 BARS"; "NO CHANGE PLANNED" if the plan holds none.
struct ArcPage {
	static constexpr int32_t kBoxes = 8;
	ArcBox box[kBoxes];
	char next[kQueueLineChars + 1] = {};
	bool operator==(const ArcPage&) const = default;
};

void composeArcPage(const ArcMapInput& in, ArcPage& out);
constexpr const char* kNoChangePlanned = "NO CHANGE PLANNED";

} // namespace deluge::generative::perform
