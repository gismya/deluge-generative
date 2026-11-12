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
#include "generative/perform/text.h"
#include <algorithm>

namespace deluge::generative::perform {

namespace {

/// A frame's input and state as the map's rules read them.
struct Frame {
	bool playing = false;
	int32_t clock = 0;
	int32_t phraseSteps = 8 * kStepsPerBar;
	/// Parts in the roster playing.
	int32_t parts = 0;
	int32_t scroll = 0;
	const PhraseHistory* history = nullptr;
	const ArcPlan* plan = nullptr;
	std::span<const QueuedAction> queued;
};

Frame frameOf(const ArcMapInput& in, const ArcMapState& state) {
	Frame frame;
	if (in.piece == nullptr || in.history == nullptr) {
		return frame;
	}
	const Genre& genre = *in.piece->recipe().genre;
	frame.playing = true;
	frame.clock = in.clock;
	frame.phraseSteps = genre.phraseSteps();
	frame.parts = partCount(genre);
	frame.scroll = state.scroll();
	frame.history = in.history;
	frame.plan = &state.plan();
	frame.queued = state.queued();
	return frame;
}

/// The phrase being heard.
int32_t nowPhrase(const Frame& in) {
	return in.clock > 0 ? in.clock / (in.phraseSteps * kTicksPerStep) : 0;
}

/// Whether a queued action lands in `phrase` on a Section-row pad (`part` < 0: a force, End or key change) or on
/// part `part`'s row (its mute, solo, take or sound).
bool landsIn(const Frame& in, int32_t phrase, int32_t part) {
	for (const QueuedAction& action : in.queued) {
		if (action.landsAt / in.phraseSteps != phrase) {
			continue;
		}
		switch (action.command.kind) {
		case CommandKind::SetMute:
		case CommandKind::SetSolo:
		case CommandKind::SetTake:
		case CommandKind::SetSound:
			if (action.command.target == part) {
				return true;
			}
			break;
		case CommandKind::ForceSection:
		case CommandKind::End:
		case CommandKind::SetKey:
		case CommandKind::Transition:
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
bool anyLandsIn(const Frame& in, int32_t phrase) {
	return std::any_of(in.queued.begin(), in.queued.end(),
	                   [&](const QueuedAction& action) { return action.landsAt / in.phraseSteps == phrase; });
}

/// What the map knows of `phrase`: heard behind now and so far in it, planned ahead. false for nothing (before step
/// 0, older than the history keeps, past the plan, or after the piece stops).
bool known(const Frame& in, int32_t phrase, int32_t now, ArcPhrase& out) {
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
	if (planned == nullptr || planned->ended || planned->next) {
		return false;
	}
	out = *planned;
	if (phrase == now) {
		out.sounds = 0; // nothing heard of it yet
	}
	return true;
}

/// How far X scrolls in a frame (arcMapScrollRange()).
ArcMapRange scrollRange(const Frame& in) {
	ArcMapRange range;
	if (!in.playing) {
		return range;
	}
	const int32_t now = nowPhrase(in);
	// Column 1 shows now + scroll - kNowColumn, column 16 now + scroll + the columns right of now.
	// The ring holds now and 15 before it, but in the 12 ticks before a phrase line the next phrase's decisions
	// already take the oldest one's slot: so one fewer.
	const int32_t oldest = std::max<int32_t>(0, now - (PhraseHistory::kPhrases - 2));
	range.back = std::min<int32_t>(0, oldest - now + ArcMap::kNowColumn);
	int32_t last = in.plan->first + in.plan->count - 1;
	while (last > now && in.plan->at(last) != nullptr && in.plan->at(last)->ended) {
		last--;
	}
	range.ahead = std::max<int32_t>(0, last - now - (ArcMap::kColumns - 1 - ArcMap::kNowColumn));
	return range;
}

} // namespace

bool ArcMapState::refreshPlan(const Generator* piece, uint32_t rosters) {
	PlanKey key;
	key.rosters = rosters;
	if (piece != nullptr) {
		key.playing = true;
		key.decidedPhrase = std::max<int32_t>(piece->nextStep() - 1, 0) / piece->recipe().genre->phraseSteps();
		uint32_t hash = 2166136261U;
		const auto mix = [&](uint32_t value) { hash = (hash ^ value) * 16777619U; };
		for (int32_t i = 0; i < piece->queuedCount(); i++) {
			const Command& command = piece->queued(i);
			mix(static_cast<uint32_t>(command.kind) << 24 | static_cast<uint32_t>(command.target) << 16);
			mix(static_cast<uint32_t>(command.value));
		}
		uint32_t parts = piece->arc().held() ? 1U : 0U;
		for (int32_t p = 0; p < partCount(*piece->recipe().genre); p++) {
			parts |= (piece->partMuted(p) ? 2U : 0U) << (2 * p);
			parts |= (piece->partSoloed(p) ? 4U : 0U) << (2 * p);
		}
		mix(parts);
		mix(static_cast<uint32_t>(piece->queuedCount()));
		key.steering = hash;
	}
	if (key == key_) {
		return false;
	}
	key_ = key;
	if (piece == nullptr) {
		plan_ = ArcPlan{};
		queuedCount_ = 0;
		return false;
	}
	piece->planArc(plan_);
	queuedCount_ = queuedByTime(*piece, queued_);
	return true;
}

bool ArcMapState::follow(const ArcMapInput& in) {
	// The flash changes only when the heard step does: notes in the step before and in this one so far.
	if (in.clock / kTicksPerStep != hitStep_) {
		hitStep_ = in.clock / kTicksPerStep;
		hitFrom_ = (hitStep_ - 1) * kTicksPerStep;
		hitTo_ = in.clock;
	}
	return scrollBy(in, 0);
}

bool ArcMapState::scrollBy(const ArcMapInput& in, int32_t offset) {
	const ArcMapRange range = scrollRange(frameOf(in, *this));
	const int32_t scroll = std::clamp(scroll_ + offset, range.back, range.ahead);
	const bool moved = scroll != scroll_;
	scroll_ = scroll;
	return moved;
}

bool ArcMapState::scrollToNow() {
	const bool moved = scroll_ != 0;
	scroll_ = 0;
	return moved;
}

void composeArcMap(const ArcMapInput& input, const ArcMapState& state, ArcMap& out) {
	out = ArcMap{};
	const Frame in = frameOf(input, state);
	if (!in.playing) {
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
			if (when == ArcMapCell::When::Now && input.focus == p) {
				cell.look = ArcMapCell::Look::Focus;
			}
			else if (sounds) {
				cell.look = ArcMapCell::Look::Part;
				cell.hit = when == ArcMapCell::When::Now && state.flashes(in.history->lastNote(p));
			}
			else if (when == ArcMapCell::When::Planned && (info.muted & (1U << p)) != 0) {
				cell.look = ArcMapCell::Look::Muted;
			}
		}
	}
}

void describeArcMapSections(const ArcMap& map, char (&out)[kArcMapTextChars + 1]) {
	constexpr char kKinds[] = {'I', 'G', 'B', 'U', 'O'}; // SectionKind order
	static_assert(sizeof(kKinds) == kNumSectionKinds);
	char* sections = out + 2;
	char* blink = sections + ArcMap::kColumns + 3;
	out[0] = 's';
	out[1] = '=';
	for (int32_t c = 0; c < ArcMap::kColumns; c++) {
		const ArcMapCell& cell = map.cell[ArcMap::kSectionRow][c];
		sections[c] = cell.look == ArcMapCell::Look::Off ? '.'
		              : cell.drop                        ? 'D'
		                                                 : kKinds[static_cast<int32_t>(cell.kind)];
		bool blinks = false;
		for (const auto& row : map.cell) {
			blinks = blinks || row[c].blink;
		}
		blink[c] = blinks ? 'x' : '.';
	}
	sections[ArcMap::kColumns] = ' ';
	blink[-2] = 'q';
	blink[-1] = '=';
	blink[ArcMap::kColumns] = '\0';
}

void describeArcMapPart(const ArcMap& map, int32_t row, char (&out)[ArcMap::kColumns + 1]) {
	for (int32_t c = 0; c < ArcMap::kColumns; c++) {
		const ArcMapCell& cell = map.cell[row][c];
		switch (cell.look) {
		case ArcMapCell::Look::Part:
			out[c] = cell.when == ArcMapCell::When::Played ? 'h' : cell.when == ArcMapCell::When::Now ? 'n' : 'p';
			break;
		case ArcMapCell::Look::Muted:
			out[c] = 'm';
			break;
		case ArcMapCell::Look::Focus:
			out[c] = 'f';
			break;
		default:
			out[c] = '.';
			break;
		}
	}
	out[ArcMap::kColumns] = '\0';
}

ArcMapRange arcMapScrollRange(const ArcMapInput& in, const ArcMapState& state) {
	return scrollRange(frameOf(in, state));
}

void composeArcPage(const ArcMapInput& input, const ArcMapState& state, ArcPage& out) {
	out = ArcPage{};
	const Frame in = frameOf(input, state);
	if (!in.playing) {
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
		Text next(out.next);
		next.add(what == nullptr ? "STOP" : "NEXT ").add(what == nullptr ? "" : what).add(" ").add(when);
		return;
	}
	Text(out.next).add(kNoChangePlanned);
}

} // namespace deluge::generative::perform
