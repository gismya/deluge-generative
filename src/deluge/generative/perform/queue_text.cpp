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

#include "generative/perform/queue_text.h"
#include "generative/perform/text.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

constexpr const char* kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
/// Indexed by Scale.
constexpr const char* kScaleNames[kNumScales] = {"MAJOR",      "MINOR",      "DORIAN",  "PHRYGIAN",
                                                 "LYDIAN",     "MIXOLYDIAN", "LOCRIAN", "MEL MINOR",
                                                 "HARM MINOR", "HUNG MINOR", "MARVA",   "ARABIAN"};
/// Indexed by SectionKind.
constexpr const char* kSectionTags[kNumSectionKinds] = {"IN", "GR", "BR", "BU", "OU"};

const char* partName(const Genre& genre, uint8_t part) {
	return part < partCount(genre) ? genre.parts[part].name : "?";
}

} // namespace

const char* keyName(uint8_t root) {
	return kKeyNames[root % 12];
}

const char* scaleName(Scale scale) {
	const auto index = static_cast<int32_t>(scale);
	return index < kNumScales ? kScaleNames[index] : "?";
}

int32_t queuedByTime(const Generator& generator, std::span<QueuedAction> out) {
	int32_t count = 0;
	for (int32_t i = 0; i < generator.queuedCount() && count < static_cast<int32_t>(out.size()); i++) {
		QueuedAction action{generator.queued(i), generator.landsAt(generator.queued(i))};
		// Insertion keeps the pending list's age order among actions landing on the same step.
		int32_t at = count;
		while (at > 0 && out[at - 1].landsAt > action.landsAt) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = action;
		count++;
	}
	return count;
}

const char* queueTag(const Command& command) {
	switch (command.kind) {
	case CommandKind::SetMute:
		return "MUT";
	case CommandKind::SetSolo:
		return "SOL";
	case CommandKind::SetKey:
		return "KEY";
	case CommandKind::SetTake:
		return "TAK";
	case CommandKind::SetSound:
		return "SND";
	case CommandKind::End:
		return "END";
	case CommandKind::ForceSection:
		return command.target < kNumSectionKinds ? kSectionTags[command.target] : "??";
	default:
		return "??";
	}
}

void queueLabel(const Command& command, const Genre& genre, std::span<char> out) {
	Text text(out);
	switch (command.kind) {
	case CommandKind::SetMute:
		text.add(partName(genre, command.target)).add(command.value != 0 ? " MUTE" : " ON");
		break;
	case CommandKind::SetSolo:
		text.add(partName(genre, command.target)).add(command.value != 0 ? " SOLO" : " UNSOLO");
		break;
	case CommandKind::SetTake:
		text.add(partName(genre, command.target)).add(" TAKE ").add(command.value + 1);
		break;
	case CommandKind::SetSound:
		text.add(partName(genre, command.target)).add(" SOUND");
		break;
	case CommandKind::SetKey:
		text.add("KEY ").add(keyName(command.target)).add(" ");
		text.add(command.value >= 0 ? scaleName(static_cast<Scale>(command.value)) : "?");
		break;
	case CommandKind::ForceSection:
		text.add(command.target < kNumSectionKinds ? sectionName(static_cast<SectionKind>(command.target)) : "?");
		break;
	case CommandKind::End:
		text.add(sectionName(SectionKind::Outro));
		break;
	default:
		text.add("?");
		break;
	}
}

void countdown(int32_t ticks, bool words, std::span<char> out) {
	Text text(out);
	if (ticks >= kTicksPerBar) {
		const int32_t bars = (ticks + kTicksPerBar - 1) / kTicksPerBar;
		text.add(bars);
		if (words) {
			text.add(bars > 1 ? " BARS" : " BAR");
		}
		return;
	}
	const int32_t beats = ticks > 0 ? (ticks + kTicksPerBeat - 1) / kTicksPerBeat : 1;
	text.add(beats);
	text.add(words ? (beats > 1 ? " BEATS" : " BEAT") : "B");
}

void queueLines(std::span<const QueuedAction> actions, int32_t clock, const Genre& genre, QueueLines& out) {
	out = QueueLines{};
	const auto count = static_cast<int32_t>(actions.size());
	if (count == 0) {
		Text(out.label[0]).add(kNothingQueued);
		return;
	}
	// Every action if they fit; otherwise one line fewer, and the last says how many more.
	const int32_t shown = count <= QueueLines::kLines ? count : QueueLines::kLines - 1;
	for (int32_t i = 0; i < shown; i++) {
		countdown(stepTick(actions[i].landsAt) - clock, true, out.when[i]);
		// The label takes what the countdown and a space leave of the line.
		size_t when = 0;
		while (out.when[i][when] != '\0') {
			when++;
		}
		const size_t room = kQueueLineChars - when - 1;
		queueLabel(actions[i].command, genre, std::span(out.label[i], std::min(room + 1, sizeof(out.label[i]))));
	}
	if (shown < count) {
		Text(out.label[shown]).add("+").add(count - shown).add(" MORE");
	}
}

void statusTag(std::span<const QueuedAction> actions, int32_t clock, std::span<char> out) {
	out[0] = '\0';
	if (actions.empty()) {
		return;
	}
	char count[8];
	countdown(stepTick(actions[0].landsAt) - clock, false, count);
	Text(out).add(">").add(queueTag(actions[0].command)).add(count).add(actions.size() > 1 ? "+" : "");
}

} // namespace deluge::generative::perform
