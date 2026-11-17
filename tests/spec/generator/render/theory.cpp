// Rough render: the MIDI file's THEORY track, what scripts/generative/theory_review.py reads beside the notes.

#include "render/names.h"
#include "render/render.h"

#include "generative/core/genre.h"

#include <algorithm>
#include <cstdio>

namespace harness::render {

using namespace deluge::generative;

namespace {

/// Macro units as OLED points with one decimal: "25.0".
std::string points(int32_t units) {
	char text[16];
	std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(units) / kMacroUnitsPerPoint);
	return text;
}

std::string target(const Genre& genre, int32_t part, Lane lane) {
	std::string name = part < partCount(genre) ? genre.parts[part].name : "?";
	return lane == Lane::None ? name : name + "." + kLaneNames[static_cast<int32_t>(lane)];
}

/// As Generator::effectiveStrength(): the rule's strength moved by its sensitivity over the Recipe's Complexity away
/// from the genre's default, clamped to 0–100 %.
std::string strength(const InterplayRule& rule, const Recipe& recipe) {
	const int32_t moved =
	    rule.sensitivity * (recipe.macros[Macro::Complexity] - recipe.genre->defaults[Macro::Complexity]);
	const int32_t units = std::clamp<int32_t>(rule.strength * kMacroFull + moved, 0, 100 * kMacroFull);
	char text[16];
	std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(units) / kMacroFull);
	return text;
}

std::string ruleLine(const Genre& genre, const Recipe& recipe, int32_t part, Lane lane, const InterplayRule& rule) {
	return "rule source=" + target(genre, part, lane) + " kind=" + ruleKindName(rule.kind)
	       + " target=" + target(genre, rule.target.part, rule.target.lane) + " off=" + (rule.offBeat ? "1" : "0")
	       + " strength=" + strength(rule, recipe);
}

std::string keyLine(uint8_t root, Scale scale) {
	std::string line = std::string("key root=") + kPitchNames[root % 12]
	                   + " scale=" + kScaleNames[static_cast<int32_t>(scale)] + " pcs=";
	for (int32_t d = 0; d < kDegreesPerScale; d++) {
		line += (d > 0 ? "," : "") + std::to_string((root + kScaleSemitones[static_cast<int32_t>(scale)][d]) % 12);
	}
	return line;
}

std::string chordLine(const Chord& chord) {
	std::string line = "chord ";
	for (int32_t i = 0; i < chord.size; i++) {
		line += (i > 0 ? "," : "") + std::to_string(chord.steps[i]);
	}
	return line;
}

bool sameChord(const Chord& a, const Chord& b) {
	return a.size == b.size && std::equal(a.steps, a.steps + a.size, b.steps);
}

} // namespace

std::vector<std::pair<int32_t, std::string>> theoryText(const Rendering& rendering) {
	const Recipe& recipe = rendering.recipe;
	const Genre& genre = *recipe.genre;
	std::vector<std::pair<int32_t, std::string>> lines;

	char tempo[16];
	std::snprintf(tempo, sizeof(tempo), "%.2f", recipe.tempo / 100.0);
	lines.emplace_back(0, std::string("recipe genre=") + genre.name + " seed=" + std::to_string(rendering.seed)
	                          + " entropy=" + std::to_string(rendering.entropy) + " tempo=" + tempo + " swing="
	                          + std::to_string(recipe.swing) + " energy=" + points(recipe.macros[Macro::Energy])
	                          + " density=" + points(recipe.macros[Macro::Density])
	                          + " complexity=" + points(recipe.macros[Macro::Complexity]) + " motion="
	                          + points(recipe.macros[Macro::Motion]) + " phrase=" + std::to_string(genre.phraseBars)
	                          + " hysteresis=" + points(kGateHysteresis));
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartSpec& part = genre.parts[p];
		lines.emplace_back(0, std::string("part ") + part.name + " role=" + roleName(part.role)
		                          + " flavour=" + flavourName(part.flavour) + " gate=" + points(part.densityGate));
		for (const InterplayRule& rule : part.rules) {
			lines.emplace_back(0, ruleLine(genre, recipe, p, Lane::None, rule));
		}
		for (const LaneSpec& lane : part.lanes) {
			lines.emplace_back(0, "lane " + target(genre, p, lane.lane) + " gate=" + points(lane.densityGate)
			                          + " note=" + std::to_string(laneNote(lane.lane)));
			for (const InterplayRule& rule : lane.rules) {
				lines.emplace_back(0, ruleLine(genre, recipe, p, lane.lane, rule));
			}
		}
	}
	lines.emplace_back(0, keyLine(recipe.keyRoot, recipe.scale));

	// The timeline: each step's macros (on bar lines) and chord, then the stream's key changes, merged by tick.
	std::vector<std::pair<int32_t, std::string>> timeline;
	std::string lastMacros;
	for (size_t step = 0; step < rendering.steps.size(); step++) {
		const StepState& state = rendering.steps[step];
		const int32_t tick = static_cast<int32_t>(step) * kTicksPerStep;
		if (step % kStepsPerBar == 0) {
			std::string macros = "macros energy=" + points(state.energy) + " density=" + points(state.density);
			if (macros != lastMacros) {
				timeline.emplace_back(tick, macros);
				lastMacros = std::move(macros);
			}
		}
		if (step == 0 || !sameChord(state.chord, rendering.steps[step - 1].chord)) {
			timeline.emplace_back(tick, chordLine(state.chord));
		}
	}
	std::string lastKey = lines.back().second;
	for (const Event& e : rendering.events) {
		if (e.kind == EventKind::KeyScale) {
			std::string key = keyLine(e.keyRoot(), e.scale);
			if (key != lastKey) {
				timeline.emplace_back(std::max(e.tick, 0), key);
				lastKey = std::move(key);
			}
		}
	}
	std::stable_sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	lines.insert(lines.end(), timeline.begin(), timeline.end());
	return lines;
}

} // namespace harness::render
