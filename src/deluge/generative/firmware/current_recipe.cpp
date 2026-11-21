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

#include "generative/firmware/current_recipe.h"
#include "OSLikeStuff/scheduler_api.h"
#include "definitions.h"
#include "fatfs/fatfs.hpp"
#include "generative/core/draw.h"
#include "generative/core/golden_case.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/card_text.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <cstddef>
#include <cstring>

#ifdef GENERATIVE_CAPTURE
#include "generative/core/recipe_overrides.h"
#include <array>
#endif

#ifdef GENERATIVE_CAPTURE
namespace deluge::generative::firmware {

/// The Recipe a capture build plays: a RecipeOverrides with the genre's name inline. scripts/generative/gen_emu.py
/// capture writes it into a copy of the ELF before each boot (CAPTURE_LAYOUT there), so a new Recipe needs no build.
/// The two layouts must agree.
struct CaptureRecipe {
	char magic[8];
	char genre[32]; ///< NUL-terminated. Empty, or a name no genre has: the fallback genre.
	uint32_t entropy;
	uint32_t seed;
	int32_t tempo;
	int8_t macroPoints[kNumMacros];
	int8_t keyRoot;
	int8_t scale;
	int8_t swing;
	uint8_t unused;
};
static_assert(sizeof(CaptureRecipe) == 60 && offsetof(CaptureRecipe, entropy) == 40
              && offsetof(CaptureRecipe, macroPoints) == 52 && offsetof(CaptureRecipe, swing) == 58);

} // namespace deluge::generative::firmware

using CaptureRecipe = deluge::generative::firmware::CaptureRecipe;

// Volatile, so LTO reads it at run time instead of folding the values the build wrote; `used` and
// `externally_visible`, so it stays in .data under this name (LTO would make it local and rename it) for gen_emu.py
// to find.
extern "C" __attribute__((used, externally_visible)) volatile CaptureRecipe generative_capture_recipe = {
    .magic = "GENCAP1",
    .genre = "",
    .entropy = 0,
    .seed = 1,
    .tempo = 0,
    .macroPoints = {-1, -1, -1, -1},
    .keyRoot = -1,
    .scale = -1,
    .swing = deluge::generative::RecipeOverrides::kKeepSwing,
    .unused = 0,
};
#endif

