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

/// The draws for one part or lane's sound picks. They come from fresh entropy, never the seed (spec §10.1). Each part
/// and lane has its own stream, so re-rolling one never moves another.
EntropyStream partDraws(uint32_t entropy, uint32_t slot) {
	return EntropyStream(detail::mix(entropy ^ detail::mix(slot + 0x51ed27U)));
}

constexpr uint32_t kPartSlot = 0x100;
constexpr uint32_t kLaneSlot = 0x200;

/// An index into `entries` drawn by weight, or -1 if they weigh nothing. An entry whose path is `avoid` is left out
/// while another weighs anything.
int32_t rollEntry(std::span<const PaletteEntry> entries, EntropyStream& draws, const char* avoid = nullptr) {
	const auto count = static_cast<int32_t>(entries.size());
	const auto below = [&](int32_t total) { return draws.below(total); };
	const int32_t picked = rollWeighted(
	    count,
	    [&](int32_t i) {
		    return avoid != nullptr && equalsIgnoringCase(entries[i].path, avoid) ? 0 : int32_t{entries[i].weight};
	    },
	    below);
	// Only `avoid` weighs anything: roll again with it back in.
	return picked >= 0 ? picked : rollWeighted(count, [&](int32_t i) { return int32_t{entries[i].weight}; }, below);
}

/// An exact-path tag's folder: the text before its last '/'.
std::string_view tagFolder(std::string_view exact) {
	return exact.substr(0, exact.rfind('/'));
}

