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

// Perform's OLED (spec §13.5) as a portable screen: the status line, and below it a SELECT-turn peek (QUEUE or
// RECIPE), or the page that follows the grid page (SONG: ARC, CLIP: PART, KEYBOARD: MACROS), or the Recipe while
// stopped. The Perform view draws the screen's text, bars and boxes as pixels and applies the blink phase; this
// decides what they say. The harness has no l10n, so the words are here, as queue_text's are. The test port's
// gen_arcpage and gen_macropage texts are made from the same screen.

#pragma once

#include "generative/core/generator.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/perform/arc_map.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/grid_page.h"
#include "generative/perform/queue_text.h"
#include <cstdint>
#include <span>

namespace deluge::generative::perform {

/// The OLED's width, for the phrase bar under the status line.
constexpr int32_t kOledWidthPixels = 128;

/// The status line's words (spec §13.5), as the stock strings the view used to read rendered them in capitals.
constexpr const char* kStatusStopped = "STOPPED";
constexpr const char* kStatusStarting = "STARTING";
constexpr const char* kStatusHeld = "HELD";
/// RECIPE's and the stopped screen's first line when the Recipe has no genre.
constexpr const char* kNoGenreName = "Perform";
/// Each macro's initial on the MACROS page, indexed by Macro.
constexpr char kMacroInitials[kNumMacros + 1] = "EDCM";

/// The SELECT-turn peek below the status line (spec §13.5).
enum class OledPeek : uint8_t { None, Queue, Recipe };

/// The last Section event played, as the driver's sink keeps it.
struct HeardSection {
	bool known = false;
	SectionKind kind = SectionKind::Intro;
	uint16_t occurrence = 0;
	int32_t tick = 0; ///< Where it started, in Generator ticks.
};

/// The heard Section's length in phrases (0 for a Groove, which has none), for the status line's count. Kept between
/// frames: the length is read while the decided side is still in the Section, and it runs 12 ticks ahead of what is
/// heard.
struct SectionCount {
	int32_t section = -1;
	uint16_t occurrence = 0;
	int32_t phrases = 0;
};

/// What the PART page's top line reads: the CLIP page's part.
struct PartTitleInput {
	/// The last part held or locked, or -1 before any. The line is empty unless it is in the roster.
	int32_t part = -1;
	/// Parts in the roster, and the roster's genre.
	int32_t parts = 0;
	const Genre* genre = nullptr;
	/// The part's preset as the Recipe's sound picks hold it: a path on the card, "SYNTHS/Bass/165 Acid Arp.XML".
	const char* preset = "";
	TakeRow takes;
	bool frozen = false;
};

/// What the OLED reads, from task context between ticks (spec §2.8).
struct OledInput {
	/// The piece playing, or nullptr while stopped.
	const Generator* piece = nullptr;
	/// The Current Recipe: RECIPE's lines, the macros' centres and QUEUE's part names.
	const Recipe* recipe = nullptr;
	HeardSection heard;
	/// Generator ticks since step 0's grid line, as heard; negative before it.
	int32_t clock = 0;
	int32_t phraseSteps = 8 * kStepsPerBar;
	/// An external clock drives playback.
	bool ext = false;
	/// The pending list, soonest first (queuedByTime()).
	std::span<const QueuedAction> queued;
	OledPeek peek = OledPeek::None;
	/// The grid page: with no peek, the OLED's page follows it.
	GridPage page = GridPage::Song;
	/// PART, read on the CLIP page: the part's title, and its six controls as last read, indexed by SoundControl.
	PartTitleInput title;
	ControlLevel levels[kNumSoundControls];
	/// ARC, read on the SONG page.
	ArcMapInput arc;
};

/// What the OLED shows, as text and numbers: the view redraws only when it changes.
struct OledScreen {
	char left[24] = {};  ///< Section and count, HELD; or STOPPED
	char right[12] = {}; ///< the next queued action's tag, or bar.beat
	bool tag = false;    ///< `right` is a tag: drawn inverted
	bool ext = false;
	int16_t phraseBar = 0; ///< pixels
	static constexpr int32_t kLines = 3;
	char lines[kLines][24] = {};
	char lineRight[kLines][12] = {};
	/// The ARC page instead of the lines, while playing with no peek (spec §13.5).
	bool arc = false;
	ArcPage arcPage;
	/// The PART page instead: the CLIP page's part's sound and take, and its six controls as bars, home filled and
	/// live ticked.
	bool part = false;
	char sound[22] = {}; ///< the part's name and its preset's
	char take[kTakeLabelChars + 1] = {};
	ControlLevel controls[kNumSoundControls];
	/// The MACROS page instead, with the KEYBOARD page (spec §13.5): each macro's centre filled, and a tick where
	/// the Arc has it (-1 when stopped), in macro units.
	bool macros = false;
	int16_t macroCentre[kNumMacros] = {};
	int16_t macroArc[kNumMacros] = {};
	bool operator==(const OledScreen&) const = default;
};

/// Composes the screen. `count` is the view's, kept from frame to frame.
void composeOled(const OledInput& in, SectionCount& count, OledScreen& out);

/// The longest gen_arcpage text: three characters a box, '|', the next change.
constexpr int32_t kArcPageTextChars = 3 * ArcPage::kBoxes + 1 + kQueueLineChars;
/// The test port's gen_arcpage text, the ARC page without its blink phase: each box's tag (-- once stopped, .. with
/// nothing known), * on a Drop, then the next change. "GR BR*BU -- -- -- -- -- |NEXT BREAK 6 BARS".
void describeArcPage(const ArcPage& page, char (&out)[kArcPageTextChars + 1]);

constexpr int32_t kMacrosPageTextChars = 47;
/// The test port's gen_macropage text, the MACROS page in points: each macro's centre, then where the Arc has it (-
/// when stopped). "25/27 25/25 21/- 25/-".
void describeMacrosPage(const OledScreen& screen, char (&out)[kMacrosPageTextChars + 1]);

} // namespace deluge::generative::perform
