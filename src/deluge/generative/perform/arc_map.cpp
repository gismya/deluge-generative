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

#include "generative/perform/arc_map.h"
#include "generative/core/recipe.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

/// The phrase being heard.
int32_t nowPhrase(const ArcMapInput& in) {
	return in.clock > 0 ? in.clock / (in.phraseSteps * kTicksPerStep) : 0;
}

/// Whether a queued action lands in `phrase` on a Section-row pad (`part` < 0: a force, End or key change) or on
/// part `part`'s row (its mute, solo or take).
bool landsIn(const ArcMapInput& in, int32_t phrase, int32_t part) {
	for (const QueuedAction& action : in.queued) {
		if (action.landsAt / in.phraseSteps != phrase) {
			continue;
		}
		switch (action.command.kind) {
		case CommandKind::SetMute:
		case CommandKind::SetSolo:
		case CommandKind::SetTake:
			if (action.command.target == part) {
				return true;
			}
			break;
		case CommandKind::ForceSection:
		case CommandKind::End:
		case CommandKind::SetKey:
			if (part < 0) {
				return true;
			}
			break;
		default:
			break;
		}
	}
	return false;
}

/// Whether any queued action lands in `phrase`.
bool anyLandsIn(const ArcMapInput& in, int32_t phrase) {
	return std::any_of(in.queued.begin(), in.queued.end(),
	                   [&](const QueuedAction& action) { return action.landsAt / in.phraseSteps == phrase; });
}

/// What the map knows of `phrase`: heard behind now and so far in it, planned ahead. false for nothing (before step
/// 0, older than the history keeps, past the plan, or after the piece stops).
bool known(const ArcMapInput& in, int32_t phrase, int32_t now, ArcPhrase& out) {
	if (phrase < 0) {
		return false;
	}
	if (phrase <= now && in.history->at(phrase, out)) {
		return true;
	}
	if (phrase < now) {
		return false;
	}
	const ArcPhrase* planned = in.plan->at(phrase);
	if (planned == nullptr || planned->ended) {
		return false;
	}
	out = *planned;
	if (phrase == now) {
		out.sounds = 0; // nothing heard of it yet
	}
	return true;
}

/// Appends `text` to the string in `out`, cutting it short when full.
void append(std::span<char> out, const char* text) {
	size_t length = 0;
	while (out[length] != '\0') {
		length++;
	}
	while (*text != '\0' && length + 1 < out.size()) {
		out[length++] = *text++;
	}
	out[length] = '\0';
}

} // namespace

void composeArcMap(const ArcMapInput& in, ArcMap& out) {
	out = ArcMap{};
	if (!in.playing || in.history == nullptr || in.plan == nullptr) {
		return;
	}
	const int32_t now = nowPhrase(in);
	for (int32_t column = 0; column < ArcMap::kColumns; column++) {
		const int32_t phrase = now + in.scroll + column - ArcMap::kNowColumn;
		ArcPhrase info;
		if (!known(in, phrase, now, info)) {
			continue;
		}
		const ArcMapCell::When when = phrase < now    ? ArcMapCell::When::Played
		                              : phrase == now ? ArcMapCell::When::Now
		                                              : ArcMapCell::When::Planned;
		ArcMapCell& section = out.cell[ArcMap::kSectionRow][column];
		section = {.look = ArcMapCell::Look::Section, .when = when, .kind = info.kind, .drop = info.drop};
		section.blink = landsIn(in, phrase, -1);
		out.cell[ArcMap::kEnergyRow][column] = {.look = ArcMapCell::Look::Energy,
		                                        .when = when,
		                                        .kind = info.kind,
		                                        .energy = static_cast<int8_t>(info.energy / kMacroUnitsPerPoint)};

		for (int32_t p = 0; p < std::min<int32_t>(in.parts, kMaxParts); p++) {
			ArcMapCell& cell = out.cell[p][column];
			cell.when = when;
			cell.part = static_cast<uint8_t>(p);
			cell.blink = landsIn(in, phrase, p);
			const bool sounds = (info.sounds & (1U << p)) != 0;
			if (when == ArcMapCell::When::Now && in.focus == p) {
				cell.look = ArcMapCell::Look::Focus;
			}
			else if (sounds) {
				cell.look = ArcMapCell::Look::Part;
				const int32_t last = in.history->lastNote(p);
				cell.hit = when == ArcMapCell::When::Now && last >= in.hitFrom && last <= in.hitTo;
			}
			else if (when == ArcMapCell::When::Planned && (info.muted & (1U << p)) != 0) {
				cell.look = ArcMapCell::Look::Muted;
			}
		}
	}
}

ArcMapRange arcMapScrollRange(const ArcMapInput& in) {
	ArcMapRange range;
	if (!in.playing || in.plan == nullptr) {
		return range;
	}
	const int32_t now = nowPhrase(in);
	// Column 1 shows now + scroll - kNowColumn, column 16 now + scroll + the columns right of now.
	const int32_t oldest = std::max<int32_t>(0, now - (PhraseHistory::kPhrases - 1));
	range.back = std::min<int32_t>(0, oldest - now + ArcMap::kNowColumn);
	int32_t last = in.plan->first + in.plan->count - 1;
	while (last > now && in.plan->at(last) != nullptr && in.plan->at(last)->ended) {
		last--;
	}
	range.ahead = std::max<int32_t>(0, last - now - (ArcMap::kColumns - 1 - ArcMap::kNowColumn));
	return range;
}

void composeArcPage(const ArcMapInput& in, ArcPage& out) {
	out = ArcPage{};
	if (!in.playing || in.history == nullptr || in.plan == nullptr) {
		return;
	}
	const int32_t now = nowPhrase(in);
	SectionKind playing = SectionKind::Intro;
	for (int32_t i = 0; i < ArcPage::kBoxes; i++) {
		ArcBox& box = out.box[i];
		ArcPhrase info;
		box.now = i == 0;
		if (!known(in, now + i, now, info)) {
			box.ended = in.plan->at(now + i) != nullptr;
			continue;
		}
		const char* name = sectionName(info.kind);
		box.tag[0] = name[0];
		box.tag[1] = name[1];
		box.drop = info.drop;
		box.blink = anyLandsIn(in, now + i);
		playing = i == 0 ? info.kind : playing;
	}

	// The Arc's next change: a Section of another kind, a Drop, or the line where the piece stops.
	for (int32_t phrase = now + 1; in.plan->at(phrase) != nullptr; phrase++) {
		const ArcPhrase& planned = *in.plan->at(phrase);
		const char* what = planned.ended             ? nullptr
		                   : planned.drop            ? "DROP"
		                   : planned.kind != playing ? sectionName(planned.kind)
		                                             : "";
		if (what != nullptr && what[0] == '\0') {
			continue;
		}
		char when[12];
		countdown(stepTick(phrase * in.phraseSteps) - in.clock, true, when);
		append(out.next, what == nullptr ? "STOP" : "NEXT ");
		if (what != nullptr) {
			append(out.next, what);
		}
		append(out.next, " ");
		append(out.next, when);
		return;
	}
	append(out.next, kNoChangePlanned);
}

} // namespace deluge::generative::perform
