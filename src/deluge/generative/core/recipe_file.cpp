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

#include "generative/core/recipe_file.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/palette_file.h"
#include "generative/core/variants.h"
#include "generative/core/xml_tags.h"

namespace deluge::generative {

namespace {

constexpr const char* kScaleNames[kNumScales] = {
    "major",   "minor",        "dorian",        "phrygian",       "lydian", "mixolydian",
    "locrian", "melodicMinor", "harmonicMinor", "hungarianMinor", "marva",  "arabian",
};

constexpr const char* kMacroNames[kNumMacros] = {"energy", "density", "complexity", "motion"};
/// A part's Part bias per macro (spec §3.3), in macro units.
constexpr const char* kBiasNames[kNumMacros] = {"energyBias", "densityBias", "complexityBias", "motionBias"};

/// A whole number's decimal text. 21 characters hold any int64_t with its sign and terminator.
struct Number {
	explicit Number(int64_t value) {
		char digits[20];
		int32_t count = 0;
		const bool negative = value < 0;
		uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
		do {
			digits[count++] = static_cast<char>('0' + magnitude % 10);
			magnitude /= 10;
		} while (magnitude != 0);
		int32_t at = 0;
		if (negative) {
			text[at++] = '-';
		}
		while (count > 0) {
			text[at++] = digits[--count];
		}
		text[at] = 0;
	}
	char text[21];
};

/// A card path with XML's special characters entity-encoded: at most 6 characters for each one of a path.
struct Encoded {
	explicit Encoded(const char* raw) {
		int32_t at = 0;
		for (const char* c = raw; *c != 0; c++) {
			const char* entity = *c == '&'    ? "&amp;"
			                     : *c == '<'  ? "&lt;"
			                     : *c == '>'  ? "&gt;"
			                     : *c == '"'  ? "&quot;"
			                     : *c == '\'' ? "&apos;"
			                                  : nullptr;
			if (entity == nullptr) {
				text[at++] = *c;
				continue;
			}
			for (const char* e = entity; *e != 0; e++) {
				text[at++] = *e;
			}
		}
		text[at] = 0;
	}
	char text[kMaxSoundPath * 6];
};

void writeNumber(RecipeFileWriter& writer, const char* name, int64_t value) {
	writer.attribute(name, Number(value).text);
}

/// A variant set's child: `<variant set="skank" pick="plain" allow="sevenths,plain" />`, `allow` left out when every
/// variant is allowed (spec §16.1).
void writeVariant(RecipeFileWriter& writer, const VariantSet& set, uint8_t pick, uint8_t allowed) {
	writer.openTag("variant");
	writer.attribute("set", set.name);
	writer.attribute("pick", set.variants[pick < set.variants.size() ? pick : 0].name);
	allowed = allowedIn(set, allowed);
	if (allowed != allVariants(set)) {
		char list[kMaxVariants * 24] = {};
		int32_t at = 0;
		for (size_t v = 0; v < set.variants.size(); v++) {
			if (((allowed >> v) & 1U) == 0) {
				continue;
			}
			for (const char* c = at == 0 ? "" : ","; *c != 0; c++) {
				list[at++] = *c;
			}
			for (const char* c = set.variants[v].name; *c != 0 && at < static_cast<int32_t>(sizeof(list)) - 2; c++) {
				list[at++] = *c;
			}
		}
		list[at] = 0;
		writer.attribute("allow", list);
	}
	writer.closeEmptyTag();
}

/// Reads a Recipe file's tags into a RecipeFile.
class RecipeParser {
public:
	explicit RecipeParser(RecipeFile& out) : out_(out) {}

