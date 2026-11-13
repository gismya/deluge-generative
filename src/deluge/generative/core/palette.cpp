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

#include "generative/core/palette.h"
#include "generative/core/draw.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include <cstddef>
#include <cstring>

namespace deluge::generative {

namespace {

/// Sound picks draw from fresh entropy, never the seed (spec §10.1). Each part and lane has its own stream, so
/// re-rolling one never moves another.
class EntropyDraws {
public:
	EntropyDraws(uint32_t entropy, uint32_t slot) : state_(detail::mix(entropy ^ detail::mix(slot + 0x51ed27U))) {}

	/// A uniform value in [0, n), for 0 < n.
	int32_t below(int32_t n) {
		state_ = detail::mix(state_ + 0x9e3779b9U);
		return static_cast<int32_t>((static_cast<uint64_t>(state_) * static_cast<uint32_t>(n)) >> 32);
	}

private:
	uint32_t state_;
};

constexpr uint32_t kPartSlot = 0x100;
constexpr uint32_t kLaneSlot = 0x200;

bool equalsIgnoringCase(const char* a, const char* b);

/// An index into `entries` drawn by weight, or -1 if they weigh nothing. An entry whose path is `avoid` is left out
/// while another weighs anything.
int32_t rollEntry(std::span<const PaletteEntry> entries, EntropyDraws& draws, const char* avoid = nullptr) {
	const auto weight = [&](const PaletteEntry& entry, bool skipAvoided) {
		return skipAvoided && avoid != nullptr && equalsIgnoringCase(entry.path, avoid) ? 0 : entry.weight;
	};
	int32_t total = 0;
	for (const PaletteEntry& entry : entries) {
		total += weight(entry, true);
	}
	const bool skipAvoided = total > 0;
	if (!skipAvoided) {
		for (const PaletteEntry& entry : entries) {
			total += entry.weight;
		}
	}
	if (total <= 0) {
		return -1;
	}
	int32_t roll = draws.below(total);
	for (int32_t i = 0; i < static_cast<int32_t>(entries.size()); i++) {
		roll -= weight(entries[i], skipAvoided);
		if (roll < 0) {
			return i;
		}
	}
	return -1;
}

constexpr char lowerChar(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(const char* a, const char* b) {
	for (; *a != 0 && *b != 0; a++, b++) {
		if (lowerChar(*a) != lowerChar(*b)) {
			return false;
		}
	}
	return *a == *b;
}

/// Whether `tag` names one exact file rather than a part of a file name.
bool isExactPath(const char* tag) {
	return std::strchr(tag, '/') != nullptr;
}

/// Whether `folder`/`name` is the path `exact`, ignoring case as FatFs does.
bool isPath(const char* exact, const char* folder, const char* name) {
	const char* slash = std::strrchr(exact, '/');
	auto folderLength = static_cast<size_t>(slash - exact);
	if (std::strlen(folder) != folderLength) {
		return false;
	}
	for (size_t i = 0; i < folderLength; i++) {
		if (lowerChar(exact[i]) != lowerChar(folder[i])) {
			return false;
		}
	}
	return equalsIgnoringCase(slash + 1, name);
}

/// Whether a sound pick can hold `folder`/`name`.
bool fits(const char* folder, const char* name) {
	return std::strlen(folder) + 1 + std::strlen(name) < static_cast<size_t>(kMaxSoundPath);
}

/// Calls `onFile(folder, name, inLaneFolder)` for every audio file a lane entry can reach: those in its folders, and
/// those in the folders of its exact-path tags. Returns false on a card error.
template <typename OnFile>
bool scanLane(const LaneSounds& sounds, FolderLister& lister, OnFile onFile) {
	struct Visitor final : FileVisitor {
		const char* folder = nullptr;
		bool inLaneFolder = false;
		OnFile* onFile = nullptr;
		void file(const char* name) override {
			if (isAudioFile(name) && fits(folder, name)) {
				(*onFile)(folder, name, inLaneFolder);
			}
		}
	};
	Visitor visitor;
	visitor.onFile = &onFile;
	std::span<const char* const> folders = laneFolders(sounds);
	visitor.inLaneFolder = true;
	for (const char* folder : folders) {
		visitor.folder = folder;
		if (!lister.list(folder, visitor)) {
			return false;
		}
	}
	// Exact paths outside the lane's folders: list each such folder once.
	visitor.inLaneFolder = false;
	for (int32_t t = 0; t < static_cast<int32_t>(sounds.tags.size()); t++) {
		const char* tag = sounds.tags[t].tag;
		if (!isExactPath(tag)) {
			continue;
		}
		SoundPath folder;
		const char* slash = std::strrchr(tag, '/');
		auto length = static_cast<size_t>(slash - tag);
		if (length >= static_cast<size_t>(kMaxSoundPath)) {
			continue;
		}
		std::memcpy(folder.text, tag, length);
		folder.text[length] = 0;
		bool listed = false;
		for (const char* laneFolder : folders) {
			listed = listed || equalsIgnoringCase(laneFolder, folder.c_str());
		}
		for (int32_t earlier = 0; earlier < t && !listed; earlier++) {
			const char* other = sounds.tags[earlier].tag;
			listed = isExactPath(other) && std::strrchr(other, '/') - other == static_cast<std::ptrdiff_t>(length)
			         && std::strncmp(other, tag, length) == 0;
		}
		if (listed) {
			continue;
		}
		visitor.folder = folder.c_str();
		if (!lister.list(folder.c_str(), visitor)) {
			return false;
		}
	}
	return true;
}

/// Whether a file matches lane tag `t`; with kAnyFile, any file of the lane's folders; with kAnyTag, any file the
/// lane can pick.
constexpr int32_t kAnyFile = -1;
/// A tag index for "no tag matched anything": the lane stays silent.
constexpr int32_t kNoTag = -2;
constexpr int32_t kAnyTag = -3;
bool matches(const LaneSounds& sounds, int32_t t, const char* folder, const char* name, bool inLaneFolder) {
	if (t == kAnyTag && sounds.tags.empty()) {
		t = kAnyFile;
	}
	if (t == kAnyFile) {
		return inLaneFolder;
	}
	if (t == kAnyTag) {
		for (int32_t each = 0; each < static_cast<int32_t>(sounds.tags.size()) && each < kMaxLaneTags; each++) {
			if (matches(sounds, each, folder, name, inLaneFolder)) {
				return true;
			}
		}
		return false;
	}
	const char* tag = sounds.tags[t].tag;
	return isExactPath(tag) ? isPath(tag, folder, name) : inLaneFolder && tagMatches(name, tag);
}

/// How many reachable files each tag of a lane matches (or, with no tags, counts[0] = every file). False on a card
/// error.
bool countMatches(const LaneSounds& sounds, FolderLister& lister, int32_t (&counts)[kMaxLaneTags]) {
	for (int32_t& count : counts) {
		count = 0;
	}
	int32_t tags = static_cast<int32_t>(sounds.tags.size()) < kMaxLaneTags ? static_cast<int32_t>(sounds.tags.size())
	                                                                       : kMaxLaneTags;
	return scanLane(sounds, lister, [&](const char* folder, const char* name, bool inLaneFolder) {
		if (tags == 0) {
			counts[0] += inLaneFolder ? 1 : 0;
			return;
		}
		for (int32_t t = 0; t < tags; t++) {
			counts[t] += matches(sounds, t, folder, name, inLaneFolder) ? 1 : 0;
		}
	});
}

/// Sets `out` to the `index`th reachable file matching tag `t` (or kAnyFile), in card order. False on a card error.
bool nthMatch(const LaneSounds& sounds, int32_t t, int32_t index, FolderLister& lister, SoundPath& out) {
	out.clear();
	int32_t seen = 0;
	return scanLane(sounds, lister, [&](const char* folder, const char* name, bool inLaneFolder) {
		if (out.empty() && matches(sounds, t, folder, name, inLaneFolder) && seen++ == index) {
			out.set(folder, name);
		}
	});
}

/// Rolls a lane's sample into `out`: a tag by weight among those matching a file, then one of its files at
/// random. With `preferredTag`, that tag (by name) is tried first. With `avoid` (a sound re-roll), a roll landing on
/// that file takes its tag's next file instead, or another tag when the tag has only that one. Returns the tag rolled
/// (or kAnyFile, or kNoTag when nothing matched) through `rolledTag`. False on a card error.
bool rollLane(const LaneSounds& sounds, FolderLister& lister, EntropyDraws& draws, const char* preferredTag,
              const char* avoid, SoundPath& out, int32_t& rolledTag) {
	out.clear();
	rolledTag = kNoTag;
	int32_t counts[kMaxLaneTags];
	if (!countMatches(sounds, lister, counts)) {
		return false;
	}
	int32_t tags = static_cast<int32_t>(sounds.tags.size()) < kMaxLaneTags ? static_cast<int32_t>(sounds.tags.size())
	                                                                       : kMaxLaneTags;
	bool skipped[kMaxLaneTags] = {};
	for (int32_t attempt = 0; attempt <= tags; attempt++) {
		int32_t tag = kNoTag;
		if (tags == 0) {
			tag = counts[0] > 0 ? kAnyFile : kNoTag;
		}
		for (int32_t t = 0; t < tags && preferredTag != nullptr && tag == kNoTag; t++) {
			if (counts[t] > 0 && !skipped[t] && equalsIgnoringCase(sounds.tags[t].tag, preferredTag)) {
				tag = t;
			}
		}
		if (tags > 0 && tag == kNoTag) {
			int32_t total = 0;
			for (int32_t t = 0; t < tags; t++) {
				total += counts[t] > 0 && !skipped[t] ? sounds.tags[t].weight : 0;
			}
			if (total > 0) {
				int32_t roll = draws.below(total);
				for (int32_t t = 0; t < tags && tag == kNoTag; t++) {
					roll -= counts[t] > 0 && !skipped[t] ? sounds.tags[t].weight : 0;
					tag = roll < 0 ? t : kNoTag;
				}
			}
		}
		if (tag == kNoTag) {
			return true; // nothing on the card, or nothing but `avoid`: out holds the last file rolled, if any
		}
		rolledTag = tag;
		const int32_t count = counts[tag == kAnyFile ? 0 : tag];
		const int32_t index = draws.below(count);
		if (!nthMatch(sounds, tag, index, lister, out)) {
			return false;
		}
		if (avoid == nullptr || avoid[0] == 0 || !equalsIgnoringCase(out.c_str(), avoid)) {
			return true;
		}
		if (count > 1) {
			return nthMatch(sounds, tag, (index + 1) % count, lister, out);
		}
		if (tag == kAnyFile) {
			return true; // the only file the lane can play
		}
		skipped[tag] = true; // the tag has only the file the lane has: try another
	}
	return true;
}

/// How many files the fallback chain collects per scan of a lane's folders, so a lane with many missing files
/// rescans once per batch rather than once per file.
constexpr int32_t kFallbackBatch = 4;

/// Collects into `out` up to kFallbackBatch files the lane can pick, after skipping the first `skip`, in card order.
/// `more` says whether further files follow. False on a card error.
bool collectMatches(const LaneSounds& sounds, int32_t skip, FolderLister& lister, SoundPath (&out)[kFallbackBatch],
                    int32_t& collected, bool& more) {
	collected = 0;
	more = false;
	int32_t seen = 0;
	return scanLane(sounds, lister, [&](const char* folder, const char* name, bool inLaneFolder) {
		if (!matches(sounds, kAnyTag, folder, name, inLaneFolder) || seen++ < skip) {
			return;
		}
		if (collected < kFallbackBatch) {
			out[collected++].set(folder, name);
		}
		else {
			more = true;
		}
	});
}

} // namespace

std::span<const PaletteEntry> Palette::partEntries(const PartSpec& part) const {
	for (const PartSounds& sounds : parts) {
		if (equalsIgnoringCase(sounds.part, part.name)) {
			return sounds.entries;
		}
	}
	for (const RoleSounds& sounds : roles) {
		if (sounds.role == part.role) {
			return sounds.entries;
		}
	}
	return {};
}

const LaneSounds* Palette::laneSounds(Lane lane) const {
	for (const LaneSounds& sounds : lanes) {
		if (sounds.lane == lane) {
			return &sounds;
		}
	}
	return nullptr;
}

namespace {
using Folders = const char* const[];
constexpr Folders kKick = {"SAMPLES/DRUMS/Kick"};
constexpr Folders kSnare = {"SAMPLES/DRUMS/Snare"};
constexpr Folders kClap = {"SAMPLES/DRUMS/Clap"};
constexpr Folders kRim = {"SAMPLES/DRUMS/Rim"};
constexpr Folders kClosedHat = {"SAMPLES/DRUMS/HatC"};
constexpr Folders kOpenHat = {"SAMPLES/DRUMS/HatO"};
constexpr Folders kRide = {"SAMPLES/DRUMS/Ride"};
constexpr Folders kCrash = {"SAMPLES/DRUMS/Crash"};
constexpr Folders kTom = {"SAMPLES/DRUMS/TomL", "SAMPLES/DRUMS/TomM", "SAMPLES/DRUMS/TomH"};
constexpr Folders kPerc = {"SAMPLES/DRUMS/Shaker", "SAMPLES/DRUMS/ConL",    "SAMPLES/DRUMS/ConM",
                           "SAMPLES/DRUMS/ConH",   "SAMPLES/DRUMS/Cowbell", "SAMPLES/DRUMS/Tambourine",
                           "SAMPLES/DRUMS/Claves", "SAMPLES/DRUMS/Block",   "SAMPLES/DRUMS/Guiro",
                           "SAMPLES/DRUMS/Other"};
constexpr Folders kRiser = {"SAMPLES/FX/Riser"};
constexpr Folders kFall = {"SAMPLES/FX/Fall"};
constexpr Folders kImpact = {"SAMPLES/FX/Impact"};
constexpr Folders kSiren = {"SAMPLES/FX/Siren"};
} // namespace

std::span<const char* const> defaultLaneFolders(Lane lane) {
	switch (lane) {
	case Lane::Kick:
		return kKick;
	case Lane::Snare:
		return kSnare;
	case Lane::Clap:
		return kClap;
	case Lane::Rim:
		return kRim;
	case Lane::ClosedHat:
		return kClosedHat;
	case Lane::OpenHat:
		return kOpenHat;
	case Lane::Ride:
		return kRide;
	case Lane::Crash:
		return kCrash;
	case Lane::Tom:
		return kTom;
	case Lane::Perc1:
	case Lane::Perc2:
		return kPerc;
	case Lane::Riser:
		return kRiser;
	case Lane::Fall:
		return kFall;
	case Lane::Impact:
		return kImpact;
	case Lane::Siren:
		return kSiren;
	case Lane::None:
		break;
	}
	return {};
}

std::span<const char* const> laneFolders(const LaneSounds& sounds) {
	return sounds.folders.empty() ? defaultLaneFolders(sounds.lane) : sounds.folders;
}

bool isAudioFile(const char* name) {
	if (name[0] == '.') {
		return false;
	}
	const char* dot = std::strrchr(name, '.');
	return dot != nullptr
	       && (equalsIgnoringCase(dot, ".wav") || equalsIgnoringCase(dot, ".aif") || equalsIgnoringCase(dot, ".aiff"));
}

bool tagMatches(const char* name, const char* tag) {
	const char* dot = std::strrchr(name, '.');
	size_t stem = dot != nullptr ? static_cast<size_t>(dot - name) : std::strlen(name);
	size_t length = std::strlen(tag);
	for (size_t start = 0; start + length <= stem; start++) {
		size_t i = 0;
		while (i < length && lowerChar(name[start + i]) == lowerChar(tag[i])) {
			i++;
		}
		if (i == length) {
			return true;
		}
	}
	return false;
}

namespace {

/// Rolls part `p`'s picks into `sounds`. With `reroll`, each pick tries to move away from the one it replaces, where
/// the palette offers another (a sound re-roll). False on a card error.
bool rollPart(const Genre& genre, const Palette& palette, int32_t p, SoundPicks& sounds, uint32_t entropy,
              FolderLister& lister, bool reroll) {
	const PartSpec& part = genre.parts[p];
	std::span<const PaletteEntry> entries = palette.partEntries(part);
	EntropyDraws draws(entropy, kPartSlot + static_cast<uint32_t>(p));
	int32_t entry = rollEntry(entries, draws, reroll ? sounds.part[p].c_str() : nullptr);
	sounds.part[p].clear();
	if (entry >= 0) {
		sounds.part[p].set(entries[entry].path);
	}
	// Hat pair (spec §10.3): the Open hat first tries the tag the Closed hat rolled. Lanes come in the fixed
	// order, so the Closed hat rolls first.
	const char* closedHatTag = nullptr;
	for (const LaneSpec& lane : part.lanes) {
		const LaneSounds* laneSounds = palette.laneSounds(lane.lane);
		const auto index = static_cast<int32_t>(lane.lane);
		if (laneSounds == nullptr) {
			sounds.lane[index].clear();
			continue;
		}
		EntropyDraws laneDraws(entropy, kLaneSlot + static_cast<uint32_t>(lane.lane));
		int32_t tag = 0;
		const char* preferred = lane.lane == Lane::OpenHat ? closedHatTag : nullptr;
		SoundPath old;
		if (reroll) {
			old = sounds.lane[index];
		}
		if (!rollLane(*laneSounds, lister, laneDraws, preferred, old.c_str(), sounds.lane[index], tag)) {
			return false;
		}
		if (lane.lane == Lane::ClosedHat && tag >= 0) {
			closedHatTag = laneSounds->tags[tag].tag;
		}
	}
	return true;
}

} // namespace

bool rollSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, uint32_t entropy,
                FolderLister& lister) {
	sounds = SoundPicks{};
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()) && p < kMaxParts; p++) {
		if (!rollPart(genre, palette, p, sounds, entropy, lister, false)) {
			return false;
		}
	}
	return true;
}

