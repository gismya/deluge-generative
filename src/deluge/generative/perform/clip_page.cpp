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

#include "generative/perform/clip_page.h"
#include "generative/perform/text.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

using Look = ClipCell::Look;

/// The highest take made, the one waiting included.
int32_t topTake(const TakeRow& row) {
	return std::max({row.newest, row.playing, row.waiting});
}

/// How take pad `take` (0–15) looks. A waiting take counts as made.
Look takeLook(const TakeRow& row, int32_t take) {
	if (take == row.waiting && row.waiting != row.playing) {
		return Look::TakeQueued;
	}
	if (take == row.playing) {
		return Look::TakePlaying;
	}
	const int32_t top = topTake(row);
	return take <= top ? Look::TakeEarlier : take == top + 1 ? Look::TakeNew : Look::Off;
}

/// From spec row 3 down.
constexpr SoundControl kFaderControls[] = {SoundControl::Brightness, SoundControl::Thin,  SoundControl::Decay,
                                           SoundControl::Space,      SoundControl::Drive, SoundControl::Wobble};
static_assert(sizeof(kFaderControls) / sizeof(kFaderControls[0]) == kNumSoundControls);
static_assert(kFirstFaderRow + kNumSoundControls == ClipPage::kRows);
static_assert(kMaxTakes <= ClipPage::kColumns && kFaderPads == ClipPage::kColumns);

constexpr int32_t kLastPad = kFaderPads - 1;

/// The pad (0–15) that shows a knob position (-64..64, as stock's gold knobs read a param), to the nearest pad. A
/// position outside the knob's range stays on the fader, so describeFaders() reads back from the cells what was put
/// there.
int32_t faderPad(int32_t knobPos) {
	return std::clamp<int32_t>(((knobPos + kKnobSpan / 2) * kLastPad * 2 + kKnobSpan) / (kKnobSpan * 2), 0, kLastPad);
}

} // namespace

SoundControl faderControl(int32_t row) {
	return kFaderControls[row - kFirstFaderRow];
}

int32_t faderKnobPos(int32_t pad) {
	return (pad * kKnobSpan * 2 + kLastPad) / (kLastPad * 2) - kKnobSpan / 2;
}

void composeClipPage(const ClipPageInput& in, ClipPage& out) {
	out = {};
	out.part = static_cast<int8_t>(in.part);
	out.shown = in.part >= 0 && in.part < in.parts;
	if (!out.shown) {
		return;
	}
	for (int32_t take = 0; take < kMaxTakes; take++) {
		out.cell[kTakesRow][take].look = takeLook(in.takes, take);
	}
	ClipCell* actions = out.cell[kActionsRow];
	actions[kFreezePad] = {.look = Look::Freeze, .lit = in.frozen};
	actions[kNewSoundPad].look = in.sound == SoundSwap::Loading   ? Look::SoundLoading
	                             : in.sound == SoundSwap::Waiting ? Look::SoundWaiting
	                                                              : Look::NewSound;
	actions[kSoundEditorPad].look = Look::SoundEditor;
	for (int32_t row = kFirstFaderRow; row < ClipPage::kRows; row++) {
		const ControlLevel& level = in.levels[static_cast<int32_t>(faderControl(row))];
		if (!level.has) {
			continue; // the part's preset doesn't use the control: dark
		}
		const int32_t home = faderPad(level.home);
		const int32_t live = faderPad(level.live);
		for (int32_t pad = 0; pad < kFaderPads; pad++) {
			out.cell[row][pad] = {.look = pad == live ? Look::FaderLive : Look::Fader, .lit = pad <= home};
		}
	}
}

void describeClip(const ClipPage& page, char (&out)[kClipTextChars + 1]) {
	Text text(out);
	text.add("part=").add(page.part).add(" takes=");
	for (int32_t take = 0; take < kMaxTakes; take++) {
		constexpr const char* kLetters[] = {".", "P", "e", "n", "q"}; // Look order, Off to TakeQueued: a take's looks
		static_assert(sizeof(kLetters) / sizeof(kLetters[0]) == static_cast<int32_t>(Look::TakeQueued) + 1);
		text.add(kLetters[static_cast<int32_t>(page.cell[kTakesRow][take].look)]);
	}
	const ClipCell* actions = page.cell[kActionsRow];
	text.add(" freeze=").add(actions[kFreezePad].lit ? "1" : "0");
	const Look sound = actions[kNewSoundPad].look;
	text.add(" sound=").add(sound == Look::SoundLoading ? "p" : sound == Look::SoundWaiting ? "w" : "-");
}

void describeFaders(const ClipPage& page, char (&out)[kClipTextChars + 1]) {
	// Each fader's home and live pads, read back from its cells in one pass; -1 where the part has no such control.
	int32_t home[kNumSoundControls];
	int32_t live[kNumSoundControls];
	bool atHome = true;
	for (int32_t fader = 0; fader < kNumSoundControls; fader++) {
		const ClipCell* row = page.cell[kFirstFaderRow + fader];
		home[fader] = -1;
		live[fader] = -1;
		for (int32_t pad = 0; pad < kFaderPads && row[0].look != Look::Off; pad++) {
			home[fader] = row[pad].lit ? pad : home[fader];
			live[fader] = row[pad].look == Look::FaderLive ? pad : live[fader];
		}
		atHome = atHome && live[fader] == home[fader];
	}
	Text text(out);
	text.add("part=").add(page.part).add(atHome ? " at_home" : "");
	for (int32_t fader = 0; fader < kNumSoundControls; fader++) {
		if (live[fader] < 0) {
			text.add(" -");
		}
		else if (live[fader] == home[fader]) {
			text.add(" ").add(home[fader] + 1).add("=");
		}
		else {
			text.add(" ").add(home[fader] + 1).add("/").add(live[fader] + 1);
		}
	}
}

bool takeTappable(const TakeRow& row, int32_t take) {
	return take >= 0 && take < kMaxTakes && takeLook(row, take) != Look::Off;
}

int32_t steppedTake(const TakeRow& row, int32_t direction) {
	const int32_t from = row.waiting >= 0 ? row.waiting : row.playing;
	const int32_t to = from + (direction < 0 ? -1 : 1);
	// Forward never skips past the next new take, which is one past the newest.
	return to >= 0 && to < kMaxTakes ? to : -1;
}

void takeLabel(const TakeRow& row, bool frozen, char (&out)[kTakeLabelChars + 1]) {
	Text text(out);
	text.add("T").add(row.playing + 1).add("/").add(topTake(row) + 1).add(frozen ? "*" : "");
}

void seedCode(uint32_t seed, char (&out)[kSeedCodeChars + 1]) {
	constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	for (int32_t i = kSeedCodeChars - 1; i >= 0; i--) {
		out[i] = kAlphabet[seed & 31U];
		seed >>= 5;
	}
	out[kSeedCodeChars] = '\0';
}

} // namespace deluge::generative::perform
