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

// The SONG page's played columns (spec §2.8, §13.5): a small per-phrase ring the Perform driver keeps from the events
// it sent, with the notes still ringing, and each part's last note, for the now column's live flash.

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

	constexpr PhraseHistory() {
		for (int32_t& tick : lastNote_) {
			tick = INT32_MIN;
		}
	}

	/// Forgets everything: a piece starts, `phraseSteps` long phrases.
	/// `playing` is the Section playing before step 0: a transition's outgoing one, so a new roster that starts on a
	/// Drop shows one.
	void begin(int32_t phraseSteps, SectionKind playing = SectionKind::Intro);
	/// An event the driver sent (muted note-ons never are), in tick order but for a sample Riser's note-on: held back
	/// (spec §9.5), it can come a few ticks behind events already played, and still counts from its own tick. A
	/// Section event sets its phrase's Section, a note-on marks its part as sounded there, and a part sounds in every
	/// phrase its note rings into, up to the note's note-off. A note-off ends only a note-on of the same key that was
	/// sent, so a muted note's note-off ends nothing.
	void played(const Event& event);
	/// A step was decided, with Energy playing `energy` against the Recipe centre (macro units). Every event before
	/// the step's decision tick has been played, so a note still ringing there sounds in the phrases up to it.
	void decided(int32_t step, int32_t energy);

	/// The phrase with index `phrase` (counted from step 0) as heard so far. false if it isn't kept, or nothing of it
	/// has played or been decided yet.
	bool at(int32_t phrase, ArcPhrase& out) const;
	/// The Section heard last.
	[[nodiscard]] SectionKind playing() const { return playing_; }
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
	/// The parts with a note ringing sound in every phrase from `ringingFrom_` up to `tick` (not included).
	void markRingingUpTo(int32_t tick);
	/// Notes key `key` of part `part` as ringing, or as ended.
	void setRinging(int32_t part, int32_t key, bool ringing);

	/// Keys per part: Event::noteKey(), a MIDI pitch or a lane. A pitch past 127 (none is sent) wraps.
	static constexpr int32_t kKeys = 128;
	static_assert(kNumLanes <= kKeys);
	static constexpr int32_t kKeyWords = kKeys / 32;

	Slot slots_[kPhrases];
	int32_t phraseSteps_ = 8 * kStepsPerBar;
	/// The Section last heard, which a phrase without a Section event plays on in.
	SectionKind playing_ = SectionKind::Intro;
	int32_t lastNote_[kMaxParts] = {};
	/// Bit k of part p: a note of key k that was sent rings.
	uint32_t ringing_[kMaxParts][kKeyWords] = {};
	/// Bit p: part p has a note ringing.
	uint8_t ringingParts_ = 0;
	/// The tick up to which ringing notes have marked their parts as sounded.
	int32_t ringingFrom_ = 0;
};

} // namespace deluge::generative::perform