bool rollPartSounds(const Genre& genre, const Palette& palette, int32_t part, SoundPicks& sounds, uint32_t entropy,
                    FolderLister& lister) {
	if (part < 0 || part >= static_cast<int32_t>(genre.parts.size()) || part >= kMaxParts) {
		return true;
	}
	return rollPart(genre, palette, part, sounds, entropy, lister, true);
}

bool rerollSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, uint32_t entropy,
                  FolderLister& lister) {
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()) && p < kMaxParts; p++) {
		if (!rollPart(genre, palette, p, sounds, entropy, lister, true)) {
			return false;
		}
	}
	return true;
}

bool rollIncomingSounds(const Genre& incoming, const Genre& playing, const Palette& palette, SoundPicks& sounds,
                        uint32_t entropy, FolderLister& lister) {
	if (&incoming == &playing) {
		return rerollSounds(incoming, palette, sounds, entropy, lister);
	}
	sounds = SoundPicks{};
	return rollSounds(incoming, palette, sounds, entropy, lister);
}

SoundLoad loadPartSound(const Palette& palette, const PartSpec& part, const char* pick, SoundLoader& loader) {
	if (pick != nullptr && pick[0] != 0) {
		LoadOutcome outcome = loader.loadPreset(pick);
		if (outcome != LoadOutcome::Missing) {
			return {outcome == LoadOutcome::Loaded ? SoundSource::Pick : SoundSource::CardError};
		}
	}
	for (const PaletteEntry& entry : palette.partEntries(part)) {
		if (pick != nullptr && equalsIgnoringCase(entry.path, pick)) {
			continue;
		}
		LoadOutcome outcome = loader.loadPreset(entry.path);
		if (outcome != LoadOutcome::Missing) {
			return {outcome == LoadOutcome::Loaded ? SoundSource::Fallback : SoundSource::CardError};
		}
	}
	return {SoundSource::DefaultSynth};
}

