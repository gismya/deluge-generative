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

// One phrase of a piece as the SONG page's Arc map shows it (spec §2.8, §13.5): what the Arc plans for a phrase
// ahead (Generator::planArc), or what was heard in one behind (the Perform driver's PhraseHistory).

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative {

struct ArcPhrase {
	/// The Section playing the phrase's last step, so a short Build shows in the phrase it ends.
	SectionKind kind = SectionKind::Intro;
	/// An Arc Drop lands on the phrase's first step.
	bool drop = false;
	/// Planned only: the piece has stopped before this phrase (the Outro ended), so nothing plays in it.
	bool ended = false;
	/// Planned only: a transition's new roster plays this phrase (from its start step), which the plan can't know
	/// (spec §2.8, §12.9).
	bool next = false;
	/// Bit p: part p sounds in the phrase. Planned: the Arc lets it play (its gate is open and no section rule keeps it
	/// out) and it is audible (not muted, not soloed out), on some step. Heard: it played a note.
	uint8_t sounds = 0;
	/// Bit p: part p is muted at the phrase's end. Planned only.
	uint8_t muted = 0;
	/// Effective Energy against the Recipe centre, in macro units, averaged over the phrase's steps.
	int16_t energy = 0;
};

/// The Arc plan (spec §2.8): the phrases from the one the last decided step is in, as the Arc will play them with the
/// pending list landed and the Recipe as it is now.
struct ArcPlan {
	static constexpr int32_t kMaxPhrases = 16;
	/// The first phrase's index, counted in phrases from step 0.
	int32_t first = 0;
	int32_t count = 0;
	ArcPhrase phrase[kMaxPhrases];
	/// The planned phrase with index `index`, or nullptr outside the plan.
	[[nodiscard]] const ArcPhrase* at(int32_t index) const {
		const int32_t i = index - first;
		return i >= 0 && i < count ? &phrase[i] : nullptr;
	}
};

} // namespace deluge::generative
