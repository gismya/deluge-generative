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

#include "generative/perform/phrase_history.h"

namespace deluge::generative::perform {

void PhraseHistory::begin(int32_t phraseSteps) {
	*this = PhraseHistory{};
	phraseSteps_ = phraseSteps;
}

PhraseHistory::Slot& PhraseHistory::slot(int32_t phrase) {
	Slot& slot = slots_[phrase % kPhrases];
	if (slot.phrase < phrase) {
		slot = Slot{};
		slot.phrase = phrase;
		slot.heard.kind = playing_;
	}
	return slot;
}

void PhraseHistory::played(const Event& event) {
	const int32_t phrase = event.tick / (phraseSteps_ * kTicksPerStep);
	if (event.tick < 0) {
		return;
	}
	switch (event.kind) {
	case EventKind::Section: {
		Slot& at = slot(phrase);
		// A Groove that a Build lands on is a Drop (spec §9.1). It starts on a phrase line.
		at.heard.drop = at.heard.drop || (event.section == SectionKind::Groove && playing_ == SectionKind::Build);
		at.heard.kind = event.section;
		playing_ = event.section;
		break;
	}
	case EventKind::NoteOn:
		if (event.part < kMaxParts) {
			slot(phrase).heard.sounds |= static_cast<uint8_t>(1U << event.part);
			lastNote_[event.part] = event.tick;
		}
		break;
	default:
		break;
	}
}

void PhraseHistory::decided(int32_t step, int32_t energy) {
	if (step < 0) {
		return;
	}
	Slot& at = slot(step / phraseSteps_);
	at.energySum += energy;
	at.decided++;
}

bool PhraseHistory::at(int32_t phrase, ArcPhrase& out) const {
	if (phrase < 0) {
		return false;
	}
	const Slot& slot = slots_[phrase % kPhrases];
	if (slot.phrase != phrase) {
		return false;
	}
	out = slot.heard;
	out.energy = static_cast<int16_t>(slot.decided > 0 ? slot.energySum / slot.decided : 0);
	return true;
}

} // namespace deluge::generative::perform
