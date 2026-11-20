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

#include "generative/firmware/recipe_files.h"
#include "definitions.h"
#include "extern.h"
#include "fatfs/ff.h"
#include "generative/firmware/card_sounds.h"
#include "generative/firmware/card_text.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/runner.h"
#include "io/midi/test_port.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <cstring>

namespace deluge::generative::firmware {

namespace {

/// What a save writes: the Current Recipe copied before the first card access, so a transition's handover or a
/// steering press during the card waits can't mix two Recipes in one file (spec §16.4). In SDRAM: it holds a set of
/// sound picks.
PLACE_SDRAM_BSS RecipeFile snapshot;

/// The stock serializer as the core's Recipe file writer.
class SerializerWriter final : public RecipeFileWriter {
public:
	explicit SerializerWriter(Serializer& serializer) : serializer_(serializer) {}
	void openTag(const char* tag) override { serializer_.writeOpeningTagBeginning(tag); }
	void attribute(const char* name, const char* value) override { serializer_.writeAttribute(name, value); }
	void endAttributes() override { serializer_.writeOpeningTagEnd(); }
	void closeEmptyTag() override { serializer_.closeTag(); }
	void closeTag(const char* tag) override { serializer_.writeClosingTag(tag); }

private:
	Serializer& serializer_;
};

/// Counts RECIPES/' files into a stem's family.
class FamilyVisitor final : public FileVisitor {
public:
	FamilyVisitor(perform::NameFamily& family, std::string_view stem) : family_(family), stem_(stem) {}
	void file(const char* name) override { family_.add(name, stem_); }

private:
	perform::NameFamily& family_;
	std::string_view stem_;
};

/// `RECIPES/<name>.XML` into `out`. False if it doesn't fit.
bool recipePath(std::string_view name, char (&out)[perform::kMaxRecipeNameChars + 16]) {
	const size_t folder = std::strlen(kRecipeFolder);
	if (folder + 1 + name.size() + 5 > sizeof(out)) {
		return false;
	}
	std::memcpy(out, kRecipeFolder, folder);
	out[folder] = '/';
	std::memcpy(out + folder + 1, name.data(), name.size());
	std::memcpy(out + folder + 1 + name.size(), ".XML", 5);
	return true;
}

} // namespace

bool recipeSaveAllowed(bool inCardRoutine) {
	return !inCardRoutine && !currentlyAccessingCard && !runner.busy() && currentRecipe.exists;
}

bool suggestRecipeName(perform::RecipeName& out, Error& error) {
	error = Error::NONE;
	const bool remembered = !currentRecipe.name.empty();
	perform::RecipeName base;
	if (remembered) {
		base = currentRecipe.name;
	}
	else {
		perform::defaultRecipeName(*currentRecipe.recipe.genre, currentRecipe.seed, base);
	}
	perform::NameFamily family;
	FamilyVisitor visitor(family, base.stemText());
	CardFolders folders;
	if (!folders.list(kRecipeFolder, visitor)) {
		error = StorageManager::initSD();
		error = error != Error::NONE ? error : Error::SD_CARD;
		out.clear();
		return false;
	}
	return perform::suggestRecipeName(base, !remembered, family, out);
}

Error saveCurrentRecipe(const char* path, std::string_view name, const perform::RecipeName& suggestion,
                        bool mayOverwrite) {
	if (!currentRecipe.exists) {
		return Error::UNSPECIFIED;
	}
	snapshot.recipe = currentRecipe.recipe;
	snapshot.sounds = currentRecipe.sounds;
	snapshot.seed = currentRecipe.seed;
	snapshot.takes = currentRecipe.takes;
	const uint32_t generation = currentRecipe.generation;

	Error error = StorageManager::initSD();
	if (error != Error::NONE) {
		return error;
	}
	const FRESULT made = f_mkdir(kRecipeFolder);
	if (made != FR_OK && made != FR_EXIST) {
		return fresultToDelugeErrorCode(made);
	}
	const bool existed = StorageManager::fileExists(path);
	if (existed && !mayOverwrite) {
		return Error::FILE_ALREADY_EXISTS;
	}
	error = StorageManager::createXMLFile(path, smSerializer, mayOverwrite, false);
	if (error != Error::NONE) {
		return error;
	}
	SerializerWriter writer(smSerializer);
	writeRecipeFile(writer, snapshot);
	error = smSerializer.closeFileAfterWriting(path, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", kRecipeFileEnd);
	if (error != Error::NONE) {
		if (!existed) {
			f_unlink(path); // nothing left behind; a pulled card refuses this too, and that's fine
		}
		return error;
	}
	if (currentRecipe.generation == generation) {
		perform::adoptSavedName(suggestion, name, currentRecipe.name);
	}
	return Error::NONE;
}

QuickSave quickSaveRecipe(perform::RecipeName& saved, Error& error) {
	if (!suggestRecipeName(saved, error)) {
		return error != Error::NONE ? QuickSave::Failed : QuickSave::NoFreeName;
	}
	char path[perform::kMaxRecipeNameChars + 16];
	if (!recipePath(saved.text, path)) {
		error = Error::UNSPECIFIED; // can't happen: a name that fits a RecipeName fits a path
		return QuickSave::Failed;
	}
	error = saveCurrentRecipe(path, saved.text, saved, false);
	return error == Error::NONE ? QuickSave::Saved : QuickSave::Failed;
}

RecipeFileRead readRecipeFile(const char* path, RecipeFile& out, Error& error) {
	struct Parse final : CardTextReader {
		explicit Parse(RecipeFile& out) : out(out) {}
		void read(std::string_view text) override { result = parseRecipeFile(text, out); }
		RecipeFile& out;
		RecipeFileRead result = RecipeFileRead::Unreadable;
	} parse{out};
	const CardText read = readCardText(path, kMaxRecipeFileBytes, parse, error);
	return read == CardText::Read ? parse.result : RecipeFileRead::Unreadable;
}

} // namespace deluge::generative::firmware
