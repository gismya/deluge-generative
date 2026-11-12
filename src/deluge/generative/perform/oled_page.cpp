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

#include "generative/perform/oled_page.h"
#include "generative/perform/text.h"
#include <algorithm>
#include <cstring>

namespace deluge::generative::perform {

namespace {

static_assert(QueueLines::kLines == OledScreen::kLines);

int32_t length(const char* text) {
	return static_cast<int32_t>(strlen(text));
}

/// The status line (spec §13.5). Left: the Section and its phrase count, then HELD; STOPPED when stopped. Right: the
/// next queued action as a tag, or bar.beat in the phrase. Under it, the phrase bar.
void composeStatus(const OledInput& in, SectionCount& count, OledScreen& out) {
	const Generator* piece = in.piece;
	const int32_t phraseTicks = in.phraseSteps * kTicksPerStep;
	const int32_t clock = std::max<int32_t>(in.clock, 0);

	Text left(out.left);
	bool held = false;
	if (piece == nullptr) {
		left.add(kStatusStopped);
	}
	else if (!in.heard.known) {
		left.add(kStatusStarting);
	}
	else {
		// The Section's length, read while the decided side is still in it (it runs 12 ticks ahead).
		const Arc& arc = piece->arc();
		const auto section = static_cast<int32_t>(in.heard.kind);
		if (arc.section() == in.heard.kind && arc.occurrence() == in.heard.occurrence) {
			count.section = section;
			count.occurrence = in.heard.occurrence;
			count.phrases = (arc.sectionSteps() * kTicksPerStep + phraseTicks - 1) / phraseTicks;
		}
		const bool lengthKnown = count.section == section && count.occurrence == in.heard.occurrence;
		const int32_t phrase = std::max<int32_t>(clock - in.heard.tick, 0) / phraseTicks + 1;
		left.add(sectionName(in.heard.kind)).add(" ");
		if (lengthKnown && count.phrases > 0) {
			left.add(std::min(phrase, count.phrases)).add("/").add(count.phrases);
		}
		else {
			left.add(phrase);
		}
		held = arc.held();
	}

	out.ext = in.ext;
	if (piece != nullptr) {
		statusTag(in.arcState->queued(), in.clock, out.right);
		out.tag = out.right[0] != '\0';
		const int32_t inPhrase = clock % phraseTicks;
		if (!out.tag) {
			Text(out.right).add(inPhrase / kTicksPerBar + 1).add(".").add(inPhrase % kTicksPerBar / kTicksPerBeat + 1);
		}
		out.phraseBar = static_cast<int16_t>(kOledWidthPixels * inPhrase / phraseTicks);
	}
	if (held) {
		// HELD in full if it fits beside the right side, or else H, or nothing rather than overlap it.
		const int32_t rightChars = length(out.right) + (out.ext ? 4 : 0) + 1;
		const int32_t room = kQueueLineChars - rightChars - length(out.left);
		if (1 + length(kStatusHeld) <= room) {
			left.add(" ").add(kStatusHeld);
		}
		else if (room >= 2) {
			left.add(" H");
		}
	}
}

/// The PART page's top line: the part's name, then a synth part's preset as its file name ("ACID 165 Acid Arp"), cut
/// to leave the take its room. A Kit part has no one pick (each lane has its own sample, spec §13.5), so it shows
/// its name only.
void composePartTitle(const PartTitleInput& in, OledScreen& out) {
	if (in.part < 0 || in.part >= in.parts || in.genre == nullptr) {
		return;
	}
	const char* slash = strrchr(in.preset, '/');
	const char* name = slash != nullptr ? slash + 1 : in.preset;
	const char* dot = strrchr(name, '.');
	const int32_t nameLength = dot != nullptr ? static_cast<int32_t>(dot - name) : length(name);
	takeLabel(in.takes, in.frozen, out.take);
	const int32_t room =
	    std::min<int32_t>(static_cast<int32_t>(sizeof(out.sound)) - 1, kQueueLineChars - 1 - length(out.take));
	const PartSpec& part = in.genre->parts[in.part];
	Text sound(std::span<char>(out.sound, static_cast<size_t>(room) + 1));
	sound.add(part.name);
	if (part.lanes.empty()) {
		sound.add(" ").add(name, nameLength);
	}
}

const char* genreName(const Recipe& recipe) {
	return recipe.genre != nullptr ? recipe.genre->oledName : kNoGenreName;
}

} // namespace

void composeOled(const OledInput& in, SectionCount& count, OledScreen& out) {
	out = OledScreen{};
	composeStatus(in, count, out);

	// Below it: a peek, or the page following the grid page: SONG → ARC (spec §13.5). Stopped, the Recipe's macros.
	const Generator* piece = in.piece;
	const Recipe& recipe = *in.recipe;
	if (in.peek == OledPeek::Queue) {
		if (piece != nullptr && recipe.genre != nullptr) {
			QueueLines queue;
			queueLines(in.arcState->queued(), in.clock, *recipe.genre, queue);
			for (int32_t i = 0; i < OledScreen::kLines; i++) {
				Text(out.lines[i]).add(queue.label[i]);
				Text(out.lineRight[i]).add(queue.when[i]);
			}
		}
		else {
			Text(out.lines[0]).add(kNothingQueued);
		}
	}
	else if (in.peek == OledPeek::Recipe) {
		Text(out.lines[0]).add(genreName(recipe));
		Text(out.lines[1]).add(recipe.tempo / 100).add(" BPM SW ").add(recipe.swing);
		Text(out.lines[2]).add(keyName(recipe.keyRoot)).add(" ").add(scaleName(recipe.scale));
	}
	else if (in.page == GridPage::Clip) {
		// PART: the CLIP page's part's sound, take and six controls (spec §13.5).
		out.part = true;
		std::copy_n(in.levels, kNumSoundControls, out.controls);
		composePartTitle(in.title, out);
	}
	else if (in.page == GridPage::Keyboard) {
		// MACROS: each macro's centre, and where the Arc has it (spec §13.5).
		out.macros = true;
		for (int32_t m = 0; m < kNumMacros; m++) {
			const auto macro = static_cast<Macro>(m);
			out.macroCentre[m] = static_cast<int16_t>(recipe.macros[macro]);
			out.macroArc[m] = static_cast<int16_t>(piece != nullptr ? piece->effectiveMacro(macro) : -1);
		}
	}
	else if (piece != nullptr) {
		out.arc = true;
		composeArcPage(in.arc, *in.arcState, out.arcPage);
	}
	else {
		Text(out.lines[0]).add(genreName(recipe));
		Text line(out.lines[1]);
		for (int32_t m = 0; m < kNumMacros; m++) {
			const char initial[] = {kMacroInitials[m], '\0'};
			line.add(m == 0 ? "" : " ").add(initial).add(macroToPoints(recipe.macros[static_cast<Macro>(m)]));
		}
	}
}

void describeArcPage(const ArcPage& page, char (&out)[kArcPageTextChars + 1]) {
	char* at = out;
	for (const ArcBox& box : page.box) {
		// A box with nothing known (past the plan, not ended) has an empty tag: '.' keeps the text whole.
		*at++ = box.ended ? '-' : box.tag[0] != '\0' ? box.tag[0] : '.';
		*at++ = box.ended ? '-' : box.tag[1] != '\0' ? box.tag[1] : '.';
		*at++ = box.drop ? '*' : ' ';
	}
	Text(std::span<char>(at, out + sizeof(out))).add("|").add(page.next);
}

void describeMacrosPage(const OledScreen& screen, char (&out)[kMacrosPageTextChars + 1]) {
	Text text(out);
	for (int32_t m = 0; m < kNumMacros; m++) {
		text.add(m == 0 ? "" : " ").add(macroToPoints(screen.macroCentre[m])).add("/");
		if (screen.macroArc[m] >= 0) {
			text.add(macroToPoints(screen.macroArc[m]));
		}
		else {
			text.add("-");
		}
	}
}

} // namespace deluge::generative::perform
