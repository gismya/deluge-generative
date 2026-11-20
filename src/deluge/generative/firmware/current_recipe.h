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

// The Current Recipe (spec §14.2): the one Recipe, with its seed and takes, that Generative mode works on. Kept in
// RAM until power-off. Perform steers it and the Compose screen edits it.

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/genre.h"
#include "generative/core/palette_file.h"
#include "generative/core/recipe.h"
#include "generative/core/recipe_file.h"
#include "generative/perform/recipe_name.h"
#include "generative/runner/runner.h"
#include <cstdint>

namespace deluge::generative::firmware {

struct CurrentRecipe {
	Recipe recipe;
	/// The Recipe's sound picks, kept beside it so the Generator's copy of the Recipe stays small (spec §2.1).
	SoundPicks sounds;
	uint32_t seed = 0;
	Takes takes;
	bool exists = false;
	/// Bumped each time the Current Recipe is replaced by a whole new one: a genre pick (rollFromGenre()),
	/// regenerate(), a Perform genre change or REGENERATE at its start step, `reset recipe`. State kept outside the
	/// Recipe about it (the Compose screen's previous picks) records the generation it belongs to and is void once it
	/// differs. A new way of replacing the Recipe (a Recipe file's load) calls replaced().
	uint32_t generation = 0;

	/// The name of the Recipe file it was loaded from or last saved as (spec §14.2, §16.3), empty if none. A genre pick
	/// and Regenerate clear it, here or in Perform, through replaced().
	perform::RecipeName name;

	/// The Current Recipe was replaced by a whole new one: see `generation`. It forgets its file name too; a Recipe
	/// file's load sets the file's name after calling this.
	void replaced() {
		generation++;
		name.clear();
	}

	/// Picking a genre rolls a fresh Recipe: rolled facts and sound picks, a new seed, takes at 0 and the genre's
	/// default macros (spec §14.1). Facts, sound picks and the seed come from fresh hardware-timer entropy
	/// (spec §3.2, §6.5, §10.1). Sound picks read the card, so this runs in task context. On a card error it returns
	/// the error and leaves the Current Recipe as it was.
	///
	/// It reads the genre's palette override file, `GENERATIVE/PALETTES/<GENRE NAME>.XML`, when the card has one
	/// (spec §10.1). A malformed or oversized file is ignored, and the built-in palette is used.
	///
	/// With a runner job's `chunks`, each folder scan is a chunk boundary: a cancel returns ABORTED_BY_USER, with the
	/// Current Recipe as it was.
	Error rollFromGenre(const Genre& genre, Chunks* chunks = nullptr);

	/// The Compose screen's REGENERATE (spec §14.4, §9.7): facts rolled again from the genre, leaning towards the key
	/// it has, every sound pick re-rolled away from the one it has, a new seed and takes at 0. The macros stay. On a
	/// card error, or a cancel through `chunks` (ABORTED_BY_USER), the Current Recipe stays as it was.
	Error regenerate(Chunks* chunks = nullptr);
	/// The Compose screen's sound re-rolls (spec §14.4): `part`'s picks (every lane of a Kit part), or one lane's,
	/// rolled again from the palette away from the ones they have. Errors as regenerate().
	Error rerollPart(int32_t part, Chunks* chunks = nullptr);
	Error rerollLane(Lane lane, Chunks* chunks = nullptr);

	/// A Recipe file's load outside Perform (spec §16.5, §16.6): the Compose screen's LOAD (and FILE..., #140). In
	/// Perform a LOAD is a transition, stopped too (Transitions::load()). `file` becomes the Current Recipe, its macros
	/// included, with its genre's palette override read as a genre pick reads it and each missing sound re-rolled from
	/// that palette (`soundsReplaced` counts them). It remembers `fileName`. Errors as rollFromGenre(); on one, the
	/// Current Recipe stays as it was.
	Error load(const RecipeFile& file, const perform::RecipeName& fileName, int32_t& soundsReplaced,
	           Chunks* chunks = nullptr);

	/// The palette the Recipe's sounds come from: the genre's built-in one, with the card's override over it if the
	/// last genre pick read one.
	[[nodiscard]] const Palette& palette() const;

	/// A genre change's preload (spec §9.7): reads `genre`'s palette override into the override not in use, as a genre
	/// pick does, leaving the playing one alone. Returns an error only when the card fails.
	Error stagePalette(const Genre& genre);
	/// The palette stagePalette() read: `genre`'s built-in one, with the card's override over it.
	[[nodiscard]] const Palette& stagedPalette() const { return overrides_[1 - activeOverride_].palette(); }
	/// The genre change commits: the staged override becomes the Recipe's.
	void adoptStagedPalette(const Genre& genre) {
		activeOverride_ = 1 - activeOverride_;
		overrideGenre_ = &genre;
	}

private:
	/// Two overrides: a genre pick reads into the one not in use, which takes over only once the roll succeeds, so a
	/// failed pick keeps the old one.
	PaletteOverride overrides_[2];
	int32_t activeOverride_ = 0;
	const Genre* overrideGenre_ = nullptr;
	/// Where a genre pick rolls its sound picks before they replace `sounds`.
	SoundPicks stagedSounds_;
};

/// In SDRAM: it is large (two palette overrides and two sets of sound picks) and touched only a few times a step.
/// `.sdram_bss` is zero-filled at boot and never gets a constant initializer's values, so every field must be correct
/// at zero until rollFromGenre() writes it: `exists` false, the first override active, no override genre. A field
/// whose default isn't zero must be set in rollFromGenre() before anything reads it.
extern CurrentRecipe currentRecipe;

#if ENABLE_SYSEX_TEST
/// Test builds ("reset recipe" on the test port, which start_state sends): puts the Current Recipe back as it is
/// after boot, all zero, so no Recipe exists and GENRE ▸ opens on the default genre again. Only outside Perform and
/// with the runner idle: nothing may hold the Recipe, its picks or its palette then.
void forgetCurrentRecipe();
#endif

/// Fresh entropy from a hardware timer.
uint32_t freshEntropy();

/// A loaded Recipe file's sound picks checked against the card (spec §16.6): replaceMissingSounds() with fresh
/// entropy, `soundsReplaced` the picks it re-rolled. With a runner job's `chunks` each check and folder read is a chunk
/// boundary: a cancel returns ABORTED_BY_USER. A card error returns the card's error. Either way `sounds` may be
/// partly replaced: work on a copy.
Error replaceMissingCardSounds(const Genre& genre, const Palette& palette, SoundPicks& sounds, int32_t& soundsReplaced,
                               Chunks* chunks = nullptr);

} // namespace deluge::generative::firmware
