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

// Palettes and sound picks (spec §10.1–§10.3, §10.5): which card presets and samples a genre's parts and lanes may
// use, how a Recipe's sound picks are rolled from them, and the order a load falls back through them. The core never
// reads the card itself: a FolderLister lists folders for it, and a SoundLoader loads.

#pragma once

#include "generative/core/types.h"
#include <cstdint>
#include <span>
#include <string_view>

namespace deluge::generative {

struct Genre;
struct PartSpec;
struct SoundPath;
struct SoundPicks;

// ---- Palette data (slot 8) ----

/// One weighted card preset for a synth part or an FX lane's synth fallback, with how far the sound controls move it
/// (spec §11.1).
struct PaletteEntry {
	const char* path = nullptr;
	uint8_t weight = 1;
	ControlRanges ranges{};
};

/// A lane's filename tag and its weight (spec §10.3). A tag matches case-insensitively anywhere in a file's name
/// without its extension. A tag holding a '/' is an exact file path instead, so a palette can name a file whose name
/// is contained in longer ones (the plain `808 Kick`).
struct TagWeight {
	const char* tag = nullptr;
	uint8_t weight = 1;
};

/// The most tags one lane entry lists.
constexpr int32_t kMaxLaneTags = 8;
/// The most presets one part or lane entry lists.
constexpr int32_t kMaxPaletteEntries = 8;

/// A lane's sounds: the folders its samples come from, the tags that filter them, and for FX lanes the pack synth
/// presets used when no sample is found (spec §10.5).
struct LaneSounds {
	Lane lane = Lane::Kick;
	/// Card folders, such as "SAMPLES/DRUMS/Kick". Empty: the lane's default folders (defaultLaneFolders()).
	std::span<const char* const> folders;
	/// Empty: any audio file in the folders can be picked.
	std::span<const TagWeight> tags;
	std::span<const PaletteEntry> synths;
	/// Whether the genre's drum kit plays this lane (spec §10.2). A lane a card override replaces leaves the kit: the
	/// player's folders and tags alone pick its sound.
	bool inKit = true;
};

/// A synth part's entry, keyed by the part's name.
struct PartSounds {
	const char* part = "";
	std::span<const PaletteEntry> entries;
};

/// The default entry for parts of a role that have none of their own.
struct RoleSounds {
	Role role = Role::Bass;
	std::span<const PaletteEntry> entries;
};

/// One lane of a drum machine whose files its machine tag doesn't name alone: the lane's own tag or exact path.
struct KitLane {
	Lane lane = Lane::Kick;
	const char* tag = nullptr;
};

/// A drum machine (spec §10.2): the tag that plays it on each Drums lane. A kit tag matches the lane's files whatever
/// the lane's own list says, as a lane tag does (§10.3).
struct DrumMachine {
	const char* name = "";
	/// The tag for every lane `lanes` doesn't list, such as "909". nullptr: only the lanes it lists.
	const char* tag = nullptr;
	std::span<const KitLane> lanes;

	/// The tag that plays this machine on `lane`, or nullptr.
	[[nodiscard]] constexpr const char* laneTag(Lane lane) const {
		for (const KitLane& each : lanes) {
			if (each.lane == lane) {
				return each.tag;
			}
		}
		return tag;
	}
};

struct MachineWeight {
	const DrumMachine* machine = nullptr;
	uint8_t weight = 1;
};

/// An idiomatic swap: one Drums lane plays `tag` (a tag or an exact path) instead of the kit's machine.
struct KitSwap {
	Lane lane = Lane::Kick;
	const char* tag = nullptr;
};

/// The most swaps one genre lists.
constexpr int32_t kMaxKitSwaps = 8;

/// A genre's drum kits (spec §10.2): each piece rolls one main machine by weight for every Drums lane, and each swap
/// in or out at `swapPercent`. No machines: each lane rolls from its own list alone.
struct DrumKits {
	std::span<const MachineWeight> machines;
	std::span<const KitSwap> swaps;
	uint8_t swapPercent = 50;
};

/// A genre's built-in palette (spec §10.1). Card override files replace single entries of it.
struct Palette {
	std::span<const PartSounds> parts;
	std::span<const RoleSounds> roles;
	std::span<const LaneSounds> lanes;
	DrumKits kits{};

	/// The part's own entry, else its role's, else empty.
	[[nodiscard]] std::span<const PaletteEntry> partEntries(const PartSpec& part) const;
	/// The lane's entry, or nullptr.
	[[nodiscard]] const LaneSounds* laneSounds(Lane lane) const;
};

/// A lane's folders when its palette entry lists none: spec §10.2's table for Drums lanes, `SAMPLES/FX/<lane>` for
/// FX lanes.
std::span<const char* const> defaultLaneFolders(Lane lane);

/// The folders a lane's samples come from.
std::span<const char* const> laneFolders(const LaneSounds& sounds);

// ---- Card access ----

/// A file name's letter as FatFs compares it: ASCII case folded.
constexpr char lowerChar(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Whether `a` and `b` are the same text, ignoring ASCII case as FatFs does. Palette names, card paths and file
/// extensions all compare this way.
constexpr bool equalsIgnoringCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (lowerChar(a[i]) != lowerChar(b[i])) {
			return false;
		}
	}
	return true;
}

/// Whether a lane tag names one exact file rather than a part of a file name (TagWeight): it holds a '/'.
constexpr bool isExactPathTag(std::string_view tag) {
	return tag.find('/') != std::string_view::npos;
}

/// Whether a card file name is an audio file the firmware loads: .wav, .aif or .aiff, not a dotfile.
bool isAudioFile(const char* name);