	RecipeFileRead parse(std::string_view text) {
		xml::Tags tags(text);
		xml::Tag tag;
		if (!tags.next(tag) || tag.closing || tag.name != kRecipeFileTag) {
			return RecipeFileRead::Unreadable;
		}
		RecipeFileRead read = root(tag);
		if (read == RecipeFileRead::UnknownGenre || read == RecipeFileRead::Unreadable) {
			return read;
		}
		if (tag.selfClosing) {
			return read;
		}
		// Children until the root's closing tag. Anything below them is skipped by depth.
		int32_t depth = 0;
		while (tags.next(tag)) {
			if (tag.closing) {
				if (depth == 0) {
					return tag.name == kRecipeFileTag ? read : RecipeFileRead::Unreadable;
				}
				depth--;
				continue;
			}
			if (depth == 0 && !child(tag)) {
				return RecipeFileRead::Unreadable;
			}
			if (!tag.selfClosing) {
				depth++;
			}
		}
		return RecipeFileRead::Unreadable; // cut short, or a syntax error
	}

private:
	RecipeFileRead root(const xml::Tag& tag) {
		std::string_view value;
		char name[32];
		if (!xml::attribute(tag, "genre", value) || xml::decode(value, name) < 0) {
			return RecipeFileRead::Unreadable;
		}
		const Genre* genre = findGenre(name);
		if (genre == nullptr) {
			return RecipeFileRead::UnknownGenre;
		}
		out_ = RecipeFile{};
		Recipe& recipe = out_.recipe;
		recipe.genre = genre;
		recipe.macros = genre->defaults;
		recipe.swing = genre->facts.swing;
		// A set the file leaves out is drawn fresh within every variant (drawUnsetVariants(), spec §16.1).
		for (size_t v = 0; v < genre->variantSets.size() && v < kMaxVariantSets; v++) {
			recipe.variants.pick[v] = kUnsetVariant;
			recipe.variants.allowed[v] = allVariants(genre->variantSets[v]);
		}
		int64_t version = genre->version;
		int64_t tempo = recipe.tempo;
		int64_t swing = recipe.swing;
		int64_t interval = recipe.swingInterval;
		int64_t key = recipe.keyRoot;
		int64_t seed = 0;
		if (!number(tag, "genreVersion", 0, UINT16_MAX, version) || !number(tag, "tempo", kMinTempo, kMaxTempo, tempo)
		    || !number(tag, "swing", -kMaxSwing, kMaxSwing, swing)
		    || !number(tag, "swingInterval", 0, kMaxSwingInterval, interval) || !number(tag, "key", 0, 11, key)
		    || !number(tag, "seed", 0, UINT32_MAX, seed)) {
			return RecipeFileRead::Unreadable;
		}
		recipe.tempo = static_cast<int32_t>(tempo);
		recipe.swing = static_cast<int8_t>(swing);
		recipe.swingInterval = static_cast<uint8_t>(interval);
		recipe.keyRoot = static_cast<uint8_t>(key);
		out_.seed = static_cast<uint32_t>(seed);
		out_.genreVersion = static_cast<uint16_t>(version);
		if (xml::attribute(tag, "scale", value)) {
			int32_t scale = 0;
			while (scale < kNumScales && value != kScaleNames[scale]) {
				scale++;
			}
			if (scale == kNumScales) {
				return RecipeFileRead::Unreadable;
			}
			recipe.scale = static_cast<Scale>(scale);
		}
		return version == genre->version ? RecipeFileRead::Ok : RecipeFileRead::OtherVersion;
	}

	/// One of the root's children. False if a value it holds isn't one.
	bool child(const xml::Tag& tag) {
		if (tag.name == "macros") {
			for (int32_t m = 0; m < kNumMacros; m++) {
				int64_t units = out_.recipe.macros.value[m];
				if (!number(tag, kMacroNames[m], 0, kMacroFull, units)) {
					return false;
				}
				out_.recipe.macros.set(static_cast<Macro>(m), static_cast<int32_t>(units));
			}
			return true;
		}
		if (tag.name == "variant") {
			variant(tag);
			return true;
		}
		std::string_view name;
		if (!xml::attribute(tag, "name", name)) {
			return true; // a part or lane with no name, or an element a newer firmware writes: skipped
		}
		if (tag.name == "part") {
			const Genre& genre = *out_.recipe.genre;
			for (int32_t p = 0; p < partCount(genre); p++) {
				if (equalsIgnoringCase(name, genre.parts[p].name)) {
					int64_t take = out_.takes.take[p];
					int64_t octave = 0;
					if (!number(tag, "take", 0, kMaxTakes - 1, take)
					    || !number(tag, "octave", -kMaxOctaveShift, kMaxOctaveShift, octave)) {
						return false;
					}
					for (int32_t m = 0; m < kNumMacros; m++) {
						int64_t bias = 0; // a file from before Part bias plays the Recipe value
						if (!number(tag, kBiasNames[m], -kMacroFull, kMacroFull, bias)) {
							return false;
						}
						out_.recipe.bias.set(p, static_cast<Macro>(m), static_cast<int32_t>(bias));
					}
					out_.takes.take[p] = static_cast<uint8_t>(take);
					// A shift this part can't take (another genre version's Register, a hand edit) is clamped to the
					// ones it can, so no note leaves MIDI's range (spec §7.3).
					out_.recipe.octave[p] =
					    static_cast<int8_t>(octaveRange(genre.parts[p]).clamp(static_cast<int32_t>(octave)));
					sound(tag, out_.sounds.part[p]);
				}
			}
		}
		else if (tag.name == "lane") {
			for (int32_t l = 0; l < kNumLanes; l++) {
				if (equalsIgnoringCase(name, lanePaletteName(static_cast<Lane>(l)))) {
					sound(tag, out_.sounds.lane[l]);
				}
			}
		}
		return true;
	}

