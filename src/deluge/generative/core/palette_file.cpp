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

#include "generative/core/palette_file.h"
#include "generative/core/genre.h"

namespace deluge::generative {

namespace {

constexpr char lowerChar(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameIgnoringCase(std::string_view a, std::string_view b) {
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

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isNameChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
	       || c == '.' || c == ':';
}

/// One tag of the file: `<name attributes>`, `<name attributes/>` or `</name>`.
struct Tag {
	std::string_view name;
	std::string_view attributes;
	bool closing = false;
	bool selfClosing = false;
};

/// Reads the tags of an XML file one at a time, skipping text, comments, declarations and processing instructions.
class Tags {
public:
	explicit Tags(std::string_view text) : text_(text) {}

	/// The next tag, or false at the end of the text or on a syntax error (then failed() is true).
	bool next(Tag& tag) {
		while (true) {
			size_t open = text_.find('<', at_);
			if (open == std::string_view::npos) {
				return false;
			}
			std::string_view rest = text_.substr(open);
			if (rest.starts_with("<!--")) {
				size_t end = text_.find("-->", open + 4);
				if (end == std::string_view::npos) {
					return fail();
				}
				at_ = end + 3;
				continue;
			}
			if (rest.starts_with("<?") || rest.starts_with("<!")) {
				size_t end = text_.find('>', open);
				if (end == std::string_view::npos) {
					return fail();
				}
				at_ = end + 1;
				continue;
			}
			size_t end = findTagEnd(open + 1);
			if (end == std::string_view::npos) {
				return fail();
			}
			std::string_view inside = text_.substr(open + 1, end - open - 1);
			at_ = end + 1;
			tag = Tag{};
			if (!inside.empty() && inside.front() == '/') {
				tag.closing = true;
				inside.remove_prefix(1);
			}
			if (!inside.empty() && inside.back() == '/') {
				tag.selfClosing = true;
				inside.remove_suffix(1);
			}
			size_t nameEnd = 0;
			while (nameEnd < inside.size() && isNameChar(inside[nameEnd])) {
				nameEnd++;
			}
			if (nameEnd == 0 || (tag.closing && tag.selfClosing)) {
				return fail();
			}
			tag.name = inside.substr(0, nameEnd);
			tag.attributes = inside.substr(nameEnd);
			return true;
		}
	}

	[[nodiscard]] bool failed() const { return failed_; }

private:
	/// The '>' that ends a tag starting at `from`, skipping quoted attribute values.
	size_t findTagEnd(size_t from) const {
		char quote = 0;
		for (size_t i = from; i < text_.size(); i++) {
			char c = text_[i];
			if (quote != 0) {
				quote = c == quote ? 0 : quote;
			}
			else if (c == '"' || c == '\'') {
				quote = c;
			}
			else if (c == '>') {
				return i;
			}
			else if (c == '<') {
				return std::string_view::npos;
			}
		}
		return std::string_view::npos;
	}

	bool fail() {
		failed_ = true;
		return false;
	}

	std::string_view text_;
	size_t at_ = 0;
	bool failed_ = false;
};

/// An attribute's raw (still entity-encoded) value, or false if the tag doesn't have it.
bool attribute(const Tag& tag, std::string_view key, std::string_view& value) {
	std::string_view rest = tag.attributes;
	while (true) {
		size_t i = 0;
		while (i < rest.size() && isSpace(rest[i])) {
			i++;
		}
		size_t nameStart = i;
		while (i < rest.size() && isNameChar(rest[i])) {
			i++;
		}
		if (i == nameStart) {
			return false;
		}
		std::string_view name = rest.substr(nameStart, i - nameStart);
		while (i < rest.size() && isSpace(rest[i])) {
			i++;
		}
		if (i >= rest.size() || rest[i] != '=') {
			return false;
		}
		i++;
		while (i < rest.size() && isSpace(rest[i])) {
			i++;
		}
		if (i >= rest.size() || (rest[i] != '"' && rest[i] != '\'')) {
			return false;
		}
		char quote = rest[i++];
		size_t valueEnd = rest.find(quote, i);
		if (valueEnd == std::string_view::npos) {
			return false;
		}
		if (name == key) {
			value = rest.substr(i, valueEnd - i);
			return true;
		}
		rest = rest.substr(valueEnd + 1);
	}
}

} // namespace

const char* lanePaletteName(Lane lane) {
	constexpr const char* kNames[kNumLanes] = {"kick",    "snare", "clap",  "rim",    "closedHat",
	                                           "openHat", "ride",  "crash", "tom",    "perc1",
	                                           "perc2",   "riser", "fall",  "impact", "siren"};
	return lane == Lane::None ? "" : kNames[static_cast<int32_t>(lane)];
}

const char* rolePaletteName(Role role) {
	constexpr const char* kNames[kNumRoles] = {"drums", "bass", "chords", "pad", "lead", "fx"};
	return kNames[static_cast<int32_t>(role)];
}

/// Fills a PaletteOverride from a file's tags. Its storage is the override's own; nothing allocates.
class PaletteParser {
public:
	explicit PaletteParser(PaletteOverride& out) : out_(out) {}

