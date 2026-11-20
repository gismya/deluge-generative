// PROTOTYPE (#414, throwaway): part_bias_prototype, the Part bias curve sweep. It plays a genre's Recipe with one
// macro's centre swept 0 → 50 across the bars, one part biased (Generator::testBias_), and writes:
// - <out>.csv: a line per decided step: bar, section, the shared value after the Arc, the part's biased value, and
//   per part its heard note-ons on that step;
// - <out>.wav (unless --no-audio): the rough render (render.h), of the biased part alone with --solo, else the mix.
// scripts/generative/prototype_part_bias.py runs it and builds the page. See its README.

#include "render/names.h"
#include "render/render.h"

#include "harness.h"

#include "generative/core/palette.h"
#include "generative/core/part_bias_prototype.h"
#include "generative/core/recipe_overrides.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace harness;
using namespace harness::render;
using namespace deluge::generative;

namespace {

struct RootsLister final : FolderLister {
	const SoundLibrary& library;
	explicit RootsLister(const SoundLibrary& library) : library(library) {}

	bool list(const char* folder, FileVisitor& visitor) override {
		std::vector<std::string> seen;
		for (const std::string& root : library.roots) {
			std::error_code error;
			std::vector<std::string> names;
			for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(root) / folder, error)) {
				if (entry.is_regular_file()) {
					names.push_back(entry.path().filename().string());
				}
			}
			std::sort(names.begin(), names.end());
			for (const std::string& name : names) {
				if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
					seen.push_back(name);
					visitor.file(name.c_str());
				}
			}
		}
		return true;
	}
};

int usage() {
	std::fputs("part_bias_prototype --genre G --part NAME --macro energy|density|complexity|motion\n"
	           "  --model linear|curved|gateshift --bias POINTS [--centre POINTS] [--density POINTS]\n"
	           "  [--entropy N] [--bars N] [--solo] [--no-audio] [--pack DIR] [--card DIR] --out PREFIX\n",
	           stderr);
	return 2;
}

} // namespace

