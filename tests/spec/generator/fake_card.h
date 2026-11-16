// A card for the harness: folders of file names, listed in insertion order, with optional failures (spec §10).

#pragma once

#include "generative/core/palette.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace harness {

using namespace deluge::generative;

inline std::string lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

/// Lists folders from memory. Folder names match case-insensitively, as FatFs does.
struct FakeCard final : FolderLister {
	std::map<std::string, std::vector<std::string>> folders; ///< Keyed by lower-case path.
	bool cardError = false;
	int32_t listings = 0;

	void add(const std::string& folder, std::vector<std::string> names) {
		auto& files = folders[lower(folder)];
		files.insert(files.end(), names.begin(), names.end());
	}

	/// Adds every file under `root` (a card-shaped folder such as pack/), folder by folder, in name order.
	void addTree(const std::filesystem::path& root) {
		std::vector<std::filesystem::path> files;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
			if (entry.is_regular_file()) {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());
		for (const auto& file : files) {
			std::string folder = std::filesystem::relative(file.parent_path(), root).generic_string();
			add(folder, {file.filename().string()});
		}
	}

	/// Adds every card path listed in a text file, one per line ('#' lines are comments).
	void addListing(const std::filesystem::path& listing) {
		std::ifstream in(listing);
		for (std::string line; std::getline(in, line);) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			size_t slash = line.rfind('/');
			add(line.substr(0, slash), {line.substr(slash + 1)});
		}
	}

	/// Whether `path` is on the card, ignoring case as FatFs does.
	[[nodiscard]] bool has(const std::string& path) const {
		size_t slash = path.rfind('/');
		auto found = folders.find(lower(path.substr(0, slash)));
		if (found == folders.end()) {
			return false;
		}
		std::string name = lower(path.substr(slash + 1));
		return std::any_of(found->second.begin(), found->second.end(),
		                   [&](const std::string& file) { return lower(file) == name; });
	}

	bool list(const char* folder, FileVisitor& visitor) override {
		listings++;
		if (cardError) {
			return false;
		}
		auto found = folders.find(lower(folder));
		if (found != folders.end()) {
			for (const std::string& name : found->second) {
				visitor.file(name.c_str());
			}
		}
		return true;
	}
};

#ifdef GENERATOR_REPO_DIR
/// The factory SD card's files (from the committed listing) with the repo's pack/ unzipped on top, as a player's card
/// looks (spec §18.1).
inline FakeCard factoryCardWithPack() {
	FakeCard card;
	std::filesystem::path repo = GENERATOR_REPO_DIR;
	card.addListing(repo / "tests/spec/generator/fixtures/factory_card.txt");
	card.addTree(repo / "pack");
	return card;
}
#endif

/// Loads nothing: records every path tried and answers from its sets.
struct FakeLoader final : SoundLoader {
	std::set<std::string> missing;    ///< Paths that report Missing. Everything else loads.
	std::set<std::string> cardErrors; ///< Paths that report CardError.
	std::vector<std::string> tried;

	LoadOutcome load(const char* path) {
		tried.emplace_back(path);
		if (cardErrors.contains(path)) {
			return LoadOutcome::CardError;
		}
		return missing.contains(path) ? LoadOutcome::Missing : LoadOutcome::Loaded;
	}
	LoadOutcome loadPreset(const char* path) override { return load(path); }
	LoadOutcome loadSample(const char* path) override { return load(path); }
};

} // namespace harness
