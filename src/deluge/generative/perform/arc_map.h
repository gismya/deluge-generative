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
// line. The Perform view turns the cells into colours and pixels; this decides what each one shows, and keeps what
// the page holds between frames (ArcMapState): the plan, the X scroll and the now column's flash window. The test
// port's gen_arcmap and gen_arcrow texts are made from the same cells.

#pragma once

#include "generative/core/arc_plan.h"
#include "generative/perform/phrase_history.h"
#include "generative/perform/queue_text.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// What the SONG page reads, from task context between ticks (spec §2.8).
struct ArcMapInput {
	/// The piece playing, or nullptr while stopped: its genre gives the phrase length and the roster.
	const Generator* piece = nullptr;
	/// Generator ticks since step 0's grid line, as heard, never negative: the now column is the phrase it is in.
	int32_t clock = 0;
	/// The part on the gold knobs, or -1.
	int8_t focus = -1;
	const PhraseHistory* history = nullptr;
};

/// How far X scrolls: back until the oldest phrase kept is in column 1 (one fewer than the ring holds, since the
/// next phrase's decisions take the oldest slot just before its line), ahead until the plan's last phrase is in
/// column 16. Never past now in either direction.
struct ArcMapRange {
	int32_t back = 0;
	int32_t ahead = 0;
};

/// What the SONG page keeps from frame to frame: the Arc plan and the queue as last worked out, how far X has
/// scrolled the map, and the now column's flash window. The Perform view holds one and asks it; it decides none of
/// these itself.
class ArcMapState {
public:
	/// Works the Arc plan and the queue out again if anything they depend on changed since the last time (spec
	/// §2.8): a phrase line passed on the decided side, a new roster plays (`rosters` counts them: its phrases count
	/// from 0 again), or something queued, held, muted or soloed changed. Stopped (`piece` nullptr), the plan and the
	/// queue are empty.
	/// @return Whether it planned a piece's Arc (Generator::planArc(), the costly part).
	bool refreshPlan(const Generator* piece, uint32_t rosters);
	/// Forgets what the plan was worked out from, so the next refreshPlan() works it out whatever plays.
	void forgetPlan() { key_ = PlanKey{.decidedPhrase = -2}; } // never a key refreshPlan() makes
	[[nodiscard]] const ArcPlan& plan() const { return plan_; }
	/// The pending list, soonest first: the status line, QUEUE and the map's blinks share it.
	[[nodiscard]] std::span<const QueuedAction> queued() const { return {queued_, static_cast<size_t>(queuedCount_)}; }

	/// Every frame, on every page: samples the flash window when the heard step changes (`clock` as
	/// ArcMapInput::clock). The window is the step before and this one so far, so a hit flashes for two 16ths and
	/// redraws the pads at most once per 16th (spec §13.5).
	void sampleFlash(int32_t clock);
	/// Every frame, on every page: keeps the scroll inside what is known. The plan can get shorter (End queued) and
	/// the history moves on at each phrase line. @return Whether the scroll moved.
	bool clampScroll(const ArcMapInput& in) { return scrollBy(in, 0); }
	/// How far X scrolls now.
	[[nodiscard]] ArcMapRange scrollRange(const ArcMapInput& in) const;
	/// X turned by `offset` columns: negative = back over what played (spec §13.5), no further than scrollRange()
	/// either way. @return Whether the scroll moved.
	bool scrollBy(const ArcMapInput& in, int32_t offset);
	/// Back to now. @return Whether the scroll moved.
	bool scrollToNow();
	/// Columns the map is scrolled by.
	[[nodiscard]] int32_t scroll() const { return scroll_; }
	/// Whether a part whose last note-on was at `tick` (Generator ticks) flashes in the now column.
	[[nodiscard]] bool flashes(int32_t tick) const { return tick >= hitFrom_ && tick <= hitTo_; }

private:
	int32_t scroll_ = 0;
	/// The flash window: the step it was sampled in, and the ticks a note-on must fall in to light its part.
	int32_t hitStep_ = -1;
	int32_t hitFrom_ = 0;
	int32_t hitTo_ = -1;
	ArcPlan plan_;
	/// What the plan was worked out from: whether a piece plays, the rosters started, the phrase of its last decided
	/// step, and a hash of its steering state (the pending list, the hold, mutes and solos).
	struct PlanKey {
		bool playing = false;
		uint32_t rosters = 0;
		int32_t decidedPhrase = -1;
		uint32_t steering = 0;
		bool operator==(const PlanKey&) const = default;
	};
	PlanKey key_;
	QueuedAction queued_[PendingList::kCapacity];
	int32_t queuedCount_ = 0;
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

void composeArcMap(const ArcMapInput& in, const ArcMapState& state, ArcMap& out);

/// The test port's gen_arcmap text, one character per column: the Section row (I G B U O, D for a Drop, . for
/// nothing), then where queued actions blink (x). "s=.GGGBBUG........ q=....x..........."
constexpr int32_t kArcMapTextChars = 2 * ArcMap::kColumns + 5;
void describeArcMapSections(const ArcMap& map, char (&out)[kArcMapTextChars + 1]);
/// The test port's gen_arcrow text for the part on `row`, one character per column: . off, h heard, n now, p
/// planned, m muted, f focused.
void describeArcMapPart(const ArcMap& map, int32_t row, char (&out)[ArcMap::kColumns + 1]);

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

void composeArcPage(const ArcMapInput& in, const ArcMapState& state, ArcPage& out);
constexpr const char* kNoChangePlanned = "NO CHANGE PLANNED";

} // namespace deluge::generative::perform
