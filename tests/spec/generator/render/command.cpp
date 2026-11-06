// The rough render's command.

#include "render/command.h"
#include "render/names.h"

#include "harness.h"

#include "generative/core/genre.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace harness::render {

using namespace deluge::generative;

const char* const kUsage =
    "usage: generator_render [--out PREFIX] [--genre NAME] [--seed N] [--bars N] [--entropy N]\n"
    "                        [--energy P] [--density P] [--complexity P] [--motion P]\n"
    "                        [--key C..B] [--scale NAME] [--tempo BPM] [--swing -49..49]\n"
    "                        [--pack DIR] [--card DIR]\n"
    "\n"
    "Renders a Recipe and seed through the harness to PREFIX.wav, PREFIX.mid and PREFIX.txt (spec 21.2). PREFIX\n"
    "defaults to the repo's build/renders/<genre>-seed<seed>.\n"
    "Macros are OLED points, 0 to 50. Anything not given keeps what the genre's new Recipe rolls from --entropy\n"
    "(default 0). Samples come from --pack (default: the repo's pack/) and then --card (default:\n"
    "~/source/delugemu-sd/sdcard).\n";

namespace {

bool parseInt(std::string_view text, long long lo, long long hi, long long* out) {
	std::string s(text);
	char* end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 0);
	if (s.empty() || *end != '\0' || v < lo || v > hi) {
		return false;
	}
	*out = v;
	return true;
}

int8_t keyFromName(std::string_view name) {
	for (int32_t k = 0; k < 12; k++) {
		if (name == kPitchNames[k]) {
			return static_cast<int8_t>(k);
		}
	}
	static constexpr const char* kFlats[12] = {nullptr, "Db",    nullptr, "Eb",    nullptr, nullptr,
	                                           "Gb",    nullptr, "Ab",    nullptr, "Bb",    nullptr};
	for (int32_t k = 0; k < 12; k++) {
		if (kFlats[k] != nullptr && name == kFlats[k]) {
			return static_cast<int8_t>(k);
		}
	}
	return -1;
}

std::string defaultCard() {
	const char* home = std::getenv("HOME");
	return home != nullptr ? std::string(home) + "/source/delugemu-sd/sdcard" : std::string();
}

/// @return False if the file couldn't be written in full.
bool writeFile(const std::string& path, const void* data, size_t size) {
	std::ofstream out(path, std::ios::binary);
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	out.close();
	return !out.fail();
}

} // namespace

