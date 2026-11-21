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

#include "generative/perform/recipe_name.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/text.h"
#include <cstring>

namespace deluge::generative::perform {

namespace {

/// `name` without a `.XML` extension, in any case.
std::string_view withoutExtension(std::string_view name) {
	constexpr std::string_view kExtension = ".XML";
	if (name.size() > kExtension.size()
	    && equalsIgnoringCase(name.substr(name.size() - kExtension.size()), kExtension)) {
		name.remove_suffix(kExtension.size());
	}
	return name;
}

char upper(char c) {
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

} // namespace

bool RecipeName::set(std::string_view name, int32_t stemLength) {
	clear();
	if (name.empty() || name.size() > static_cast<size_t>(kMaxRecipeNameChars)) {
		return false;
	}
	std::memcpy(text, name.data(), name.size());
	text[name.size()] = 0;
	const auto length = static_cast<int32_t>(name.size());
	stem = static_cast<uint8_t>(stemLength < 0 || stemLength > length ? length : stemLength);
	return true;
}

void defaultRecipeName(const Genre& genre, uint32_t seed, RecipeName& out) {
	char code[kSeedCodeChars + 1];
	seedCode(seed, code);
	char name[kMaxRecipeNameChars + 1];
	Text(name).add(genre.oledName).add(" ").add(code);
	out.set(name);
}

void NameFamily::add(std::string_view fileName, std::string_view stem) {
	const std::string_view name = withoutExtension(fileName);
	if (name.size() < stem.size() || !equalsIgnoringCase(name.substr(0, stem.size()), stem)) {
		return;
	}
	if (name.size() == stem.size()) {
		stemTaken = true;
		return;
	}
	const char letter = upper(name[stem.size()]);
	if (name.size() == stem.size() + 1 && letter >= 'A' && letter <= 'Z') {
		takenLetters |= 1U << (letter - 'A');
	}
}

bool suggestRecipeName(const RecipeName& base, bool offerStem, const NameFamily& family, RecipeName& out) {
	const std::string_view stem = base.stemText();
	if (offerStem && !family.stemTaken && stem.size() == std::strlen(base.text)) {
		return out.set(stem);
	}
	char from = 'A';
	if (std::strlen(base.text) > stem.size()) {
		from = static_cast<char>(upper(base.text[stem.size()]) + 1);
	}
	if (stem.size() + 1 > static_cast<size_t>(kMaxRecipeNameChars)) {
		out.clear();
		return false;
	}
	for (char letter = from; letter <= 'Z'; letter++) {
		if ((family.takenLetters & (1U << (letter - 'A'))) == 0) {
			char name[kMaxRecipeNameChars + 1];
			std::memcpy(name, stem.data(), stem.size());
			name[stem.size()] = letter;
			return out.set({name, stem.size() + 1}, static_cast<int32_t>(stem.size()));
		}
	}
	out.clear();
	return false;
}

bool adoptSavedName(const RecipeName& suggestion, std::string_view saved, RecipeName& remembered) {
	if (!suggestion.empty() && equalsIgnoringCase(saved, suggestion.text)) {
		remembered = suggestion;
		return true;
	}
	return remembered.set(saved);
}

} // namespace deluge::generative::perform