namespace deluge::generative::firmware {

// Zero-filled at boot, not constructed from its defaults: see the declaration. Aligned to 8 for the start step's
// copy of the sound picks: see pendingRecipe in transitions.cpp.
alignas(8) PLACE_SDRAM_BSS CurrentRecipe currentRecipe{};

#if ENABLE_SYSEX_TEST
void forgetCurrentRecipe() {
	// Boot's zero-fill, not a CurrentRecipe{}: that would set the Recipe's non-zero defaults, which boot never does,
	// and build several KB on the stack. The generation goes on counting, so state kept about the forgotten Recipe
	// can't match a later one.
	const uint32_t generation = currentRecipe.generation;
	std::memset(static_cast<void*>(&currentRecipe), 0, sizeof(currentRecipe));
	currentRecipe.generation = generation;
	currentRecipe.replaced();
}
#endif

uint32_t freshEntropy() {
#if ENABLE_SYSEX_TEST
	uint32_t pinned;
	if (TestPort::pinnedEntropy(&pinned)) {
		return pinned; // "hold entropy": the rolls repeat run to run
	}
#endif
	// The timer's low bits change every few cycles, so two calls a few instructions apart still differ.
	static uint32_t calls = 0;
	return deluge::generative::detail::mix(getSystemTicks() ^ (++calls * 0x85ebca6bU));
}

namespace {

/// Reads the genre's palette override file into `override` (spec §10.1). No file, or one that is malformed or too
/// large, or one FatFs can't open or read through, leaves the built-in palette. Returns an error only when the card
/// fails or RAM runs out.
Error readPaletteOverride(const Genre& genre, PaletteOverride& override) {
	override.clear(*genre.palette);
	// GENERATIVE/PALETTES/<GENRE NAME>.XML, built without stdio (the firmware links none).
	char name[32];
	size_t length = std::strlen(genre.oledName);
	if (length + 5 > sizeof(name)) {
		return Error::NONE;
	}
	std::memcpy(name, genre.oledName, length);
	std::memcpy(name + length, ".XML", 5);
	SoundPath filePath;
	filePath.set("GENERATIVE/PALETTES", name);
	struct Parse final : CardTextReader {
		Parse(PaletteOverride& target, const Genre& genre) : target(target), genre(genre) {}
		void read(std::string_view text) override { parsed = target.parse(text, genre); }
		PaletteOverride& target;
		const Genre& genre;
		bool parsed = false;
	} parse{override, genre};
	Error error = Error::NONE;
	const CardText read = readCardText(filePath.c_str(), PaletteOverride::kMaxFileBytes, parse, error);
	if (error == Error::NONE && read != CardText::Missing) {
		TEST_EVENT("gen_palette_override %s %s", genre.name, parse.parsed ? "read" : "ignored");
	}
	return error;
}

} // namespace

const Palette& CurrentRecipe::palette() const {
	if (overrideGenre_ != nullptr && overrideGenre_ == recipe.genre) {
		return overrides_[activeOverride_].palette();
	}
	return *recipe.genre->palette;
}

Error CurrentRecipe::stagePalette(const Genre& genre) {
	return readPaletteOverride(genre, overrides_[1 - activeOverride_]);
}

Error CurrentRecipe::rollFromGenre(const Genre& genre, Chunks* chunks) {
#ifdef GENERATIVE_CAPTURE
	// A capture build plays the Recipe, seed and sounds gen_emu.py patched into its ELF, so a capture is a function
	// of its arguments.
	const volatile CaptureRecipe& capture = generative_capture_recipe;
	std::array<char, sizeof(capture.genre)> genreName{};
	for (size_t i = 0; i + 1 < genreName.size(); i++) { // the last stays NUL
		genreName[i] = capture.genre[i];
	}
	RecipeOverrides overrides;
	overrides.genre = genreName.data();
	overrides.entropy = capture.entropy;
	overrides.seed = capture.seed;
	for (int32_t m = 0; m < kNumMacros; m++) {
		overrides.macroPoints[m] = capture.macroPoints[m];
	}
	overrides.tempo = capture.tempo;
	overrides.keyRoot = capture.keyRoot;
	overrides.scale = capture.scale;
	overrides.swing = capture.swing;
	RolledRecipe rolledRecipe = rollWithOverrides(genre, overrides);
	Recipe rolled = rolledRecipe.recipe;
	uint32_t rolledSeed = rolledRecipe.seed;
	uint32_t soundEntropy = overrides.entropy;
#else
	Recipe rolled = newRecipe(genre, freshEntropy());
	uint32_t rolledSeed = freshEntropy();
	uint32_t soundEntropy = freshEntropy();
#if ENABLE_SYSEX_TEST
	if (TestPort::goldenHeld()) { // the firmware check's golden case (spec §21.3); the sounds still roll
		rolled = newRecipe(genre, golden_case::kEntropy);
		rolledSeed = golden_case::kSeed;
	}
#endif
#endif
	const Genre& rolledGenre = *rolled.genre;
	// Staged: the override, the picks and the Recipe replace the current ones only once all of them are in, so a card
	// error leaves the Current Recipe, its picks and its palette as they were.
	PaletteOverride& staged = overrides_[1 - activeOverride_];
	Error error = readPaletteOverride(rolledGenre, staged);
	if (error != Error::NONE) {
		return error;
	}
	CardFolders folders(chunks);
	if (!rollSounds(rolledGenre, staged.palette(), stagedSounds_, soundEntropy, folders)) {
		if (folders.cancelled()) {
			return Error::ABORTED_BY_USER;
		}
		error = StorageManager::initSD();
		return error != Error::NONE ? error : Error::SD_CARD;
	}
	activeOverride_ = 1 - activeOverride_;
	overrideGenre_ = &rolledGenre;
	recipe = rolled;
	sounds = stagedSounds_;
	seed = rolledSeed;
	takes = Takes{};
	exists = true;
	replaced();
	return Error::NONE;
}

namespace {

/// A roll's error: a cancel, or else the card's (SD_CARD when the card itself still answers).
Error rollError(const CardFolders& folders) {
	if (folders.cancelled()) {
		return Error::ABORTED_BY_USER;
	}
	const Error error = StorageManager::initSD();
	return error != Error::NONE ? error : Error::SD_CARD;
}

} // namespace

Error CurrentRecipe::regenerate(Chunks* chunks) {
	if (!exists || recipe.genre == nullptr) {
		return Error::NONE;
	}
	const Genre& genre = *recipe.genre;
	Recipe rolled = newRecipe(genre, freshEntropy(), Key{recipe.keyRoot, recipe.scale});
	rolled.macros = recipe.macros;
	rolled.bias = recipe.bias; // the same roster: each part keeps its bias (spec §3.3)
	stagedSounds_ = sounds;
	CardFolders folders(chunks);
	if (!rerollSounds(genre, palette(), stagedSounds_, freshEntropy(), folders)) {
		return rollError(folders);
	}
	recipe = rolled;
	sounds = stagedSounds_;
	seed = freshEntropy();
	takes = Takes{};
	replaced();
	return Error::NONE;
}

Error replaceMissingCardSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, int32_t& soundsReplaced,
                               Chunks* chunks) {
	CardFolders folders(chunks);
	CardFiles files(chunks);
	soundsReplaced = replaceMissingSounds(genre, palette, sounds, freshEntropy(), folders, files);
	if (soundsReplaced >= 0) {
		return Error::NONE;
	}
	soundsReplaced = 0;
	if (folders.cancelled() || files.cancelled()) {
		return Error::ABORTED_BY_USER;
	}
	const Error error = StorageManager::initSD();
	return error != Error::NONE ? error : Error::SD_CARD;
}