/// Whether `tag` is found case-insensitively in `name` without its extension.
bool tagMatches(const char* name, const char* tag);

/// Receives one file name per call.
class FileVisitor {
public:
	virtual void file(const char* name) = 0;

protected:
	~FileVisitor() = default;
};

/// Lists card folders for the core: over FatFs in the firmware, over a folder or a fixture in the harness.
class FolderLister {
public:
	/// Calls `visitor` for every file directly inside `folder` (no subfolders, no recursion), in card order. A folder
	/// that doesn't exist lists nothing. Returns false only on a card error.
	virtual bool list(const char* folder, FileVisitor& visitor) = 0;

protected:
	~FolderLister() = default;
};

// ---- Sound picks (spec §10.1) ----

/// Rolls every sound pick for `genre` from `palette` (its built-in one, or a card override over it) and fresh
/// entropy: a preset per synth part by weight, a sample per Drums and FX lane by tag weight then at random among
/// matching files, with the hat pair rule (spec §10.3). An FX lane with no sample keeps an empty pick, and loading
/// falls back to its synth presets. Each part and lane draws on its own, so re-rolling one never moves another. Files
/// whose path doesn't fit a SoundPath are never picked. Returns false on a card error.
bool rollSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, uint32_t entropy, FolderLister& lister);

/// A sound re-roll (spec §10.7, §13.4): rolls `part`'s picks again from `palette` and fresh entropy, as rollSounds()
/// rolls them, but each pick moves away from the one in `sounds` where the palette offers another, so NEW SOUND is
/// heard as new. A Kit part re-rolls every lane it plays, its drum kit on another machine (spec §10.2). Every other
/// part's and lane's picks stay. Returns false on a card error, with the part's picks partly rolled: roll into a copy.
bool rollPartSounds(const Genre& genre, const Palette& palette, int32_t part, SoundPicks& sounds, uint32_t entropy,
                    FolderLister& lister);

/// Regenerate's sound picks (spec §9.7): every part re-rolled as rollPartSounds() re-rolls one, so no part or lane
/// keeps the sound it has where its palette offers another, but a Kit's drum machine rolls by its weights and may stay
/// (spec §10.2). Returns false on a card error, with the picks partly
/// rolled: roll into a copy.
bool rerollSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, uint32_t entropy,
                  FolderLister& lister);

/// A transition's incoming sound picks (spec §9.7), into `sounds`, which holds the playing ones: a Regenerate
/// (`incoming` is `playing`) re-rolls them as rerollSounds() does; a genre change rolls the new roster's afresh, as
/// rollSounds() does, keeping none of the old roster's. Returns false on a card error, with the picks partly rolled:
/// roll into a copy.
bool rollIncomingSounds(const Genre& incoming, const Genre& playing, const Palette& palette, SoundPicks& sounds,
                        uint32_t entropy, FolderLister& lister);

/// Whether `part` is a synth part of `genre`, one preset its pick names: the parts a player can choose a preset for
/// (spec §13.5). False for a Kit part (Drums, FX) and a part the genre doesn't have.
[[nodiscard]] bool synthPart(const Genre& genre, int32_t part);

/// A sound pick the player chose (spec §10.1, §13.5): `path` becomes synth `part`'s pick, whether or not the
/// palette lists it. Every other part's and lane's picks stay. False, with nothing changed, unless synthPart() and
/// `path` isn't empty.
bool choosePartSound(const Genre& genre, int32_t part, const SoundPath& path, SoundPicks& sounds);

// ---- Loading with fallback (spec §10.1, §10.2, §10.5, §10.7) ----

enum class LoadOutcome : uint8_t {
	Loaded,
	Missing,   ///< Absent or unloadable: fall back.
	CardError, ///< The card failed: stop, and cancel the load like BACK would (spec §10.7).
};

/// Loads card sounds for the core's fallback chain. The firmware's loader keeps what it loaded.
class SoundLoader {
public:
	virtual LoadOutcome loadPreset(const char* path) = 0;
	virtual LoadOutcome loadSample(const char* path) = 0;

protected:
	~SoundLoader() = default;
};

enum class SoundSource : uint8_t {
	Pick,         ///< The Recipe's sound pick loaded.
	Fallback,     ///< Another palette entry or matching file loaded.
	DefaultSynth, ///< Nothing loaded: the caller loads the Deluge default synth patch and warns.
	Silent,       ///< A lane with nothing loadable: it stays silent, and the caller warns.
	CardError,
};

struct SoundLoad {
	SoundSource source = SoundSource::Silent;
	bool synth = false; ///< An FX lane that loaded a synth preset into its row.
	/// The control ranges of the preset that loaded, from its palette entry (spec §11.1). The defaults for a sample,
	/// the default synth and a preset the palette doesn't list.
	ControlRanges ranges{};

	/// Whether the player sees the one missing-sound warning for this load.
	[[nodiscard]] constexpr bool warns() const {
		return source == SoundSource::DefaultSynth || source == SoundSource::Silent;
	}
};

/// Loads a synth part: its pick, then its palette entries in order (spec §10.1).
SoundLoad loadPartSound(const Palette& palette, const PartSpec& part, const char* pick, SoundLoader& loader);

/// Loads a lane: its pick, then the other files its tags allow (found a few per folder scan), then for FX lanes its
/// synth presets (spec §10.2, §10.5). Returns CardError if the lister or the loader reports one.
SoundLoad loadLaneSound(const Palette& palette, Lane lane, const char* pick, FolderLister& lister, SoundLoader& loader);

} // namespace deluge::generative
