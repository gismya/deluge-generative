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

// The SONG page's played columns (spec §2.8): a small per-phrase ring the Perform driver keeps from the events it
// sent, and each part's last note, for the now column's live flash.

#pragma once

#include "generative/core/arc_plan.h"
#include "generative/core/event.h"
#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative::perform {

class PhraseHistory {
public:
	/// Phrases kept, the newest included.
	static constexpr int32_t kPhrases = 16;

	/// Forgets everything: a piece starts, `phraseSteps` long phrases.
	void begin(int32_t phraseSteps);
	/// An event the driver sent (muted note-ons never are): a Section event sets its phrase's Section, a note-on marks
	/// its part as sounded there.
	void played(const Event& event);
	/// A step was decided, with Energy playing `energy` against the Recipe centre (macro units).
	void decided(int32_t step, int32_t energy);

	/// The phrase with index `phrase` (counted from step 0) as heard so far. false if it isn't kept, or nothing of it
	/// has played or been decided yet.
	bool at(int32_t phrase, ArcPhrase& out) const;
	/// The tick of part `part`'s last note-on sent, or INT32_MIN.
	[[nodiscard]] int32_t lastNote(int32_t part) const { return lastNote_[part]; }

private:
	struct Slot {
		int32_t phrase = -1;
		ArcPhrase heard;
		int32_t energySum = 0;
		int32_t decided = 0;
	};
	/// The slot for `phrase`, reset if it held an older one.
	Slot& slot(int32_t phrase);

	Slot slots_[kPhrases];
	int32_t phraseSteps_ = 8 * kStepsPerBar;
	/// The Section last heard, which a phrase without a Section event plays on in.
	SectionKind playing_ = SectionKind::Intro;
	int32_t lastNote_[kMaxParts] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
};

} // namespace deluge::generative::perform
