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

// Recipe files (spec §16.1–§16.3): what a file holds, its genre version, and the names SAVE offers.

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe_file.h"
#include "generative/perform/recipe_name.h"

#include "cppspec.hpp"

#include <cstring>
#include <memory>
#include <set>
#include <string>

using namespace deluge::generative;
using namespace harness;
using deluge::generative::perform::adoptSavedName;
using deluge::generative::perform::defaultRecipeName;
using deluge::generative::perform::NameFamily;
using deluge::generative::perform::RecipeName;
using deluge::generative::perform::suggestRecipeName;

namespace {

/// Writes a Recipe file's XML as the stock XMLSerializer lays it out: attributes on their own lines, tabs to indent,
/// and createXMLFile()'s declaration first.
class XmlText final : public RecipeFileWriter {
public:
	XmlText() { text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"; }
	void openTag(const char* tag) override {
		indents();
		text += "<";
		text += tag;
		depth_++;
	}
	void attribute(const char* name, const char* value) override {
		text += "\n";
		indents();
		text += name;
		text += "=\"";
		text += value;
		text += "\"";
	}
	void endAttributes() override { text += ">\n"; }
	void closeEmptyTag() override {
		text += " />\n";
		depth_--;
	}
	void closeTag(const char* tag) override {
		depth_--;
		indents();
		text += "</";
		text += tag;
		text += ">\n";
	}
	std::string text;

private:
	void indents() { text.append(depth_, '\t'); }
	int32_t depth_ = 0;
};

/// A Current Recipe as Perform or the Compose screen leaves it: rolled facts, sound picks off the factory card with
/// the pack, a seed, and takes and macros moved away from where a roll puts them.
std::unique_ptr<RecipeFile> rolledFile(const Genre& genre, uint32_t entropy, FakeCard& card) {
	auto file = std::make_unique<RecipeFile>();
	file->recipe = newRecipe(genre, entropy);
	rollSounds(genre, *genre.palette, file->sounds, entropy, card);
	file->seed = 0xF00D0000U + entropy * 7919U;
	for (int32_t p = 0; p < partCount(genre); p++) {
		file->takes.take[p] = static_cast<uint8_t>((entropy + p * 5) % kMaxTakes);
		// A shift the part takes, so the reload's clamp keeps it: -1, 0 or +1, where its range allows.
		file->recipe.octave[p] =
		    static_cast<int8_t>(octaveRange(genre.parts[p]).clamp(static_cast<int32_t>((entropy + p) % 3) - 1));
		// A Part bias on some macros, both signs, in units that aren't whole points.
		file->recipe.bias.set(p, static_cast<Macro>((entropy + p) % kNumMacros),
		                      (p % 2 == 0 ? 1 : -1) * static_cast<int32_t>(25 * (entropy + 3 * p) + 7));
	}
	file->recipe.macros.set(Macro::Energy, 17 * 25 + entropy);
	file->recipe.macros.set(Macro::Motion, kMacroFull);
	file->recipe.swing = static_cast<int8_t>(entropy % 2 == 0 ? -12 : 31);
	file->recipe.swingInterval = static_cast<uint8_t>(entropy % (kMaxSwingInterval + 1));
	file->recipe.tempo = 13377 + static_cast<int32_t>(entropy); // hundredths of a BPM, not whole ones
	return file;
}

std::string written(const RecipeFile& file) {
	XmlText xml;
	writeRecipeFile(xml, file);
	return xml.text;
}

bool samePicks(const SoundPicks& a, const SoundPicks& b) {
	for (int32_t p = 0; p < kMaxParts; p++) {
		if (std::strcmp(a.part[p].c_str(), b.part[p].c_str()) != 0) {
			return false;
		}
	}
	for (int32_t l = 0; l < kNumLanes; l++) {
		if (std::strcmp(a.lane[l].c_str(), b.lane[l].c_str()) != 0) {
			return false;
		}
	}
	return true;
}

/// A minimal file's text around `body`, for the reading rules.
std::string fileWith(const std::string& rootAttributes, const std::string& body = "") {
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<generativeRecipe " + rootAttributes + ">\n" + body
	       + "</generativeRecipe>\n";
}

std::string versionOf(const Genre& genre) {
	return std::to_string(genre.version);
}

NameFamily familyOf(std::initializer_list<const char*> files, const char* stem) {
	NameFamily family;
	for (const char* file : files) {
		family.add(file, stem);
	}
	return family;
}

std::string suggested(const RecipeName& base, bool offerStem, const NameFamily& family) {
	RecipeName out;
	return suggestRecipeName(base, offerStem, family, out) ? std::string(out.text) : std::string("<none>");
}

RecipeName named(const char* text) {
	RecipeName name;
	name.set(text);
	return name;
}

} // namespace

// clang-format off
describe recipe_file("Recipe files", $ {
	context("a save and a reload", _ {
		it("gives back the same Recipe, sound picks, seed and takes, for every genre", _ {
			FakeCard card = factoryCardWithPack();
			for (const Genre* genre : allGenres()) {
				for (uint32_t entropy = 0; entropy < 6; entropy++) {
					auto saved = rolledFile(*genre, entropy, card);
					auto loaded = std::make_unique<RecipeFile>();
					expect(parseRecipeFile(written(*saved), *loaded) == RecipeFileRead::Ok).to_be_true();
					expect(loaded->recipe == saved->recipe).to_be_true();
					expect(loaded->seed).to_equal(saved->seed);
					expect(loaded->takes == saved->takes).to_be_true();
					expect(samePicks(loaded->sounds, saved->sounds)).to_be_true();
				}
			}
		});

		it("keeps a card path with XML's special characters in it", _ {
			auto saved = rolledFile(genres::technoAcid, 1, *std::make_unique<FakeCard>());
			saved->sounds.part[0].set("SYNTHS/R&B", "Bass \"<1>\" & 'Lead'.XML");
			saved->sounds.lane[static_cast<int32_t>(Lane::Kick)].set("SAMPLES/DRUMS/A&B", "Kick.wav");
			auto loaded = std::make_unique<RecipeFile>();
			expect(parseRecipeFile(written(*saved), *loaded) == RecipeFileRead::Ok).to_be_true();
			expect(samePicks(loaded->sounds, saved->sounds)).to_be_true();
		});

		it("keeps a lane pick the player chose: any card sample, or a preset on an FX lane (spec §13.5)", _ {
			auto saved = rolledFile(genres::technoAcid, 2, *std::make_unique<FakeCard>());
			saved->sounds.lane[static_cast<int32_t>(Lane::ClosedHat)].set("MY SAMPLES/Loops", "Hat loop.aiff");
			saved->sounds.lane[static_cast<int32_t>(Lane::Riser)].set("SYNTHS/GENERATIVE/FX", "GEN Riser 2.XML");
			auto loaded = std::make_unique<RecipeFile>();
			expect(parseRecipeFile(written(*saved), *loaded) == RecipeFileRead::Ok).to_be_true();
			expect(std::string(loaded->sounds.lane[static_cast<int32_t>(Lane::Riser)].c_str()))
			    .to_equal(std::string("SYNTHS/GENERATIVE/FX/GEN Riser 2.XML"));
			expect(samePicks(loaded->sounds, saved->sounds)).to_be_true();
		});

		it("writes the genre's stable name and version, and the values a reader can see", _ {
			auto saved = rolledFile(genres::technoAcid, 2, *std::make_unique<FakeCard>());
			const std::string text = written(*saved);
			expect(text.find("genre=\"techno-acid\"") != std::string::npos).to_be_true();
			expect(text.find("genreVersion=\"" + versionOf(genres::technoAcid) + "\"") != std::string::npos).to_be_true();
			expect(text.find("tempo=\"13379\"") != std::string::npos).to_be_true();
			expect(text.find("seed=\"" + std::to_string(saved->seed) + "\"") != std::string::npos).to_be_true();
			// Part 1's bias, on Motion ((2 + 1) % 4): -(25 * 5 + 7), in units; its other macros' 0 too.
			expect(text.find("motionBias=\"-132\"") != std::string::npos).to_be_true();
			expect(text.find("energyBias=\"0\"") != std::string::npos).to_be_true();
		});

		it("holds nothing but the Recipe: no steering, homes, freezes, mutes, Arc or LENGTH", _ {
			const std::string text = written(*rolledFile(genres::technoAcid, 3, *std::make_unique<FakeCard>()));
			for (const char* word : {"freeze", "mute", "home", "arc", "length", "glide", "force", "hold"}) {
				expect(lower(text).find(word) == std::string::npos).to_be_true();
			}
		});

		it("names every part of every genre uniquely, so a file's parts find theirs", _ {
			for (const Genre* genre : allGenres()) {
				std::set<std::string> names;
				for (int32_t p = 0; p < partCount(*genre); p++) {
					expect(std::strlen(genre->parts[p].name)).to_be_greater_than(size_t{0});
					names.insert(lower(genre->parts[p].name));
				}
				expect(names.size()).to_equal(static_cast<size_t>(partCount(*genre)));
			}
		});
	});

	context("genre versions (spec §16.2)", _ {
		it("loads a file from another version of its genre and says so", _ {
			const Genre& acid = genres::technoAcid;
			auto loaded = std::make_unique<RecipeFile>();
			const std::string older = fileWith("genre=\"techno-acid\" genreVersion=\"" + std::to_string(acid.version - 1)
			                                   + "\" tempo=\"12000\" seed=\"5\"");
			expect(parseRecipeFile(older, *loaded) == RecipeFileRead::OtherVersion).to_be_true();
			expect(loaded->recipe.genre == &acid).to_be_true();
			expect(loaded->genreVersion).to_equal(static_cast<uint16_t>(acid.version - 1)); // the notice says OLDER
			expect(loaded->recipe.tempo).to_equal(12000);
			expect(loaded->seed).to_equal(5U);
			// A newer file's version, for the NEWER notice; a file without one is the genre's own.
			expect(parseRecipeFile(fileWith("genre=\"techno-acid\" genreVersion=\"" + std::to_string(acid.version + 1)
			                                + "\""),
			                       *loaded)
			       == RecipeFileRead::OtherVersion)
			    .to_be_true();
			expect(loaded->genreVersion).to_equal(static_cast<uint16_t>(acid.version + 1));
			expect(parseRecipeFile(fileWith("genre=\"techno-acid\""), *loaded) == RecipeFileRead::Ok).to_be_true();
			expect(loaded->genreVersion).to_equal(static_cast<uint16_t>(acid.version));
		});

		it("refuses a genre this firmware doesn't have, and Techno's parent table, with no alias", _ {
			auto loaded = std::make_unique<RecipeFile>();
			expect(parseRecipeFile(fileWith("genre=\"polka\" genreVersion=\"1\""), *loaded)
			       == RecipeFileRead::UnknownGenre).to_be_true();
			expect(parseRecipeFile(fileWith("genre=\"techno\" genreVersion=\"1\""), *loaded)
			       == RecipeFileRead::UnknownGenre).to_be_true();
		});
	});

	context("reading what it didn't write", _ {
		it("refuses text that isn't a Recipe file, or is cut short", _ {
			auto loaded = std::make_unique<RecipeFile>();
			const std::string whole = written(*rolledFile(genres::technoAcid, 4, *std::make_unique<FakeCard>()));
			expect(parseRecipeFile("", *loaded) == RecipeFileRead::Unreadable).to_be_true();
			expect(parseRecipeFile("<song></song>", *loaded) == RecipeFileRead::Unreadable).to_be_true();
			expect(parseRecipeFile(whole.substr(0, whole.size() / 2), *loaded) == RecipeFileRead::Unreadable).to_be_true();
			expect(parseRecipeFile(fileWith("tempo=\"12000\""), *loaded) == RecipeFileRead::Unreadable).to_be_true();
			const std::string acid = "genre=\"techno-acid\" genreVersion=\"" + versionOf(genres::technoAcid) + "\"";
			expect(parseRecipeFile(fileWith(acid + " tempo=\"fast\""), *loaded) == RecipeFileRead::Unreadable).to_be_true();
			expect(parseRecipeFile(fileWith(acid + " scale=\"blues\""), *loaded) == RecipeFileRead::Unreadable).to_be_true();
		});

		it("gives a file that leaves out swing the middle of the genre's swing range", _ {
			const Genre& house = genres::house;
			auto loaded = std::make_unique<RecipeFile>();
			expect(parseRecipeFile(fileWith("genre=\"house\" genreVersion=\"" + versionOf(house) + "\""), *loaded)
			       == RecipeFileRead::Ok)
			    .to_be_true();
			expect(int32_t{loaded->recipe.swing}).to_equal((house.facts.swingLow + house.facts.swingHigh) / 2);
			expect(int32_t{loaded->recipe.swing}).to_equal(15); // House's swing before its range (#498)
		});

		it("fills what a file leaves out from the genre, clamps what's out of range and skips what it doesn't know", _ {
			const Genre& acid = genres::technoAcid;
			auto loaded = std::make_unique<RecipeFile>();
			const std::string text = fileWith(
			    "genre=\"techno-acid\" genreVersion=\"" + versionOf(acid)
			        + "\" tempo=\"999999\" swing=\"-80\" swingInterval=\"9\" key=\"30\" mood=\"happy\"",
			    "<part name=\"ACID\" take=\"40\" octave=\"9\" sound=\"SYNTHS/X.XML\" />\n"
			    "<part name=\"drums\" take=\"1\" octave=\"-1\" densityBias=\"-320\" motionBias=\"-99999\" />\n"
			    "<part name=\"tuba\" take=\"1\" />\n<lane name=\"cowbell\" sound=\"A.wav\" />\n<extra />\n");
			expect(parseRecipeFile(text, *loaded) == RecipeFileRead::Ok).to_be_true();
			expect(loaded->recipe.tempo).to_equal(kMaxTempo);
			expect(static_cast<int32_t>(loaded->recipe.swing)).to_equal(-kMaxSwing);
			expect(static_cast<int32_t>(loaded->recipe.swingInterval)).to_equal(kMaxSwingInterval);
			expect(static_cast<int32_t>(loaded->recipe.keyRoot)).to_equal(11);
			expect(loaded->recipe.macros == acid.defaults).to_be_true();
			int32_t acidPart = -1;
			for (int32_t p = 0; p < partCount(acid); p++) {
				if (lower(acid.parts[p].name) == "acid") {
					acidPart = p;
				}
			}
			expect(acidPart).to_be_greater_than(-1);
			expect(static_cast<int32_t>(loaded->takes.take[acidPart])).to_equal(kMaxTakes - 1);
			expect(std::string(loaded->sounds.part[acidPart].c_str())).to_equal(std::string("SYNTHS/X.XML"));
			// An octave shift past what the part takes is clamped to its range; a part with no Register takes none.
			expect(static_cast<int32_t>(loaded->recipe.octave[acidPart]))
			    .to_equal(octaveRange(acid.parts[acidPart]).highest);
			for (int32_t p = 0; p < partCount(acid); p++) {
				if (!hasRegister(acid.parts[p].role)) {
					expect(static_cast<int32_t>(loaded->recipe.octave[p])).to_equal(0);
				}
			}
			// A Part bias is read in units and clamped to ±50 points; a part, or a macro, the file leaves out has 0.
			expect(loaded->recipe.bias.of(0, Macro::Density)).to_equal(-macroPoints(5));
			expect(loaded->recipe.bias.of(0, Macro::Motion)).to_equal(-kMacroFull);
			expect(loaded->recipe.bias.of(0, Macro::Energy)).to_equal(0);
			for (int32_t p = 1; p < partCount(acid); p++) {
				expect(loaded->recipe.bias.any(p)).to_be_false();
			}
		});
	});

	context("names (spec §16.3)", _ {
		it("names a never-saved Recipe after its genre and seed code", _ {
			RecipeName name;
			defaultRecipeName(genres::technoAcid, 0x0A2B3C4DU, name);
			expect(std::string(name.text)).to_equal(std::string("ACID TECHNO 052PF2D"));
			expect(static_cast<int32_t>(name.stem)).to_equal(static_cast<int32_t>(std::strlen(name.text)));
		});

		it("offers the default name while it's free, else its first free letter variation", _ {
			RecipeName base = named("ACID TECHNO 052PF2D");
			expect(suggested(base, true, familyOf({"OTHER.XML"}, base.text))).to_equal(std::string("ACID TECHNO 052PF2D"));
			expect(suggested(base, true, familyOf({"acid techno 052pf2d.xml", "ACID TECHNO 052PF2DA.XML"}, base.text)))
			    .to_equal(std::string("ACID TECHNO 052PF2DB"));
		});

		it("suggests the next free letter after the remembered name's, never the name itself", _ {
			expect(suggested(named("MY JAM"), false, familyOf({}, "MY JAM"))).to_equal(std::string("MY JAMA"));
			RecipeName lettered;
			lettered.set("MY JAMB", 6);
			expect(suggested(lettered, false, familyOf({"MY JAMB.XML", "MY JAMC.XML", "MY JAMAA.XML"}, "MY JAM")))
			    .to_equal(std::string("MY JAMD"));
			expect(suggested(lettered, false, familyOf({"MY JAMA.XML"}, "MY JAM"))).to_equal(std::string("MY JAMC"));
		});

		it("has no name to offer once Z is taken", _ {
			RecipeName lettered;
			lettered.set("MY JAMY", 6);
			expect(suggested(lettered, false, familyOf({"MY JAMZ.XML"}, "MY JAM"))).to_equal(std::string("<none>"));
		});

		it("keeps a seed code's last letter in the stem: a default name's letters come after it", _ {
			RecipeName base = named("DUB TECHNO 0Y8438B");
			expect(suggested(base, true, familyOf({"DUB TECHNO 0Y8438B.XML"}, base.text)))
			    .to_equal(std::string("DUB TECHNO 0Y8438BA"));
		});

		it("remembers the saved name, keeping the suggestion's stem when the player took it as offered", _ {
			RecipeName suggestion;
			suggestion.set("MY JAMC", 6);
			RecipeName remembered;
			expect(adoptSavedName(suggestion, "my jamc", remembered)).to_be_true();
			expect(std::string(remembered.text)).to_equal(std::string("MY JAMC"));
			expect(static_cast<int32_t>(remembered.stem)).to_equal(6);
			expect(adoptSavedName(suggestion, "NIGHT DRIVE", remembered)).to_be_true();
			expect(std::string(remembered.text)).to_equal(std::string("NIGHT DRIVE"));
			expect(static_cast<int32_t>(remembered.stem)).to_equal(11);
			const std::string tooLong(perform::kMaxRecipeNameChars + 1, 'X');
			expect(adoptSavedName(suggestion, tooLong, remembered)).to_be_false();
			expect(remembered.empty()).to_be_true();
		});
	});
});

CPPSPEC_SPEC(recipe_file)