	bool parse(std::string_view text) {
		Tags tags(text);
		Tag tag;
		std::string_view open[kMaxDepth];
		int32_t depth = 0;
		bool sawPalette = false;
		while (tags.next(tag)) {
			if (tag.closing) {
				if (depth == 0 || open[depth - 1] != tag.name) {
					return false;
				}
				depth--;
				if (depth == 1 && !finishSection()) {
					return false;
				}
				continue;
			}
			if (depth == 0) {
				if (tag.name != "palette" || sawPalette) {
					return false;
				}
				sawPalette = true;
			}
			else if (depth == 1) {
				if (!startSection(tag)) {
					return false;
				}
			}
			else if (depth == 2 && !addItem(tag)) {
				return false;
			}
			if (tag.selfClosing) {
				if (depth == 1 && !finishSection()) {
					return false;
				}
				continue;
			}
			if (depth == kMaxDepth) {
				return false;
			}
			open[depth++] = tag.name;
		}
		return !tags.failed() && depth == 0 && sawPalette;
	}

private:
	static constexpr int32_t kMaxDepth = 8;
	enum class Section : uint8_t { None, Part, Role, Lane };

	bool startSection(const Tag& tag) {
		section_ = Section::None;
		std::string_view name;
		if (!attribute(tag, "name", name)) {
			return tag.name != "part" && tag.name != "role" && tag.name != "lane"; // a known section needs a name
		}
		entriesStart_ = out_.entriesUsed_;
		tagsStart_ = out_.tagsUsed_;
		foldersStart_ = out_.foldersUsed_;
		if (tag.name == "part") {
			sectionName_ = copy(name);
			section_ = sectionName_ != nullptr ? Section::Part : Section::None;
			return sectionName_ != nullptr;
		}
		if (tag.name == "role") {
			for (int32_t r = 0; r < kNumRoles; r++) {
				if (sameIgnoringCase(name, rolePaletteName(static_cast<Role>(r)))) {
					role_ = static_cast<Role>(r);
					section_ = Section::Role;
				}
			}
		}
		else if (tag.name == "lane") {
			for (int32_t l = 0; l < kNumLanes; l++) {
				if (sameIgnoringCase(name, lanePaletteName(static_cast<Lane>(l)))) {
					lane_ = static_cast<Lane>(l);
					section_ = Section::Lane;
				}
			}
		}
		return true; // an unknown section or name is skipped
	}

	bool addItem(const Tag& tag) {
		if (section_ == Section::None) {
			return true;
		}
		std::string_view value;
		int32_t weight = 1;
		std::string_view weightText;
		if (attribute(tag, "weight", weightText)) {
			weight = 0;
			for (char c : weightText) {
				if (c < '0' || c > '9' || weight > 255) {
					return false;
				}
				weight = weight * 10 + (c - '0');
			}
			if (weightText.empty() || weight > 255) {
				return false;
			}
		}
		if (tag.name == "preset") {
			if (!attribute(tag, "path", value) || out_.entriesUsed_ >= PaletteOverride::kMaxEntries) {
				return false;
			}
			const char* path = copy(value);
			out_.entries_[out_.entriesUsed_++] = {path, static_cast<uint8_t>(weight)};
			return path != nullptr;
		}
		if (section_ != Section::Lane) {
			return true;
		}
		if (tag.name == "folder") {
			if (!attribute(tag, "path", value) || out_.foldersUsed_ >= PaletteOverride::kMaxFolders) {
				return false;
			}
			const char* path = copy(value);
			out_.folders_[out_.foldersUsed_++] = path;
			return path != nullptr;
		}
		if (tag.name == "tag" || tag.name == "file") {
			bool file = tag.name == "file";
			if (!attribute(tag, file ? "path" : "text", value) || out_.tagsUsed_ >= PaletteOverride::kMaxTags) {
				return false;
			}
			const char* text = copy(value);
			if (text == nullptr || isExactPathTag(text) != file) {
				return false; // a file needs a folder in its path, and a tag mustn't have one
			}
			out_.tags_[out_.tagsUsed_++] = {text, static_cast<uint8_t>(weight)};
			return true;
		}
		return true;
	}

