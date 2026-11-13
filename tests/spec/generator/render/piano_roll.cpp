// Rough render: the text piano roll.

#include "render/names.h"
#include "render/render.h"

#include "generative/core/genre.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>

namespace harness::render {

using namespace deluge::generative;

namespace {

constexpr int32_t kBarsPerBlock = 4;

std::string rowLabel(const PartSpec& part, Lane lane, uint8_t pitch) {
	return std::string(part.name) + " "
	       + (lane != Lane::None ? std::string(kLaneNames[static_cast<int32_t>(lane)])
	                             : kPitchNames[pitch % 12] + std::to_string(pitch / 12 - 1));
}

char hit(uint8_t velocity) {
	return velocity >= 100 ? 'X' : velocity >= 64 ? 'x' : 'o';
}

/// One row of the roll: a label and a character per 16th of the whole rendering.
struct Row {
	std::string label;
	std::string cells;
	void strike(int32_t tick, uint8_t velocity) {
		int32_t cell = tick / kTicksPerStep;
		if (cell < 0 || cell >= static_cast<int32_t>(cells.size())) {
			return;
		}
		char c = hit(velocity);
		char& at = cells[cell];
		// The loudest hit in a 16th wins, and any hit beats a held note.
		if (at == '.' || at == '=' || (at == 'o' && c != 'o') || (at == 'x' && c == 'X')) {
			at = c;
		}
	}
	void hold(int32_t onTick, int32_t offTick) {
		for (int32_t cell = onTick / kTicksPerStep + 1;
		     cell * kTicksPerStep < offTick && cell < static_cast<int32_t>(cells.size()); cell++) {
			if (cells[cell] == '.') {
				cells[cell] = '=';
			}
		}
	}
};

} // namespace

std::string pianoRoll(const Rendering& rendering) {
	const Recipe& recipe = rendering.recipe;
	const Genre& genre = *recipe.genre;
	const int32_t numParts = partCount(genre);
	const int32_t steps = rendering.bars * kStepsPerBar;

	Row sections{"Section", std::string(steps, ' ')};
	std::vector<Row> rows;
	for (int32_t p = 0; p < numParts; p++) {
		const PartSpec& part = genre.parts[p];
		if (!part.lanes.empty()) {
			for (const LaneSpec& lane : part.lanes) {
				rows.push_back({rowLabel(part, lane.lane, 0), std::string(steps, '.')});
			}
			continue;
		}
		std::set<uint8_t, std::greater<>> pitches;
		for (const Event& e : rendering.events) {
			if (e.kind == EventKind::NoteOn && heard(e) && e.part == p) {
				pitches.insert(e.pitch);
			}
		}
		for (uint8_t pitch : pitches) {
			rows.push_back({rowLabel(part, Lane::None, pitch), std::string(steps, '.')});
		}
	}

	auto rowFor = [&](const Event& e) -> Row* {
		std::string label = rowLabel(genre.parts[e.part], e.lane, e.pitch);
		auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.label == label; });
		return it == rows.end() ? nullptr : &*it;
	};

	for (size_t i = 0; i < rendering.events.size(); i++) {
		const Event& e = rendering.events[i];
		if (e.kind == EventKind::Section) {
			const std::string name = sectionLabel(e);
			for (int32_t c = 0; c < static_cast<int32_t>(name.size()) && e.tick / kTicksPerStep + c < steps; c++) {
				sections.cells[e.tick / kTicksPerStep + c] = name[c];
			}
			continue;
		}
		if (e.kind != EventKind::NoteOn || !heard(e) || e.part >= numParts) {
			continue;
		}
		Row* row = rowFor(e);
		if (row == nullptr) {
			continue;
		}
		row->strike(e.tick, e.velocity);
		if (e.lane == Lane::None) {
			// Held until this note's own note-off.
			for (size_t j = i + 1; j < rendering.events.size(); j++) {
				const Event& off = rendering.events[j];
				if (off.kind == EventKind::NoteOff && off.sameKey(e)) {
					row->hold(e.tick, off.tick);
					break;
				}
			}
		}
	}

	size_t width = sections.label.size();
	for (const Row& row : rows) {
		width = std::max(width, row.label.size());
	}

	char header[128];
	std::snprintf(header, sizeof(header), "%s seed %u, %s %s, %d.%02d BPM, %d bar%s\n", genre.name,
	              static_cast<unsigned>(rendering.seed), kPitchNames[recipe.keyRoot % 12],
	              kScaleNames[static_cast<int32_t>(recipe.scale)], static_cast<int>(recipe.tempo / 100),
	              static_cast<int>(recipe.tempo % 100), static_cast<int>(rendering.bars),
	              rendering.bars == 1 ? "" : "s");
	std::string out = header;

	auto line = [&](const std::string& label, const std::string& cells, int32_t firstBar, int32_t lastBar) {
		out += label;
		out.append(width + 1 - label.size(), ' ');
		for (int32_t bar = firstBar; bar < lastBar; bar++) {
			out += '|';
			out.append(cells, bar * kStepsPerBar, kStepsPerBar);
		}
		out += "|\n";
	};

	for (int32_t first = 0; first < rendering.bars; first += kBarsPerBlock) {
		const int32_t last = std::min(first + kBarsPerBlock, rendering.bars);
		std::string ruler(steps, ' ');
		for (int32_t bar = first; bar < last; bar++) {
			std::string number = std::to_string(bar + 1);
			ruler.replace(bar * kStepsPerBar, number.size(), number);
		}
		out += '\n';
		line("", ruler, first, last);
		line(sections.label, sections.cells, first, last);
		for (const Row& row : rows) {
			line(row.label, row.cells, first, last);
		}
	}
	return out;
}

} // namespace harness::render
