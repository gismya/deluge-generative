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

// Palette override files (spec §10.1): `GENERATIVE/PALETTES/<GENRE NAME>.XML` on the card. Each part, role or lane the
// file lists replaces that entry of the genre's built-in palette; everything else falls back to it. The firmware reads
// the file; the core parses it from text, so the harness tests the same code. No allocation: an override owns fixed
// storage, and its palette points into it and into the built-in one.
//
//   <palette>
//     <part name="ACID"> <preset path="SYNTHS/047 Basic Dirty Bass.XML" weight="3"/> </part>
//     <role name="pad"> <preset path="SYNTHS/148 Warm 5th Pad.XML"/> </role>
//     <lane name="kick">
//       <folder path="SAMPLES/DRUMS/MyKicks"/>
//       <tag text="909" weight="2"/>
//       <file path="SAMPLES/DRUMS/Kick/808 Kick.wav"/>
//     </lane>
//     <lane name="riser"> <preset path="SYNTHS/GENERATIVE/FX/GEN Riser.XML"/> </lane>
//   </palette>

#pragma once

#include "generative/core/palette.h"
#include <cstdint>
#include <string_view>

namespace deluge::generative {

/// A card palette override parsed over a built-in palette. Not copyable: its palette points into itself.
class PaletteOverride {
public:
	/// The largest override file the firmware reads.
	static constexpr int32_t kMaxFileBytes = 8192;

	PaletteOverride() = default;
	PaletteOverride(const PaletteOverride&) = delete;
	PaletteOverride& operator=(const PaletteOverride&) = delete;

	/// Parses an override file's text over `builtIn`. Returns false if the file is malformed or too large for the
	/// override's storage, and then palette() is `builtIn` unchanged. Unknown elements, and parts, roles or lanes with
	/// names it doesn't know, are skipped, so newer files still load.
	bool parse(std::string_view text, const Palette& builtIn);

	/// No override: palette() is `builtIn`.
	void clear(const Palette& builtIn);

	[[nodiscard]] const Palette& palette() const { return palette_; }

private:
	static constexpr int32_t kTextBytes = 2048;
	static constexpr int32_t kMaxEntries = 64;
	static constexpr int32_t kMaxTags = 64;
	static constexpr int32_t kMaxFolders = 32;
	static constexpr int32_t kMaxPartEntries = 2 * kMaxParts;

	friend class PaletteParser;

	char text_[kTextBytes] = {};
	int32_t textUsed_ = 0;
	PaletteEntry entries_[kMaxEntries] = {};
	int32_t entriesUsed_ = 0;
	TagWeight tags_[kMaxTags] = {};
	int32_t tagsUsed_ = 0;
	const char* folders_[kMaxFolders] = {};
	int32_t foldersUsed_ = 0;
	PartSounds parts_[kMaxPartEntries] = {};
	int32_t partsUsed_ = 0;
	RoleSounds roles_[kNumRoles] = {};
	int32_t rolesUsed_ = 0;
	LaneSounds lanes_[kNumLanes] = {};
	int32_t lanesUsed_ = 0;
	Palette palette_;
};

/// The name a palette file uses for a lane ("kick", "closedHat", "perc1", "riser"...) or a role ("bass", "fx"...).
const char* lanePaletteName(Lane lane);
const char* rolePaletteName(Role role);

} // namespace deluge::generative