/// Whether `folder`/`name` is the path `exact`, ignoring case as FatFs does.
bool isPath(std::string_view exact, const char* folder, const char* name) {
	const std::string_view exactFolder = tagFolder(exact);
	return equalsIgnoringCase(exactFolder, folder) && equalsIgnoringCase(exact.substr(exactFolder.size() + 1), name);
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
		if (!isExactPathTag(tag)) {
			continue;
		}
		const std::string_view tagFolderName = tagFolder(tag);
		const size_t length = tagFolderName.size();
		if (length >= static_cast<size_t>(kMaxSoundPath)) {
			continue;
		}
		SoundPath folder;
		std::memcpy(folder.text, tag, length);
		folder.text[length] = 0;
		bool listed = false;
		for (const char* laneFolder : folders) {
			listed = listed || equalsIgnoringCase(laneFolder, tagFolderName);
		}
		for (int32_t earlier = 0; earlier < t && !listed; earlier++) {
			const char* other = sounds.tags[earlier].tag;
			listed = isExactPathTag(other) && equalsIgnoringCase(tagFolder(other), tagFolderName);
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
	return isExactPathTag(tag) ? isPath(tag, folder, name) : inLaneFolder && tagMatches(name, tag);
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
bool rollLane(const LaneSounds& sounds, FolderLister& lister, EntropyStream& draws, const char* preferredTag,
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
			const int32_t rolled = rollWeighted(
			    tags, [&](int32_t t) { return counts[t] > 0 && !skipped[t] ? int32_t{sounds.tags[t].weight} : 0; },
			    [&](int32_t total) { return draws.below(total); });
			tag = rolled >= 0 ? rolled : kNoTag;
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

constexpr uint32_t kKitSlot = 0x300;

/// Whether `pick` is a file `tag` (a tag or an exact path) names.
bool pickMatches(const SoundPath& pick, const char* tag) {
	if (pick.empty() || tag == nullptr) {
		return false;
	}
	if (isExactPathTag(tag)) {
		return equalsIgnoringCase(pick.c_str(), tag);
	}
	const char* slash = std::strrchr(pick.c_str(), '/');
	return tagMatches(slash == nullptr ? pick.c_str() : slash + 1, tag);
}

/// A part's drum kit for one roll (spec §10.2): its main machine and which swaps are in.
struct KitRoll {
	const DrumMachine* machine = nullptr;
	bool swapped[kMaxKitSwaps] = {};

	/// `lane`'s swap if it is in, else nullptr.
	[[nodiscard]] const char* swap(const DrumKits& kits, Lane lane) const {
		for (int32_t s = 0; machine != nullptr && s < static_cast<int32_t>(kits.swaps.size()) && s < kMaxKitSwaps;
		     s++) {
			if (swapped[s] && kits.swaps[s].lane == lane) {
				return kits.swaps[s].tag;
			}
		}
		return nullptr;
	}
};

/// The machine whose tags name the most of `part`'s Drums picks in `sounds`, or -1: the kit a re-roll moves from.
int32_t playingMachine(const DrumKits& kits, const PartSpec& part, const SoundPicks& sounds) {
	int32_t best = -1;
	int32_t bestCount = 0;
	for (int32_t m = 0; m < static_cast<int32_t>(kits.machines.size()); m++) {
		int32_t count = 0;
		for (const LaneSpec& lane : part.lanes) {
			count += !isFxLane(lane.lane)
			                 && pickMatches(sounds.lane[static_cast<int32_t>(lane.lane)],
			                                kits.machines[m].machine->laneTag(lane.lane))
			             ? 1
			             : 0;
		}
		if (count > bestCount) {
			best = m;
			bestCount = count;
		}
	}
	return best;
}

/// Rolls part `p`'s kit: a machine by weight (away from `avoid` where another weighs anything), then each swap in or
/// out. A part with no Drums lanes, or a palette with no machines, has none. The draws come from the part's own kit
/// stream, so they never move a lane's.
KitRoll rollKit(const DrumKits& kits, const PartSpec& part, int32_t p, uint32_t entropy, int32_t avoid) {
	KitRoll kit;
	bool drums = false;
	for (const LaneSpec& lane : part.lanes) {
		drums = drums || !isFxLane(lane.lane);
	}
	if (!drums || kits.machines.empty()) {
		return kit;
	}
	EntropyStream draws = partDraws(entropy, kKitSlot + static_cast<uint32_t>(p));
	const auto count = static_cast<int32_t>(kits.machines.size());
	const auto below = [&](int32_t total) { return draws.below(total); };
	int32_t machine =
	    rollWeighted(count, [&](int32_t m) { return m == avoid ? 0 : int32_t{kits.machines[m].weight}; }, below);
	if (machine < 0) {
		machine = rollWeighted(count, [&](int32_t m) { return int32_t{kits.machines[m].weight}; }, below);
	}
	if (machine < 0) {
		return kit;
	}
	kit.machine = kits.machines[machine].machine;
	for (int32_t s = 0; s < static_cast<int32_t>(kits.swaps.size()) && s < kMaxKitSwaps; s++) {
		kit.swapped[s] = draws.below(100) < kits.swapPercent;
	}
	return kit;
}

/// Rolls `lane` from the one tag `tag` over the lane's folders, whatever its own list says. Leaves `out` empty when
/// the tag matches no file. False on a card error.
bool rollKitTag(const LaneSounds& sounds, const char* tag, FolderLister& lister, EntropyStream& draws,
                const char* avoid, SoundPath& out) {
	const TagWeight only[] = {{tag, 1}};
	LaneSounds single = sounds;
	single.tags = only;
	int32_t rolled = 0;
	return rollLane(single, lister, draws, nullptr, avoid, out, rolled);
}

/// Rolls part `p`'s picks into `sounds`. With `reroll`, each pick tries to move away from the one it replaces, where
/// the palette offers another (a sound re-roll), and the kit moves to another machine. False on a card error.
bool rollPart(const Genre& genre, const Palette& palette, int32_t p, SoundPicks& sounds, uint32_t entropy,
              FolderLister& lister, bool reroll) {
	const PartSpec& part = genre.parts[p];
	std::span<const PaletteEntry> entries = palette.partEntries(part);
	EntropyStream draws = partDraws(entropy, kPartSlot + static_cast<uint32_t>(p));
	int32_t entry = rollEntry(entries, draws, reroll ? sounds.part[p].c_str() : nullptr);
	sounds.part[p].clear();
	if (entry >= 0) {
		sounds.part[p].set(entries[entry].path);
	}
	const KitRoll kit =
	    rollKit(palette.kits, part, p, entropy, reroll ? playingMachine(palette.kits, part, sounds) : -1);
	// Hat pair (spec §10.3): the Open hat first tries the tag the Closed hat rolled, and the Closed hat's swap if it
	// played one. Lanes come in the fixed order, so the Closed hat rolls first.
	const char* closedHatTag = nullptr;
	const char* closedHatSwap = nullptr;
	for (const LaneSpec& lane : part.lanes) {
		const LaneSounds* laneSounds = palette.laneSounds(lane.lane);
		const auto index = static_cast<int32_t>(lane.lane);
		if (laneSounds == nullptr) {
			sounds.lane[index].clear();
			continue;
		}
		EntropyStream laneDraws = partDraws(entropy, kLaneSlot + static_cast<uint32_t>(lane.lane));
		SoundPath old;
		if (reroll) {
			old = sounds.lane[index];
		}
		// The kit's tags first (spec §10.2): the Closed hat's swap for the Open hat, the lane's swap, its machine's
		// tag. Not the Closed hat's machine tag: the Open hat's own names the same machine, and a set's closed-hat
		// prefix (the Psy set's CLHAT-) could match closed hats in the Open hat's folders.
		const char* kitTags[3] = {};
		int32_t kitCount = 0;
		const bool drums = kit.machine != nullptr && !isFxLane(lane.lane) && laneSounds->inKit;
		if (drums && lane.lane == Lane::OpenHat && closedHatSwap != nullptr) {
			kitTags[kitCount++] = closedHatSwap;
		}
		const char* swap = drums ? kit.swap(palette.kits, lane.lane) : nullptr;
		if (swap != nullptr) {
			kitTags[kitCount++] = swap;
		}
		if (const char* machineTag = drums ? kit.machine->laneTag(lane.lane) : nullptr; machineTag != nullptr) {
			kitTags[kitCount++] = machineTag;
		}
		sounds.lane[index].clear();
		for (int32_t t = 0; t < kitCount && sounds.lane[index].empty(); t++) {
			if (!rollKitTag(*laneSounds, kitTags[t], lister, laneDraws, old.c_str(), sounds.lane[index])) {
				return false;
			}
			if (lane.lane == Lane::ClosedHat && kitTags[t] == swap && !sounds.lane[index].empty()) {
				closedHatSwap = swap;
			}
		}
		if (!sounds.lane[index].empty()) {
			continue;
		}
		int32_t tag = 0;
		const char* preferred = lane.lane == Lane::OpenHat ? closedHatTag : nullptr;
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

bool synthPart(const Genre& genre, int32_t part) {
	return part >= 0 && part < partCount(genre) && genre.parts[part].synth();
}

bool choosePartSound(const Genre& genre, int32_t part, const SoundPath& path, SoundPicks& sounds) {
	if (!synthPart(genre, part) || path.empty()) {
		return false;
	}
	sounds.part[part] = path;
	return true;
}

SoundLoad loadPartSound(const Palette& palette, const PartSpec& part, const char* pick, SoundLoader& loader) {
	std::span<const PaletteEntry> entries = palette.partEntries(part);
	const PaletteEntry* picked = nullptr; // the pick's entry, if the palette lists it: the first that does (spec §11.1)
	if (pick != nullptr && pick[0] != 0) {
		for (const PaletteEntry& entry : entries) {
			if (equalsIgnoringCase(entry.path, pick)) {
				picked = &entry;
				break;
			}
		}
		LoadOutcome outcome = loader.loadPreset(pick);
		if (outcome != LoadOutcome::Missing) {
			return {.source = outcome == LoadOutcome::Loaded ? SoundSource::Pick : SoundSource::CardError,
			        .ranges = picked != nullptr ? picked->ranges : ControlRanges{}};
		}
	}
	for (const PaletteEntry& entry : entries) {
		if (&entry == picked || (picked != nullptr && equalsIgnoringCase(entry.path, pick))) {
			continue; // missing as the pick
		}
		LoadOutcome outcome = loader.loadPreset(entry.path);
		if (outcome != LoadOutcome::Missing) {
			return {.source = outcome == LoadOutcome::Loaded ? SoundSource::Fallback : SoundSource::CardError,
			        .ranges = entry.ranges};
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
				return {outcome == LoadOutcome::Loaded ? SoundSource::Fallback : SoundSource::CardError, true,
				        entry.ranges};
			}
		}
	}
	return {SoundSource::Silent};
}

} // namespace deluge::generative