int main(int argc, char** argv) {
	std::string genreName;
	std::string partName;
	std::string out;
	std::string pack = std::string(GENERATOR_RENDER_REPO_ROOT) + "/pack";
	std::string card;
	if (const char* home = std::getenv("HOME")) {
		card = std::string(home) + "/source/delugemu-sd/sdcard";
	}
	int32_t macro = -1;
	auto model = prototype::BiasModel::Linear;
	int32_t biasPoints = 0;
	uint32_t entropy = 1;
	int32_t bars = 32;
	int32_t heldDensity = -1; // --density: Density's points while another macro sweeps, so a gated part plays
	int32_t fixedCentre = -1; // --centre: no sweep; the Arc is forced through a Break and a Build instead
	bool solo = false;
	bool audio = true;
	for (int i = 1; i < argc; i++) {
		const std::string_view flag = argv[i];
		if (flag == "--solo") {
			solo = true;
			continue;
		}
		if (flag == "--no-audio") {
			audio = false;
			continue;
		}
		if (i + 1 >= argc) {
			return usage();
		}
		const std::string value = argv[++i];
		if (flag == "--genre") {
			genreName = value;
		}
		else if (flag == "--part") {
			partName = value;
		}
		else if (flag == "--macro") {
			for (int32_t m = 0; m < kNumMacros; m++) {
				macro = value == kMacroNames[m] ? m : macro;
			}
		}
		else if (flag == "--model") {
			if (value == "linear") {
				model = prototype::BiasModel::Linear;
			}
			else if (value == "curved") {
				model = prototype::BiasModel::Curved;
			}
			else if (value == "gateshift") {
				model = prototype::BiasModel::CurvedGateShift;
			}
			else {
				return usage();
			}
		}
		else if (flag == "--bias") {
			biasPoints = std::atoi(value.c_str());
		}
		else if (flag == "--density") {
			heldDensity = std::clamp(std::atoi(value.c_str()), 0, kMacroPoints);
		}
		else if (flag == "--centre") {
			fixedCentre = std::clamp(std::atoi(value.c_str()), 0, kMacroPoints);
		}
		else if (flag == "--entropy") {
			entropy = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
		}
		else if (flag == "--bars") {
			bars = std::max(2, std::atoi(value.c_str()));
		}
		else if (flag == "--pack") {
			pack = value;
		}
		else if (flag == "--card") {
			card = value;
		}
		else if (flag == "--out") {
			out = value;
		}
		else {
			return usage();
		}
	}
	const Genre* genre = findGenre(genreName.c_str());
	if (genre == nullptr || macro < 0 || out.empty()) {
		return usage();
	}
	int32_t part = -1;
	for (int32_t p = 0; p < partCount(*genre); p++) {
		part = partName == genre->parts[p].name ? p : part;
	}
	if (part < 0) {
		std::fprintf(stderr, "no part %s in %s\n", partName.c_str(), genreName.c_str());
		return 2;
	}

	RecipeOverrides overrides;
	overrides.genre = genre->name;
	overrides.entropy = entropy;
	overrides.seed = entropy;
	if (heldDensity >= 0) {
		overrides.macroPoints[static_cast<int32_t>(Macro::Density)] = static_cast<int8_t>(heldDensity);
	}
	overrides.macroPoints[macro] = static_cast<int8_t>(std::max(fixedCentre, 0));
	const RolledRecipe rolled = rollWithOverrides(*genre, overrides);

	prototype::PartBias bias;
	bias.part = static_cast<int8_t>(part);
	bias.model = model;
	bias.units[macro] = macroPoints(biasPoints);
	Generator::testBias_ = bias;

	struct StepRow {
		int32_t section;
		int32_t shared;
		int32_t biased;
	};
	std::vector<StepRow> steps;
	Run run;
	run.recipe = rolled.recipe;
	run.seed = rolled.seed;
	run.skipIntro = true; // the sweep starts at once; Breaks and Builds still move it (the shared Arc)
	int32_t steeredBar = -1;
	// With --centre, a Break and then a Build are forced, so the shared Arc moves the value (and a Build jump, if a
	// model had one, would show). Each lands on its next phrase line. Without it, the centre sweeps.
	bool breakForced = false;
	bool buildForced = false;
	run.steer = [&](int32_t nextStep, SteeringQueue& queue) {
		const int32_t bar = nextStep / kStepsPerBar;
		if (fixedCentre >= 0) {
			if (!breakForced && bar >= 1) {
				breakForced = queue.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
			}
			if (!buildForced && !steps.empty() && steps.back().section == static_cast<int32_t>(SectionKind::Break)) {
				buildForced = queue.push({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
			}
			return;
		}
		if (nextStep % kStepsPerBar == 0 && bar != steeredBar && bar < bars) {
			// The centre for this bar: 0 on the first, 50 on the last.
			const int32_t centre = macroPoints(kMacroPoints * bar) / (bars - 1);
			if (queue.push({CommandKind::SetMacro, static_cast<uint8_t>(macro), centre})) {
				steeredBar = bar;
			}
		}
	};
	run.watch = [&](const Generator& generator) {
		while (static_cast<int32_t>(steps.size()) < generator.nextStep()) {
			const auto m = static_cast<Macro>(macro);
			const int32_t shared =
			    m == Macro::Energy || m == Macro::Density ? generator.effectiveMacro(m) : generator.recipe().macros[m];
			steps.push_back(
			    {static_cast<int32_t>(generator.arc().section()), shared, generator.testBiasedMacro(part, m)});
		}
	};
	Result played = play(run, bars);
	Generator::testBias_ = {};

	const int32_t parts = partCount(*genre);
	std::vector<std::vector<int32_t>> notes(static_cast<size_t>(bars) * kStepsPerBar, std::vector<int32_t>(parts, 0));
	for (const Event& e : played.events) {
		if (e.kind == EventKind::NoteOn && heard(e) && !e.ornament) {
			const int32_t s = e.tick / kTicksPerStep;
			if (s >= 0 && s < static_cast<int32_t>(notes.size()) && e.part < parts) {
				notes[s][e.part]++;
			}
		}
	}
	std::filesystem::create_directories(std::filesystem::path(out).parent_path());
	std::ofstream csv(out + ".csv");
	csv << "step,bar,section,shared,biased";
	for (int32_t p = 0; p < parts; p++) {
		csv << "," << genre->parts[p].name;
	}
	csv << "\n";
	const size_t rows = std::min(steps.size(), notes.size());
	for (size_t s = 0; s < rows; s++) {
		csv << s << "," << s / kStepsPerBar << "," << sectionName(static_cast<SectionKind>(steps[s].section)) << ","
		    << steps[s].shared << "," << steps[s].biased;
		for (int32_t p = 0; p < parts; p++) {
			csv << "," << notes[s][p];
		}
		csv << "\n";
	}

	if (audio) {
		Rendering rendering{rolled.recipe, {}, rolled.seed, entropy, bars, played.events, {}, {}};
		if (solo) {
			for (Event& e : rendering.events) {
				e.muted = e.muted || (e.kind == EventKind::NoteOn && e.part != part);
			}
		}
		SoundLibrary library{{pack}};
		if (!card.empty()) {
			library.roots.push_back(card);
		}
		RootsLister lister(library);
		rollSounds(*genre, *genre->palette, rendering.sounds, entropy, lister);
		Audio wav = renderAudio(rendering, library);
		std::ofstream file(out + ".wav", std::ios::binary);
		file.write(reinterpret_cast<const char*>(wav.wav.data()), static_cast<std::streamsize>(wav.wav.size()));
	}
	if (played.droppedEvents != 0) {
		std::fprintf(stderr, "warning: %u events dropped\n", played.droppedEvents);
	}
	return 0;
}
