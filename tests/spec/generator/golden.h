// The goldens (spec §21.2, #121): each Techno subgenre's default Recipe at a fixed seed, played for two phrases and
// dumped as text, one event per line, with the subgenre's stream fingerprint (stream_fingerprint.h) in its header.
// golden_spec compares a fresh dump with the committed file. A retune regenerates them as spec §21.2 says
// ("A retune"): `./dbt test --generator --update-goldens` runs golden_spec with GENERATOR_UPDATE_GOLDENS=1 (it writes
// the files), then scripts/generative/goldens.py's version check.

#pragma once

#include "generative/core/golden_case.h"
#include "line_diff.h"
#include "render/names.h"
#include "stream_fingerprint.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace harness::goldens {

/// The dump's format: goes up with any change to what a golden holds that isn't a genre's stream, such as its layout,
/// kSeed, kBars or kFingerprintPieces. Every golden changes then without a retune, so no version needs a bump
/// (scripts/generative/goldens.py reads it). The `fingerprint` line is the exception: it doesn't depend on the dump,
/// so goldens.py excuses a changed fingerprint only when the text in its parentheses changed too. kFingerprintPieces
/// is in that text; a change to how stream_fingerprint.h hashes or picks its pieces edits the text in goldenText().
constexpr int32_t kFormat = 1;
/// The seed, length and fingerprint size every golden is cut with.
constexpr uint32_t kSeed = deluge::generative::golden_case::kSeed;
constexpr uint32_t kEntropy = deluge::generative::golden_case::kEntropy; ///< The genre's new Recipe from entropy 0.
constexpr int32_t kBars = 16;
/// From SHIFT+PLAY: the Groove at the Recipe's macros from the first bar, the part a retune by ear is about. The
/// fingerprint's pieces cover the Intro, the rest of the Arc and other macros, scales and keys.
constexpr bool kSkipIntro = deluge::generative::golden_case::kSkipIntro;
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
		if (genre->parent == genres::technoAcid.parent) { // the Techno parent table
			out.push_back(genre);
		}
	}
	return out;
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
	const std::string key = kit ? render::kLaneNames[static_cast<int32_t>(e.lane)] : render::noteName(e.pitch);
	switch (e.kind) {
	case EventKind::Section:
		return render::sectionLabel(e);
	case EventKind::KeyScale:
		return std::string(render::kPitchNames[e.keyRoot() % 12]) + " "
		       + render::kScaleNames[static_cast<int32_t>(e.scale)];
	case EventKind::Control: {
		const std::string offset = (e.offset > 0 ? "+" : "") + std::to_string(e.offset);
		return std::string(kit ? key + " " : "") + render::kControlNames[static_cast<int32_t>(e.control)] + " " + offset
		       + (e.throwDelay() > 0 ? " delay " + std::to_string(e.throwDelay()) : "") + (e.snap ? " snap" : "");
	}
	case EventKind::NoteOff:
		return key + " off";
	case EventKind::Sound:
		return "sound " + std::to_string(e.soundToken());
	case EventKind::NoteOn:
		return key + " on v" + std::to_string(e.velocity) + (e.ornament ? " ornament" : "") + (e.muted ? " muted" : "");
	case EventKind::Pump:
		return e.pumps() ? "pump on" : "pump off";
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
	case EventKind::Pump:
		return "PUNCH";
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
	out << "format " << kFormat << "\n";
	out << "genre " << genre.name << " version " << genre.version << "\n";
	out << "recipe entropy " << kEntropy << ": " << render::kPitchNames[recipe.keyRoot % 12] << " "
	    << render::kScaleNames[static_cast<int32_t>(recipe.scale)] << ", " << bpm(recipe.tempo) << " BPM, swing "
	    << int32_t{recipe.swing};
	for (int32_t m = 0; m < kNumMacros; m++) {
		out << ", " << render::kMacroNames[m] << " " << macroToPoints(recipe.macros[static_cast<Macro>(m)]);
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

/// Plays `genre`'s golden: the default Recipe at kSeed for kBars bars from SHIFT+PLAY, with its fingerprint over
/// `pieces` pieces (a spec that only needs the dump passes 0).
inline Golden playGolden(const Genre& genre, uint32_t pieces = kFingerprintPieces) {
	Run run;
	run.recipe = newRecipe(genre, kEntropy);
	run.seed = kSeed;
	run.skipIntro = kSkipIntro;
	const Result result = play(run, kBars);
	const StreamFingerprint fingerprint = fingerprintOf(genre, pieces);
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

/// Where `actual` parts ways with `expected`, for a failing spec to print: a hunk per run of changes, each changed
/// line with one kept line around it, "-" for the golden's lines and "+" for the fresh dump's, each with its line
/// number, the golden's lines first. Stops after `limit` changed lines. Empty when they're the same.
inline std::string diffOf(const std::string& expected, const std::string& actual, size_t limit = 40) {
	if (expected == actual) {
		return "";
	}
	const std::vector<std::string> a = linesOf(expected);
	const std::vector<std::string> b = linesOf(actual);
	if (a == b) {
		return "(only the file's last newline differs)\n";
	}
	const std::optional<std::vector<DiffLine>> script = lineDiff(a, b);
	if (!script) {
		return "(too many changes to list: run the update and read git diff)\n";
	}
	const std::vector<DiffLine>& edits = *script;
	std::string out;
	size_t shown = 0;
	size_t changed = 0;
	for (const DiffLine& edit : edits) {
		changed += edit.op != ' ' ? 1 : 0;
	}
	auto line = [&](char op, size_t index) {
		const std::string& text = op == '+' ? b[index] : a[index];
		out += std::string(1, op) + std::to_string(index + 1) + ": " + text + "\n";
	};
	size_t i = 0;
	while (i < edits.size() && shown < limit) {
		if (edits[i].op == ' ') {
			i++;
			continue;
		}
		// A run of changes, and later runs within two kept lines of it: one hunk.
		size_t end = i;
		while (end < edits.size()) {
			if (edits[end].op != ' ') {
				end++;
				continue;
			}
			size_t kept = end;
			while (kept < edits.size() && edits[kept].op == ' ') {
				kept++;
			}
			if (kept < edits.size() && kept - end <= 2) {
				end = kept;
				continue;
			}
			break;
		}
		out += "@@ golden line " + std::to_string(edits[i].a + 1) + ", fresh line " + std::to_string(edits[i].b + 1)
		       + " @@\n";
		if (i > 0) {
			line(' ', edits[i - 1].a);
		}
		for (size_t r = i; r < end && shown < limit;) {
			if (edits[r].op == ' ') {
				line(' ', edits[r].a);
				r++;
				continue;
			}
			size_t runEnd = r;
			while (runEnd < end && edits[runEnd].op != ' ') {
				runEnd++;
			}
			for (char op : {'-', '+'}) {
				for (size_t e = r; e < runEnd && shown < limit; e++) {
					if (edits[e].op == op) {
						line(op, op == '+' ? edits[e].b : edits[e].a);
						shown++;
					}
				}
			}
			r = runEnd;
		}
		if (shown < limit && end < edits.size()) {
			line(' ', edits[end].a);
		}
		i = end;
	}
	if (changed > shown) {
		out += "... " + std::to_string(changed - shown) + " more changed lines\n";
	}
	return out;
}

/// What syncing a golden found.
struct Sync {
	bool matched = false;   ///< The file held `text` already.
	bool wrote = false;     ///< The file was (re)written with `text`.
	bool unwritten = false; ///< An update was asked for and the write failed: the file may not hold `text`.
	bool lossy = false;     ///< An update was asked for, but the play lost events: the file was left as it was.
	std::string diff;       ///< diffOf(the file, text) when they differ; the whole text when there was no file.
};

/// Compares the file at `path` with `text`. With `update` it rewrites the file when they differ, so the repo then
/// holds the fresh dump; without it, it only reports. `lost` is what the play lost to full buffers (Golden::lost): a
/// dump that lost events isn't what the genre plays, so it is never written.
inline Sync sync(const std::string& path, const std::string& text, bool update, uint32_t lost = 0) {
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
	out.lossy = update && !out.matched && lost != 0;
	if (update && !out.matched && !out.lossy) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		file.close();
		out.wrote = !file.fail();
		out.unwritten = !out.wrote;
	}
	return out;
}

/// Whether this run updates the goldens: GENERATOR_UPDATE_GOLDENS is set and not "0".
inline bool updating() {
	const char* value = std::getenv("GENERATOR_UPDATE_GOLDENS");
	return value != nullptr && std::string(value) != "0" && std::string(value) != "";
}

} // namespace harness::goldens