Parsed parseArgs(int argc, const char* const* argv) {
	Parsed parsed;
	Request& r = parsed.request;
	std::string pack = GENERATOR_RENDER_REPO_ROOT "/pack";
	std::string card = defaultCard();
	auto fail = [&](std::string why) {
		parsed.error = std::move(why);
		return parsed;
	};
	static constexpr const char* kMacroFlags[kNumMacros] = {"--energy", "--density", "--complexity", "--motion"};

	for (int i = 1; i < argc; i++) {
		std::string_view flag = argv[i];
		if (flag == "-h" || flag == "--help") {
			parsed.help = true;
			return parsed;
		}
		int32_t macro = -1;
		for (int32_t m = 0; m < kNumMacros; m++) {
			macro = flag == kMacroFlags[m] ? m : macro;
		}
		static constexpr std::string_view kFlags[] = {"--genre", "--seed",  "--entropy", "--bars", "--key", "--scale",
		                                              "--tempo", "--swing", "--pack",    "--card", "--out"};
		if (macro < 0 && std::find(std::begin(kFlags), std::end(kFlags), flag) == std::end(kFlags)) {
			return fail("unknown argument: " + std::string(flag));
		}
		if (i + 1 >= argc) {
			return fail(std::string(flag) + " needs a value");
		}
		const char* value = argv[i + 1];
		long long n = 0;
		if (macro >= 0) {
			if (!parseInt(value, 0, kMacroPoints, &n)) {
				return fail(std::string(flag) + " takes 0 to 50");
			}
			r.overrides.macroPoints[macro] = static_cast<int8_t>(n);
		}
		else if (flag == "--genre") {
			if (findGenre(value) == nullptr) {
				return fail(std::string("unknown genre: ") + value);
			}
			r.overrides.genre = value;
		}
		else if (flag == "--seed" || flag == "--entropy") {
			if (!parseInt(value, 0, 0xFFFFFFFFLL, &n)) {
				return fail(std::string(flag) + " takes a 32-bit number");
			}
			(flag == "--seed" ? r.overrides.seed : r.overrides.entropy) = static_cast<uint32_t>(n);
		}
		else if (flag == "--bars") {
			if (!parseInt(value, 1, 10000, &n)) {
				return fail("--bars takes 1 to 10000");
			}
			r.bars = static_cast<int32_t>(n);
		}
		else if (flag == "--key") {
			r.overrides.keyRoot = keyFromName(value);
			if (r.overrides.keyRoot < 0) {
				return fail(std::string("unknown key: ") + value);
			}
		}
		else if (flag == "--scale") {
			int32_t scale = -1;
			for (int32_t s = 0; s < kNumScales; s++) {
				scale = std::string_view(kScaleNames[s]) == value ? s : scale;
			}
			if (scale < 0) {
				return fail(std::string("unknown scale: ") + value);
			}
			r.overrides.scale = static_cast<int8_t>(scale);
		}
		else if (flag == "--tempo") {
			char* end = nullptr;
			double bpm = std::strtod(value, &end);
			if (*end != '\0' || !(bpm >= 20 && bpm <= 999)) { // also refuses NaN
				return fail("--tempo takes 20 to 999 BPM");
			}
			r.overrides.tempo = static_cast<int32_t>(bpm * 100 + 0.5);
		}
		else if (flag == "--swing") {
			if (!parseInt(value, -49, 49, &n)) {
				return fail("--swing takes -49 to 49");
			}
			r.overrides.swing = static_cast<int8_t>(n);
		}
		else if (flag == "--pack") {
			pack = value;
		}
		else if (flag == "--card") {
			card = value;
		}
		else { // --out
			r.out = value;
		}
		i++;
	}
	if (r.out.empty()) {
		const Genre* genre = r.overrides.genre != nullptr ? findGenre(r.overrides.genre) : allGenres()[0];
		r.out = std::string(GENERATOR_RENDER_REPO_ROOT "/build/renders/") + genre->name + "-seed"
		        + std::to_string(r.overrides.seed);
	}
	r.library.roots = {pack};
	if (!card.empty()) {
		r.library.roots.push_back(card);
	}
	return parsed;
}

Rendered render(const Request& request) {
	RolledRecipe rolled = rollWithOverrides(*allGenres()[0], request.overrides);
	Run run;
	run.recipe = rolled.recipe;
	run.seed = rolled.seed;
	Result played = play(run, request.bars);

	Rendering rendering{rolled.recipe, rolled.seed, request.bars, std::move(played.events)};
	Audio audio = renderAudio(rendering, request.library);
	std::vector<uint8_t> midi = midiFile(rendering);
	std::string roll = pianoRoll(rendering);

	Rendered rendered{std::move(audio.warnings), {}};
	if (played.droppedEvents != 0) {
		rendered.warnings.push_back(std::to_string(played.droppedEvents) + " events dropped on overflow");
	}
	const std::filesystem::path folder = std::filesystem::path(request.out).parent_path();
	std::error_code ignored; // a folder that can't be made shows up as a file that can't be written
	if (!folder.empty()) {
		std::filesystem::create_directories(folder, ignored);
	}
	const struct {
		const char* ext;
		const void* data;
		size_t size;
	} files[] = {
	    {".wav", audio.wav.data(), audio.wav.size()},
	    {".mid", midi.data(), midi.size()},
	    {".txt", roll.data(), roll.size()},
	};
	for (const auto& file : files) {
		if (!writeFile(request.out + file.ext, file.data, file.size)) {
			rendered.error = "can't write " + request.out + file.ext;
			break;
		}
	}
	return rendered;
}

} // namespace harness::render
