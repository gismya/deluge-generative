// The goldens (spec §21.2, #121): each Techno subgenre's default Recipe at a fixed seed, played for two phrases and
// dumped as text, one event per line, with the subgenre's stream fingerprint (stream_fingerprint.h) in its header.
// golden_spec compares a fresh dump with the committed file. A retune regenerates them with
//
//   ./dbt test --generator --update-goldens
//
// which runs golden_spec with GENERATOR_UPDATE_GOLDENS=1 (it writes the files), then checks that each subgenre whose
// golden changed has a higher version than the committed golden (scripts/generative/goldens.py).

#pragma once

#include "render/names.h"
#include "stream_fingerprint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace harness::goldens {

/// The seed, length and fingerprint size every golden is cut with. Changing one changes every golden.
constexpr uint32_t kSeed = 1;
constexpr uint32_t kEntropy = 0; ///< The default Recipe: the genre's new Recipe from entropy 0.
constexpr int32_t kBars = 16;
/// From SHIFT+PLAY: the Groove at the Recipe's macros from the first bar, the part a retune by ear is about. The
/// fingerprint's pieces cover the Intro, the rest of the Arc and other macros, scales and keys.
constexpr bool kSkipIntro = true;
constexpr uint32_t kFingerprintPieces = 60;

/// Where a genre's golden lives in the repo: tests/spec/generator/goldens/<name>.txt.
inline std::string pathOf(const Genre& genre) {
	const std::filesystem::path path =
	    std::filesystem::path(GENERATOR_REPO_DIR) / "tests/spec/generator/goldens" / (std::string(genre.name) + ".txt");
	return path.lexically_normal().string();
}

/// The genres that have a golden before v1 (spec §21.2): every subgenre of Techno.
inline std::vector<const Genre*> goldenGenres() {
	std::vector<const Genre*> out;
	for (const Genre* genre : allGenres()) {
		if (genre->parent != nullptr && std::string(genre->parent->name) == "techno") {
			out.push_back(genre);
		}
	}
	return out;
}

/// A MIDI note's name, C4 = 60, with sharps: "C4", "F#1", "C-1".
inline std::string noteName(uint8_t note) {
	return std::string(render::kPitchNames[note % 12]) + std::to_string(note / 12 - 1);
}

/// Where a tick falls: "bar.beat.16th", each from 1, and "+ticks" past the 16th's grid line: "2.3.4+12".
inline std::string positionOf(int32_t tick) {
	const int32_t step = tick / kTicksPerStep;
	const int32_t rest = tick % kTicksPerStep;
	std::string out = std::to_string(step / kStepsPerBar + 1) + "." + std::to_string(step % kStepsPerBar / 4 + 1) + "."
	                  + std::to_string(step % 4 + 1);
	return rest == 0 ? out : out + "+" + std::to_string(rest);
}

/// `text` left-aligned in `width` characters, or as is if longer.
inline std::string padded(const std::string& text, size_t width) {
	return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

/// What an event says, after its tick, position and part.
inline std::string whatOf(const Event& e) {
	const bool kit = e.lane != Lane::None;
	const std::string key = kit ? render::kLaneNames[static_cast<int32_t>(e.lane)] : noteName(e.pitch);
	switch (e.kind) {
	case EventKind::Section:
		return render::sectionLabel(e);
	case EventKind::KeyScale:
		return std::string(render::kPitchNames[e.keyRoot() % 12]) + " "
		       + render::kScaleNames[static_cast<int32_t>(e.scale)];
	case EventKind::Control: {
		const std::string offset = (e.offset > 0 ? "+" : "") + std::to_string(e.offset);
		return std::string(kit ? key + " " : "") + render::kControlNames[static_cast<int32_t>(e.control)] + " " + offset
		       + (e.snap ? " snap" : "");
	}
	case EventKind::NoteOff:
		return key + " off";
	case EventKind::Sound:
		return "sound " + std::to_string(e.soundToken());
	case EventKind::NoteOn:
		return key + " on v" + std::to_string(e.velocity) + (e.ornament ? " ornament" : "") + (e.muted ? " muted" : "");
	}
	return "?";
}

/// Who an event belongs to: its part's name, or SECTION and KEY for the piece's own events.
inline std::string ownerOf(const Genre& genre, const Event& e) {
	switch (e.kind) {
	case EventKind::Section:
		return "SECTION";
	case EventKind::KeyScale:
		return "KEY";
	default:
		return e.part < genre.parts.size() ? genre.parts[e.part].name : "PART " + std::to_string(e.part);
	}
}

/// The events, one per line in play order: the tick, where it falls, whose it is and what it says.
///   "    384  1.4.1     ACID     G2 on v100"
inline std::string dumpEvents(const Genre& genre, std::span<const Event> events) {
	std::string out;
	for (const Event& e : events) {
		std::string tick = std::to_string(e.tick);
		out += std::string(tick.size() < 7 ? 7 - tick.size() : 0, ' ') + tick + "  " + padded(positionOf(e.tick), 10)
		       + padded(ownerOf(genre, e), 9) + whatOf(e) + "\n";
	}
	return out;
}

/// Hundredths of a BPM as "130.00".
inline std::string bpm(int32_t tempo) {
	const std::string cents = std::to_string(tempo % 100);
	return std::to_string(tempo / 100) + "." + (cents.size() < 2 ? "0" : "") + cents;
}

/// A whole golden file: the header (what was played, the version, the fingerprint), then the dump. `fingerprint` is
/// the genre's stream_fingerprint over kFingerprintPieces pieces.
inline std::string goldenText(const Genre& genre, const Recipe& recipe, std::span<const Event> events,
                              const StreamFingerprint& fingerprint) {
	std::ostringstream out;
	out << "# " << genre.name << "'s golden (spec 21.2): its default Recipe, seed " << kSeed << ", " << kBars
	    << " bars from SHIFT+PLAY.\n";
	out << "# Regenerate with ./dbt test --generator --update-goldens. A change here bumps the genre's version.\n";
	out << "genre " << genre.name << " version " << genre.version << "\n";
	out << "recipe entropy " << kEntropy << ": " << render::kPitchNames[recipe.keyRoot % 12] << " "
	    << render::kScaleNames[static_cast<int32_t>(recipe.scale)] << ", " << bpm(recipe.tempo) << " BPM, swing "
	    << int32_t{recipe.swing};
	static constexpr const char* kMacroNames[kNumMacros] = {"energy", "density", "complexity", "motion"};
	for (int32_t m = 0; m < kNumMacros; m++) {
		out << ", " << kMacroNames[m] << " " << macroToPoints(recipe.macros[static_cast<Macro>(m)]);
	}
	out << "\n";
	out << "parts";
	for (size_t p = 0; p < genre.parts.size(); p++) {
		out << (p == 0 ? " " : ", ") << p << " " << genre.parts[p].name;
	}
	out << "\n";
	char hash[17];
	std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fingerprint.hash));
	out << "fingerprint " << hash << ", " << fingerprint.events << " events, " << fingerprint.lost << " lost ("
	    << kFingerprintPieces << " pieces with random macros, scales, keys and seeds)\n";
	out << "\n";
	out << dumpEvents(genre, events);
	return out.str();
}

