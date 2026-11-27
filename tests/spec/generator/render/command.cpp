// The rough render's command.

#include "render/command.h"
#include "render/names.h"

#include "harness.h"

#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/sound_settings.h"
#include "generative/core/variants.h"
#include "generative/perform/riser_timing.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace harness::render {

using namespace deluge::generative;

const char* const kUsage =
    "usage: generator_render [--out PREFIX] [--genre NAME] [--seed N] [--bars N] [--entropy N]\n"
    "                        [--energy P] [--density P] [--complexity P] [--motion P]\n"
    "                        [--key C..B] [--scale NAME] [--tempo BPM] [--swing -49..49]\n"
    "                        [--pack DIR] [--card DIR] [--end-after-drop [N] | --end-at-bar BAR] [--midi-only]\n"
    "                        [--mute PART]... [--variant SET=NAME]...\n"
    "       generator_render --list-sounds [--genre NAME] [--entropy N] [Recipe flags] [--pack DIR] [--card DIR]\n"
    "       generator_render --list-palettes\n"
    "       generator_render --list-risers\n"
    "\n"
    "Renders a Recipe and seed through the harness to PREFIX.wav, PREFIX.mid and PREFIX.txt (spec 21.2). PREFIX\n"
    "defaults to the repo's build/renders/<genre>-seed<seed>.\n"
    "Macros are OLED points, 0 to 50. Anything not given keeps what the genre's new Recipe rolls from --entropy\n"
    "(default 0). Samples come from --pack (default: the repo's pack/) and then --card (default:\n"
    "~/source/delugemu-sd/sdcard).\n"
    "--bars is the most it plays (default 16). --end-after-drop arms End on the Drop's first step, so the piece plays\n"
    "one Arc cycle and ends after its Outro; with N (1 to 99), on the Nth Drop's. --end-at-bar BAR arms it on bar\n"
    "BAR's first step (bar 0 at PLAY), as gen_emu.py capture holds the End pad from that bar line. Either warns if "
    "End\n"
    "was never armed in --bars, and the WAV's hour limit then counts the bars it played. --mute PART (a sidebar row, 1 "
    "to the genre's part count, at most 7: the part order, as gen_emu.py "
    "capture --mute; "
    "repeatable)\n"
    "leaves that part out of the WAV from step 0, as a Perform mute does: the Generator plays as without it, and the\n"
    "MIDI file and piano roll keep every part. --variant SET=NAME (repeatable) fixes one of the genre's variant sets "
    "to\n"
    "that variant by their stable names (skank=plain, spec 3.6); a set not given keeps what --entropy draws.\n"
    "--midi-only writes only PREFIX.mid. The MIDI file's last track, "
    "THEORY,\n"
    "holds the Recipe, the parts' rules, and the key, chords and macros as they change, for\n"
    "scripts/generative/theory_review.py.\n"
    "--list-sounds prints the sounds the render's WAV plays, rolled from --entropy over --pack and --card: one\n"
    "\"PART<TAB>path\" a line per part, \"PART.LANE<TAB>path\" per lane of a Drums or FX part (- for none). The "
    "card's\n"
    "file order can differ from the render's, so a capture can pick another file from the same entropy.\n"
    "--list-palettes prints each built-in genre's pitched parts' palette presets, one \"genre<TAB>PART<TAB>path\" a\n"
    "line, for scripts/generative/osc_octaves.py.\n"
    "--list-risers prints the pack's fallback Risers, one \"fallback<TAB>path<TAB>ms\" a line, longest first, then\n"
    "each built-in genre's Riser lane, one \"genre<TAB>ms<TAB>folders<TAB>tags\" a line: its shortest Build in ms at\n"
    "the top of its tempo range, its variants' Builds counted, and the lane's folders and tags, each |-separated, for\n"
    "scripts/generative/riser_fit.py.\n"
    "--riser-fallback MICROS prints the fallback Riser the firmware loads beside a sample MICROS long, or -, for\n"
    "scripts/generative/riser_fit.py.\n";