	/// A `<variant>`: a set by its stable name, its pick and the variants it allows (spec §16.1). An unknown set is
	/// skipped, an unknown name in `allow` dropped, and a pick this genre's set doesn't have left for
	/// drawUnsetVariants().
	void variant(const xml::Tag& tag) {
		const Genre& genre = *out_.recipe.genre;
		std::string_view value;
		const int32_t s = xml::attribute(tag, "set", value) ? findVariantSet(genre, value) : -1;
		if (s < 0) {
			return;
		}
		const VariantSet& set = genre.variantSets[s];
		VariantPicks& picks = out_.recipe.variants;
		if (xml::attribute(tag, "allow", value)) {
			uint8_t allowed = 0;
			while (!value.empty()) {
				const size_t comma = value.find(',');
				const int32_t v = findVariant(set, value.substr(0, comma));
				allowed |= v >= 0 ? static_cast<uint8_t>(1U << v) : 0;
				value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
			}
			picks.allowed[s] = allowedIn(set, allowed);
		}
		const int32_t pick = xml::attribute(tag, "pick", value) ? findVariant(set, value) : -1;
		picks.pick[s] = pick >= 0 ? static_cast<uint8_t>(pick) : kUnsetVariant;
	}

	/// The tag's sound path into `pick`, which stays empty if it has none, or one too long for a pick.
	static void sound(const xml::Tag& tag, SoundPath& pick) {
		std::string_view value;
		if (xml::attribute(tag, "sound", value) && xml::decode(value, pick.text) < 0) {
			pick.clear();
		}
	}

	/// Reads attribute `key` as a whole number into `out`, clamped to `least`..`most`. `out` keeps its value if the tag
	/// doesn't have it. False if the value isn't a whole number.
	static bool number(const xml::Tag& tag, std::string_view key, int64_t least, int64_t most, int64_t& out) {
		std::string_view text;
		if (!xml::attribute(tag, key, text)) {
			return true;
		}
		int64_t value = 0;
		if (!xml::wholeNumber(text, INT64_MAX / 16, value)) {
			return false;
		}
		out = value < least ? least : value > most ? most : value;
		return true;
	}

	RecipeFile& out_;
};

} // namespace

const char* scaleFileName(Scale scale) {
	const auto index = static_cast<int32_t>(scale);
	return index < kNumScales ? kScaleNames[index] : kScaleNames[0];
}

void writeRecipeFile(RecipeFileWriter& writer, const RecipeFile& file) {
	const Recipe& recipe = file.recipe;
	const Genre& genre = *recipe.genre;
	writer.openTag(kRecipeFileTag);
	writer.attribute("genre", genre.name);
	writeNumber(writer, "genreVersion", genre.version);
	writeNumber(writer, "tempo", recipe.tempo);
	writeNumber(writer, "swing", recipe.swing);
	writeNumber(writer, "swingInterval", recipe.swingInterval);
	writeNumber(writer, "key", recipe.keyRoot);
	writer.attribute("scale", scaleFileName(recipe.scale));
	writeNumber(writer, "seed", file.seed);
	writer.endAttributes();

	writer.openTag("macros");
	for (int32_t m = 0; m < kNumMacros; m++) {
		writeNumber(writer, kMacroNames[m], recipe.macros.value[m]);
	}
	writer.closeEmptyTag();

	for (int32_t p = 0; p < partCount(genre); p++) {
		writer.openTag("part");
		writer.attribute("name", genre.parts[p].name);
		writeNumber(writer, "take", file.takes.take[p]);
		writeNumber(writer, "octave", recipe.octave[p]);
		for (int32_t m = 0; m < kNumMacros; m++) {
			writeNumber(writer, kBiasNames[m], recipe.bias.value[p][m]);
		}
		if (!file.sounds.part[p].empty()) {
			writer.attribute("sound", Encoded(file.sounds.part[p].c_str()).text);
		}
		writer.closeEmptyTag();
	}
	for (int32_t l = 0; l < kNumLanes; l++) {
		if (file.sounds.lane[l].empty()) {
			continue;
		}
		writer.openTag("lane");
		writer.attribute("name", lanePaletteName(static_cast<Lane>(l)));
		writer.attribute("sound", Encoded(file.sounds.lane[l].c_str()).text);
		writer.closeEmptyTag();
	}
	for (size_t s = 0; s < genre.variantSets.size() && s < kMaxVariantSets; s++) {
		writeVariant(writer, genre.variantSets[s], recipe.variants.pick[s], recipe.variants.allowed[s]);
	}
	writer.closeTag(kRecipeFileTag);
}

RecipeFileRead parseRecipeFile(std::string_view text, RecipeFile& out) {
	return RecipeParser(out).parse(text);
}

} // namespace deluge::generative