/// A golden as played: its text, and what the piece and the fingerprint's pieces lost to full buffers.
struct Golden {
	std::string text;
	uint32_t lost = 0; ///< Dropped events and evicted notes, the golden's piece and the fingerprint's together.
};

/// Plays `genre`'s golden: the default Recipe at kSeed for kBars bars from SHIFT+PLAY, with its fingerprint.
inline Golden playGolden(const Genre& genre) {
	Run run;
	run.recipe = newRecipe(genre, kEntropy);
	run.seed = kSeed;
	run.skipIntro = kSkipIntro;
	const Result result = play(run, kBars);
	const StreamFingerprint fingerprint = fingerprintOf(genre, kFingerprintPieces);
	return {.text = goldenText(genre, run.recipe, result.events, fingerprint),
	        .lost = result.droppedEvents + result.evictedNotes + fingerprint.lost};
}

/// Lines of `text`, without their newlines.
inline std::vector<std::string> linesOf(const std::string& text) {
	std::vector<std::string> lines;
	std::istringstream in(text);
	for (std::string line; std::getline(in, line);) {
		lines.push_back(line);
	}
	return lines;
}

/// Where `actual` parts ways with `expected`, for a failing spec to print: the lines between their common start and
/// common end, "-" for the golden's and "+" for the fresh dump's, each with its line number, at most `limit` of each.
/// Empty when they're the same.
inline std::string diffOf(const std::string& expected, const std::string& actual, size_t limit = 40) {
	const std::vector<std::string> a = linesOf(expected);
	const std::vector<std::string> b = linesOf(actual);
	size_t start = 0;
	while (start < a.size() && start < b.size() && a[start] == b[start]) {
		start++;
	}
	size_t endA = a.size();
	size_t endB = b.size();
	while (endA > start && endB > start && a[endA - 1] == b[endB - 1]) {
		endA--;
		endB--;
	}
	if (start == endA && start == endB) {
		return expected == actual ? "" : "(only the file's last newline differs)\n";
	}
	std::string out = "@@ golden lines " + std::to_string(start + 1) + "-" + std::to_string(endA) + ", fresh lines "
	                  + std::to_string(start + 1) + "-" + std::to_string(endB) + " @@\n";
	auto side = [&](const std::vector<std::string>& lines, size_t end, const char* mark) {
		for (size_t i = start; i < end && i < start + limit; i++) {
			out += std::string(mark) + std::to_string(i + 1) + ": " + lines[i] + "\n";
		}
		if (end > start + limit) {
			out += std::string(mark) + "... " + std::to_string(end - start - limit) + " more\n";
		}
	};
	side(a, endA, "-");
	side(b, endB, "+");
	return out;
}

/// What syncing a golden found.
struct Sync {
	bool matched = false; ///< The file held `text` already.
	bool wrote = false;   ///< The file was (re)written with `text`.
	std::string diff;     ///< diffOf(the file, text) when they differ; the whole text when there was no file.
};

/// Compares the file at `path` with `text`. With `update` it rewrites the file when they differ, so the repo then
/// holds the fresh dump; without it, it only reports.
inline Sync sync(const std::string& path, const std::string& text, bool update) {
	Sync out;
	std::ifstream in(path, std::ios::binary);
	const bool exists = in.good();
	std::string committed;
	if (exists) {
		std::ostringstream buffer;
		buffer << in.rdbuf();
		committed = buffer.str();
	}
	out.matched = exists && committed == text;
	if (!out.matched) {
		out.diff = exists ? diffOf(committed, text) : "(no golden at " + path + ")\n";
	}
	if (update && !out.matched) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		file.close();
		out.wrote = !file.fail();
	}
	return out;
}

/// Whether this run updates the goldens: GENERATOR_UPDATE_GOLDENS is set and not "0".
inline bool updating() {
	const char* value = std::getenv("GENERATOR_UPDATE_GOLDENS");
	return value != nullptr && std::string(value) != "0" && std::string(value) != "";
}

} // namespace harness::goldens