Error CurrentRecipe::load(const RecipeFile& file, const perform::RecipeName& fileName, int32_t& soundsReplaced,
                          Chunks* chunks) {
	soundsReplaced = 0;
	const Genre& genre = *file.recipe.genre;
	// Staged, as a genre pick: nothing changes until the palette and every pick are in.
	PaletteOverride& staged = overrides_[1 - activeOverride_];
	Error error = readPaletteOverride(genre, staged);
	if (error != Error::NONE) {
		return error;
	}
	stagedSounds_ = file.sounds;
	error = replaceMissingCardSounds(genre, staged.palette(), stagedSounds_, soundsReplaced, chunks);
	if (error != Error::NONE) {
		return error;
	}
	activeOverride_ = 1 - activeOverride_;
	overrideGenre_ = &genre;
	recipe = file.recipe;
	sounds = stagedSounds_;
	seed = file.seed;
	takes = file.takes;
	exists = true;
	replaced();
	name = fileName; // a loaded name is its own stem: the next save offers `<name>A` (spec §16.3)
	return Error::NONE;
}

Error CurrentRecipe::rerollPart(int32_t part, Chunks* chunks) {
	if (!exists || recipe.genre == nullptr) {
		return Error::NONE;
	}
	stagedSounds_ = sounds;
	CardFolders folders(chunks);
	if (!rollPartSounds(*recipe.genre, palette(), part, stagedSounds_, freshEntropy(), folders)) {
		return rollError(folders);
	}
	sounds = stagedSounds_;
	return Error::NONE;
}

Error CurrentRecipe::rerollLane(Lane lane, Chunks* chunks) {
	if (!exists || recipe.genre == nullptr) {
		return Error::NONE;
	}
	stagedSounds_ = sounds;
	CardFolders folders(chunks);
	if (!rollLaneSound(*recipe.genre, palette(), lane, stagedSounds_, freshEntropy(), folders)) {
		return rollError(folders);
	}
	sounds = stagedSounds_;
	return Error::NONE;
}

} // namespace deluge::generative::firmware