namespace {

/// Lists a card folder across the sound library's roots, like a card with pack/ unzipped over it: a name in an earlier
/// root hides the same name in a later one. Each root's files come in name order, where the firmware gets FAT directory
/// order, so a render can pick another file than the Deluge does from the same entropy.
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

constexpr long long kMaxEndDrops = 99;

bool parseInt(std::string_view text, long long lo, long long hi, long long* out) {
	std::string s(text);
	char* end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 10); // base 10, as gen_emu.py reads them
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

std::string pitchedPalettePresets() {
	std::string out;
	for (const Genre* genre : allGenres()) {
		for (const PartSpec& part : genre->parts) {
			// The parts the load sets to whole-sound transpose 0: the ones whose oscillators' octaves it can't reach.
			if (!partSoundSettings(part).zeroTranspose || genre->palette == nullptr) {
				continue;
			}
			for (const PaletteEntry& entry : genre->palette->partEntries(part)) {
				out += std::string(genre->name) + "\t" + part.name + "\t" + entry.path + "\n";
			}
		}
	}
	return out;
}

std::string riserLanes() {
	std::string out;
	// The fallbacks a Riser lane loads beside a longer drawn sample (spec §9.5), for the check to measure.
	for (const perform::PackRiser& riser : perform::kPackRisers) {
		out += std::string("fallback\t") + riser.path + "\t" + std::to_string(riser.ms) + "\n";
	}
	for (const Genre* genre : allGenres()) {
		const bool hasRiser = std::ranges::any_of(genre->parts, [](const PartSpec& part) {
			return std::ranges::any_of(part.lanes, [](const LaneSpec& lane) { return lane.lane == Lane::Riser; });
		});
		if (!hasRiser || genre->palette == nullptr) {
			continue;
		}
		// The shortest Build a Riser has to fit: the raw minimum of its lengths (a phrase when it lists none, as
		// Arc::buildSteps() plays), not shortestBuildBars(), which caps it at the phrase: Ambient's only Build is 16
		// bars over an 8-bar phrase. Over the genre as written and each of its variants, since a set may patch the
		// Build (Techno's hard Build, 4 bars, #531). A variant whose Builds play no Riser (House's filter) doesn't
		// count.
		auto shortestIn = [](const Genre& g) {
			const std::span<const WeightedBars> builds = g.arc.buildBars;
			if (!g.arc.buildRiser) {
				return INT32_MAX;
			}
			return builds.empty() ? static_cast<int32_t>(g.phraseBars)
			                      : static_cast<int32_t>(std::ranges::min(builds, {}, &WeightedBars::bars).bars);
		};
		int32_t buildBars = shortestIn(*genre);
		auto copy = std::make_unique<ResolvedGenre>();
		for (size_t s = 0; s < genre->variantSets.size(); s++) {
			for (size_t v = 0; v < genre->variantSets[s].variants.size(); v++) {
				VariantPicks picks;
				picks.pick[s] = static_cast<uint8_t>(v);
				buildBars = std::min(buildBars, shortestIn(resolveGenre(*genre, picks, copy.get())));
			}
		}
		if (buildBars == INT32_MAX) {
			continue; // no Build of it plays a Riser
		}
		const int64_t ms = int64_t{buildBars} * (kStepsPerBar / kStepsPerBeat) * 60000 / genre->facts.tempoHighBpm;
		const LaneSounds* sounds = genre->palette->laneSounds(Lane::Riser);
		const LaneSounds none{.lane = Lane::Riser};
		const LaneSounds& lane = sounds != nullptr ? *sounds : none;
		std::string folders;
		for (const char* folder : laneFolders(lane)) {
			folders += (folders.empty() ? "" : "|") + std::string(folder);
		}
		std::string tags;
		for (const TagWeight& tag : lane.tags) {
			tags += (tags.empty() ? "" : "|") + std::string(tag.tag);
		}
		out += std::string(genre->name) + "\t" + std::to_string(ms) + "\t" + folders + "\t" + tags + "\n";
	}
	return out;
}

Parsed parseArgs(int argc, const char* const* argv) {
	Parsed parsed;
	Request& r = parsed.request;
	std::string pack = GENERATOR_RENDER_REPO_ROOT "/pack";
	std::string card = defaultCard();
	std::vector<std::string> variants;
	auto fail = [&](std::string why) {
		parsed.error = std::move(why);
		return parsed;
	};

	for (int i = 1; i < argc; i++) {
		std::string_view flag = argv[i];
		if (flag == "-h" || flag == "--help") {
			parsed.help = true;
			return parsed;
		}
		if (flag == "--end-after-drop") {
			// Its count is optional: a number right after it is the count, as no other argument is a bare number.
			long long drops = 1;
			if (i + 1 < argc && parseInt(argv[i + 1], 1, kMaxEndDrops, &drops)) {
				i++;
			}
			else if (i + 1 < argc && argv[i + 1][0] != '-') {
				return fail("--end-after-drop takes a Drop count, 1 to " + std::to_string(kMaxEndDrops));
			}
			r.endAfterDrop = static_cast<int32_t>(drops);
			continue;
		}
		if (flag == "--midi-only") {
			r.midiOnly = true;
			continue;
		}
		if (flag == "--list-sounds") {
			parsed.listSounds = true;
			continue;
		}
		if (flag == "--list-palettes") {
			if (argc != 2) {
				return fail("--list-palettes takes no other arguments");
			}
			parsed.listPalettes = true;
			return parsed;
		}
		if (flag == "--list-risers") {
			if (argc != 2) {
				return fail("--list-risers takes no other arguments");
			}
			parsed.listRisers = true;
			return parsed;
		}
		if (flag == "--riser-fallback") {
			char* end = nullptr;
			const long long micros = argc == 3 ? std::strtoll(argv[2], &end, 10) : -1;
			if (argc != 3 || end == argv[2] || *end != 0 || micros < 0 || micros > INT32_MAX) {
				return fail("--riser-fallback takes one length in microseconds");
			}
			parsed.riserFallbackFor = micros;
			return parsed;
		}
		int32_t macro = -1;
		for (int32_t m = 0; m < kNumMacros; m++) {
			macro = flag.starts_with("--") && flag.substr(2) == kMacroNames[m] ? m : macro;
		}
		static constexpr std::string_view kFlags[] = {"--genre", "--seed",  "--entropy",    "--bars",   "--key",
		                                              "--scale", "--tempo", "--swing",      "--pack",   "--card",
		                                              "--out",   "--mute",  "--end-at-bar", "--variant"};
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
		else if (flag == "--end-at-bar") {
			if (!parseInt(value, 0, 9999, &n)) {
				return fail("--end-at-bar takes a bar, 0 to 9999");
			}
			r.endAtBar = static_cast<int32_t>(n);
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
		else if (flag == "--mute") {
			if (!parseInt(value, 1, kMaxParts, &n)) {
				return fail("--mute takes a part, 1 to " + std::to_string(kMaxParts));
			}
			r.mutedParts |= 1U << (n - 1);
		}
		else if (flag == "--variant") {
			variants.emplace_back(value); // named against the genre once every flag is read
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
	if (r.endAfterDrop > 0 && r.endAtBar >= 0) {
		return fail("--end-after-drop and --end-at-bar: give one");
	}
	const Genre* genre = r.overrides.genre != nullptr ? findGenre(r.overrides.genre) : allGenres()[0];
	for (const std::string& variant : variants) {
		const std::optional<VariantFix> fix = parseVariantFix(*genre, variant);
		if (!fix) {
			std::string have;
			for (const VariantSet& set : genre->variantSets) {
				for (const Variant& v : set.variants) {
					have += std::string(have.empty() ? "" : ", ") + set.name + "=" + v.name;
				}
			}
			return fail("unknown variant for " + std::string(genre->name) + ": " + variant + " (have "
			            + (have.empty() ? std::string("none") : have) + ")");
		}
		r.overrides.variant[fix->set] = static_cast<int8_t>(fix->variant);
	}
	const auto parts = static_cast<int32_t>(genre->parts.size());
	if ((r.mutedParts >> parts) != 0) {
		return fail(std::string(genre->name) + " has " + std::to_string(parts) + " parts: --mute takes 1 to "
		            + std::to_string(parts));
	}
	if (r.out.empty()) {
		r.out = std::string(GENERATOR_RENDER_REPO_ROOT "/build/renders/") + genre->name + "-seed"
		        + std::to_string(r.overrides.seed);
	}
	r.library.roots = {pack};
	if (!card.empty()) {
		r.library.roots.push_back(card);
	}
	return parsed;
}

namespace {

Rendering playRolled(const Request& request, const RolledRecipe& rolled, uint32_t* droppedEvents, bool* endArmed) {
	Rendering rendering{rolled.recipe, {}, rolled.seed, request.overrides.entropy, request.bars, {}, {}};
	Run run;
	run.recipe = rolled.recipe;
	run.seed = rolled.seed;
	bool armEnd = false;
	bool armed = false;
	// Each driver call decides at most one step, so the last decided step's queries are that step's. An Ornament hit
	// due at the decision's own tick (a flam's grace leaned early) can play before this reads the buffer: it then gets
	// no step=, and theory_review.py puts it on its main hit, a 16th or less after it.
	int32_t drops = 0;
	run.watch = [&](const Generator& generator) {
		const bool decided = static_cast<int32_t>(rendering.steps.size()) < generator.nextStep();
		if (decided) {
			const int32_t decided = generator.nextStep() - 1;
			for (int32_t i = 0; i < generator.lastStepEventCount(); i++) {
				const Event& e = generator.lastStepEvent(i);
				if (e.kind == EventKind::NoteOn && e.ornament) {
					rendering.ornamentSteps.emplace_back(e, decided);
				}
			}
		}
		while (static_cast<int32_t>(rendering.steps.size()) < generator.nextStep()) {
			rendering.steps.push_back(
			    {generator.chord(), generator.effectiveMacro(Macro::Energy), generator.effectiveMacro(Macro::Density)});
		}
		if (decided) {
			drops += generator.arc().dropStartsHere() ? 1 : 0;
			armEnd = armEnd || (request.endAfterDrop > 0 && drops == request.endAfterDrop)
			         || (request.endAtBar >= 0 && generator.nextStep() > request.endAtBar * kStepsPerBar);
		}
	};
	run.steer = [&](int32_t /*nextStep*/, SteeringQueue& queue) {
		if (armEnd && !armed) {
			armed = queue.push({CommandKind::End, 0, 1});
		}
	};
	Result played = play(run, request.bars);
	if (played.ended) {
		constexpr int32_t kTicksPerBar = kStepsPerBar * kTicksPerStep;
		rendering.bars = std::min(request.bars, (played.endedAt + kTicksPerBar - 1) / kTicksPerBar);
	}
	// The driver decides ahead of what plays: keep the steps of the bars played.
	rendering.steps.resize(
	    std::min<size_t>(rendering.steps.size(), static_cast<size_t>(rendering.bars) * kStepsPerBar));
	rendering.events = std::move(played.events);
	if (droppedEvents != nullptr) {
		*droppedEvents = played.droppedEvents;
	}
	if (endArmed != nullptr) {
		*endArmed = armed;
	}
	return rendering;
}

/// The rendering with each note-on of a part in `mutedParts` muted, as Perform's sidebar mutes it (spec §12.5): heard()
/// leaves it out of the WAV, and its note-off still passes.
Rendering withMutes(const Rendering& rendering, uint32_t mutedParts) {
	Rendering muted = rendering;
	for (Event& e : muted.events) {
		if (e.kind == EventKind::NoteOn && e.part < 32 && ((mutedParts >> e.part) & 1U) != 0) {
			e.muted = true;
		}
	}
	return muted;
}

/// The Recipe's sound picks from the overrides' entropy, the entropy a capture build rolls from, over the library.
SoundPicks rolledSounds(const Genre& genre, const Request& request, bool* rolledAll) {
	SoundPicks sounds{};
	RootsLister lister(request.library);
	*rolledAll = rollSounds(genre, *genre.palette, sounds, request.overrides.entropy, lister);
	return sounds;
}

} // namespace

std::string soundPicks(const Request& request, bool* rolledAll) {
	const Genre& genre = *rollWithOverrides(*allGenres()[0], request.overrides).recipe.genre;
	bool all = false;
	const SoundPicks sounds = rolledSounds(genre, request, &all);
	if (rolledAll != nullptr) {
		*rolledAll = all;
	}
	auto path = [](const SoundPath& pick) { return pick.empty() ? std::string("-") : std::string(pick.c_str()); };
	std::string out;
	for (size_t p = 0; p < genre.parts.size(); p++) {
		const PartSpec& part = genre.parts[p];
		if (part.lanes.empty()) {
			out += std::string(part.name) + "\t" + path(sounds.part[p]) + "\n";
		}
		if (const RolledKit kit = rolledKit(*genre.palette, part, static_cast<int32_t>(p), request.overrides.entropy);
		    kit.machine != nullptr) {
			std::string swaps;
			for (size_t w = 0; w < genre.palette->kits.swaps.size() && w < kMaxKitSwaps; w++) {
				if ((kit.swaps >> w) & 1U) {
					const KitSwap& swap = genre.palette->kits.swaps[w];
					swaps += (swaps.empty() ? "" : ",") + std::string(kLaneNames[static_cast<int32_t>(swap.lane)]) + "="
					         + swap.tag;
				}
			}
			out += std::string(part.name) + ".kit\t" + kit.machine->name + "\t" + (swaps.empty() ? "-" : swaps) + "\n";
		}
		for (const LaneSpec& lane : part.lanes) {
			const auto l = static_cast<int32_t>(lane.lane);
			out += std::string(part.name) + "." + kLaneNames[l] + "\t" + path(sounds.lane[l]) + "\n";
		}
	}
	return out;
}

Rendering playRequest(const Request& request, uint32_t* droppedEvents, bool* endArmed) {
	return playRolled(request, rollWithOverrides(*allGenres()[0], request.overrides), droppedEvents, endArmed);
}

Rendered render(const Request& request) {
	const RolledRecipe rolled = rollWithOverrides(*allGenres()[0], request.overrides);
	// Before it plays, unless End makes --bars only a ceiling: then the bars it played count.
	if (!request.midiOnly && !armsEnd(request)) {
		if (std::string why = audioLengthError(request.bars, rolled.recipe.tempo); !why.empty()) {
			return {{}, std::move(why)};
		}
	}
	uint32_t dropped = 0;
	bool endArmed = false;
	Rendering rendering = playRolled(request, rolled, &dropped, &endArmed);
	if (!request.midiOnly && armsEnd(request)) {
		if (std::string why = audioLengthError(rendering.bars, rolled.recipe.tempo); !why.empty()) {
			return {{}, std::move(why)};
		}
	}
	Rendered rendered;
	if (armsEnd(request) && !endArmed) {
		const std::string what = request.endAtBar >= 0 ? "--end-at-bar: no bar " + std::to_string(request.endAtBar)
		                         : request.endAfterDrop == 1
		                             ? std::string("--end-after-drop: no Drop")
		                             : "--end-after-drop " + std::to_string(request.endAfterDrop) + ": no Drop "
		                                   + std::to_string(request.endAfterDrop);
		rendered.warnings.push_back(what + " in " + std::to_string(request.bars)
		                            + " bars, so End was never armed and the piece has no Outro");
	}
	Audio audio;
	std::string roll;
	if (!request.midiOnly) {
		// Sound picks rolled from the overrides' entropy, the entropy a capture build rolls from, over pack/ and the
		// card folder. File order differs from the card's (RootsLister), so a lane can pick another file than a
		// capture does.
		bool rolledAll = false;
		rendering.sounds = rolledSounds(*rendering.recipe.genre, request, &rolledAll);
		audio = renderAudio(withMutes(rendering, request.mutedParts), request.library);
		roll = pianoRoll(rendering);
		rendered.warnings.insert(rendered.warnings.end(), audio.warnings.begin(), audio.warnings.end());
		if (!rolledAll) {
			rendered.warnings.push_back("a sound folder couldn't be listed: some parts and lanes have no sound pick");
		}
	}
	std::vector<uint8_t> midi = midiFile(rendering);
	if (dropped != 0) {
		rendered.warnings.push_back(std::to_string(dropped) + " events dropped on overflow");
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
		bool write;
	} files[] = {
	    {".wav", audio.wav.data(), audio.wav.size(), !request.midiOnly},
	    {".mid", midi.data(), midi.size(), true},
	    {".txt", roll.data(), roll.size(), !request.midiOnly},
	};
	for (const auto& file : files) {
		if (file.write && !writeFile(request.out + file.ext, file.data, file.size)) {
			rendered.error = "can't write " + request.out + file.ext;
			break;
		}
	}
	return rendered;
}

} // namespace harness::render
