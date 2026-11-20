// What genre specs share to check a palette (spec §10.1, §18.3): its synth parts' presets, their main picks, and the
// files its lanes roll on a card.

#pragma once

#include "fake_card.h"

#include "generative/core/genre.h"
#include "generative/core/palette.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace harness {

/// The fewest presets any synth part of `genre` lists in its palette.
inline size_t fewestPartPresets(const Genre& genre) {
	size_t fewest = SIZE_MAX;
	for (const PartSpec& part : genre.parts) {
		if (part.lanes.empty()) {
			fewest = std::min(fewest, genre.palette->partEntries(part).size());
		}
	}
	return fewest;
}

/// The presets `genre`'s synth parts list that `card` doesn't have.
inline std::vector<std::string> partPresetsMissing(const Genre& genre, const FakeCard& card) {
	std::vector<std::string> missing;
	for (const PartSpec& part : genre.parts) {
		for (const PaletteEntry& entry :
		     part.lanes.empty() ? genre.palette->partEntries(part) : std::span<const PaletteEntry>{}) {
			if (!card.has(entry.path)) {
				missing.emplace_back(entry.path);
			}
		}
	}
	return missing;
}

/// A part's main pick: its palette's first preset and weight, as "path x weight".
inline std::string mainPick(const Genre& genre, int32_t part) {
	const auto entries = genre.palette->partEntries(genre.parts[part]);
	return entries.empty() ? "" : std::string(entries[0].path) + " x" + std::to_string(entries[0].weight);
}

/// Every distinct file each lane picks over `rolls` fresh rolls of `genre`'s sounds from `card`, entropy 0 on.
/// `each` sees every roll's picks. With `kits` false, each lane rolls from its own list alone, without the genre's
/// drum kit (spec §10.2): the files its fallback chain has, which §10.1's minimum length counts.
inline std::map<Lane, std::set<std::string>> lanePicks(const Genre& genre, FakeCard& card, uint32_t rolls,
                                                       const std::function<void(const SoundPicks&)>& each = {},
                                                       bool kits = true) {
	std::map<Lane, std::set<std::string>> picks;
	Palette palette = *genre.palette;
	if (!kits) {
		palette.kits = {};
	}
	for (uint32_t entropy = 0; entropy < rolls; entropy++) {
		SoundPicks sounds;
		rollSounds(genre, palette, sounds, entropy, card);
		if (each) {
			each(sounds);
		}
		for (int32_t l = 0; l < kNumLanes; l++) {
			if (!sounds.lane[l].empty()) {
				picks[static_cast<Lane>(l)].insert(sounds.lane[l].c_str());
			}
		}
	}
	return picks;
}

} // namespace harness