SoundLoad loadLaneSound(const Palette& palette, Lane lane, const char* pick, FolderLister& lister,
                        SoundLoader& loader) {
	bool picked = pick != nullptr && pick[0] != 0;
	if (picked) {
		LoadOutcome outcome = loader.loadSample(pick);
		if (outcome != LoadOutcome::Missing) {
			return {outcome == LoadOutcome::Loaded ? SoundSource::Pick : SoundSource::CardError};
		}
	}
	const LaneSounds* sounds = palette.laneSounds(lane);
	if (sounds == nullptr) {
		return {SoundSource::Silent};
	}
	// The other files the lane's tags allow, in card order, a batch per scan. Nothing is scanned unless the pick
	// failed.
	SoundPath batch[kFallbackBatch];
	int32_t collected = 0;
	bool more = true;
	for (int32_t skip = 0; more; skip += collected) {
		if (!collectMatches(*sounds, skip, lister, batch, collected, more)) {
			return {SoundSource::CardError};
		}
		for (int32_t i = 0; i < collected; i++) {
			if (picked && equalsIgnoringCase(batch[i].c_str(), pick)) {
				continue;
			}
			LoadOutcome outcome = loader.loadSample(batch[i].c_str());
			if (outcome != LoadOutcome::Missing) {
				return {outcome == LoadOutcome::Loaded ? SoundSource::Fallback : SoundSource::CardError};
			}
		}
	}
	// FX lanes with no sample load a pack synth preset into their row (spec §10.5). Drums have no synth fallback.
	if (isFxLane(lane)) {
		for (const PaletteEntry& entry : sounds->synths) {
			LoadOutcome outcome = loader.loadPreset(entry.path);
			if (outcome != LoadOutcome::Missing) {
				return {outcome == LoadOutcome::Loaded ? SoundSource::Fallback : SoundSource::CardError, true};
			}
		}
	}
	return {SoundSource::Silent};
}

} // namespace deluge::generative