	bool finishSection() {
		std::span<const PaletteEntry> entries{out_.entries_ + entriesStart_,
		                                      static_cast<size_t>(out_.entriesUsed_ - entriesStart_)};
		switch (section_) {
		case Section::Part:
			if (out_.partsUsed_ >= PaletteOverride::kMaxPartEntries) {
				return false;
			}
			out_.parts_[out_.partsUsed_++] = {sectionName_, entries};
			break;
		case Section::Role:
			if (out_.rolesUsed_ >= kNumRoles) {
				return false;
			}
			out_.roles_[out_.rolesUsed_++] = {role_, entries};
			break;
		case Section::Lane:
			if (out_.lanesUsed_ >= kNumLanes) {
				return false;
			}
			out_.lanes_[out_.lanesUsed_++] = {
			    .lane = lane_,
			    .folders = {out_.folders_ + foldersStart_, static_cast<size_t>(out_.foldersUsed_ - foldersStart_)},
			    .tags = {out_.tags_ + tagsStart_, static_cast<size_t>(out_.tagsUsed_ - tagsStart_)},
			    .synths = entries,
			};
			break;
		case Section::None:
			break;
		}
		section_ = Section::None;
		return true;
	}

	static bool isExactPathTag(const char* text) {
		for (; *text != 0; text++) {
			if (*text == '/') {
				return true;
			}
		}
		return false;
	}

	/// Copies an attribute value into the override's text, decoding XML's five entities. Null if it doesn't fit.
	const char* copy(std::string_view value) {
		char* start = out_.text_ + out_.textUsed_;
		int32_t used = out_.textUsed_;
		for (size_t i = 0; i < value.size(); i++) {
			char c = value[i];
			if (c == '&') {
				constexpr std::string_view kEntities[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
				constexpr char kChars[] = {'&', '<', '>', '"', '\''};
				bool decoded = false;
				for (int32_t e = 0; e < 5 && !decoded; e++) {
					if (value.substr(i).starts_with(kEntities[e])) {
						c = kChars[e];
						i += kEntities[e].size() - 1;
						decoded = true;
					}
				}
				if (!decoded) {
					return nullptr;
				}
			}
			if (used >= PaletteOverride::kTextBytes - 1) {
				return nullptr;
			}
			out_.text_[used++] = c;
		}
		if (used >= PaletteOverride::kTextBytes) {
			return nullptr;
		}
		out_.text_[used++] = 0;
		out_.textUsed_ = used;
		return start;
	}

	PaletteOverride& out_;
	Section section_ = Section::None;
	const char* sectionName_ = nullptr;
	Role role_ = Role::Bass;
	Lane lane_ = Lane::Kick;
	int32_t entriesStart_ = 0;
	int32_t tagsStart_ = 0;
	int32_t foldersStart_ = 0;
};

void PaletteOverride::clear(const Palette& builtIn) {
	textUsed_ = entriesUsed_ = tagsUsed_ = foldersUsed_ = partsUsed_ = rolesUsed_ = lanesUsed_ = 0;
	palette_ = builtIn;
}

bool PaletteOverride::parse(std::string_view text, const Palette& builtIn) {
	clear(builtIn);
	if (!PaletteParser(*this).parse(text)) {
		clear(builtIn);
		return false;
	}
	// The override's entries first, then each built-in entry it doesn't replace. Lookups take the first match.
	int32_t overriddenParts = partsUsed_;
	for (const PartSounds& part : builtIn.parts) {
		bool replaced = false;
		for (int32_t i = 0; i < overriddenParts; i++) {
			replaced = replaced || sameIgnoringCase(parts_[i].part, part.part);
		}
		if (!replaced) {
			if (partsUsed_ >= kMaxPartEntries) {
				clear(builtIn);
				return false;
			}
			parts_[partsUsed_++] = part;
		}
	}
	int32_t overriddenRoles = rolesUsed_;
	for (const RoleSounds& role : builtIn.roles) {
		bool replaced = false;
		for (int32_t i = 0; i < overriddenRoles; i++) {
			replaced = replaced || roles_[i].role == role.role;
		}
		if (!replaced && rolesUsed_ < kNumRoles) {
			roles_[rolesUsed_++] = role;
		}
	}
	int32_t overriddenLanes = lanesUsed_;
	for (const LaneSounds& lane : builtIn.lanes) {
		bool replaced = false;
		for (int32_t i = 0; i < overriddenLanes; i++) {
			replaced = replaced || lanes_[i].lane == lane.lane;
		}
		if (!replaced && lanesUsed_ < kNumLanes) {
			lanes_[lanesUsed_++] = lane;
		}
	}
	palette_ = Palette{
	    .parts = {parts_, static_cast<size_t>(partsUsed_)},
	    .roles = {roles_, static_cast<size_t>(rolesUsed_)},
	    .lanes = {lanes_, static_cast<size_t>(lanesUsed_)},
	};
	return true;
}

} // namespace deluge::generative
