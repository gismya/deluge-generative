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

// The little XML reader the card files Generative mode reads share (palette override files, Recipe files): a tag at a
// time, with its attributes still in the text. Portable, so the harness tests the same code. It reads what the stock
// XMLSerializer writes and what a player types by hand; it is not a general XML parser (no text content, no CDATA).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace deluge::generative::xml {

inline bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool isNameChar(char c) {
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
inline bool attribute(const Tag& tag, std::string_view key, std::string_view& value) {
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

/// Decodes XML's five entities in an attribute's raw value into `out`, with a terminator. Returns the decoded length,
/// or -1 if the value holds an '&' that starts none of them or doesn't fit.
inline int32_t decode(std::string_view value, std::span<char> out) {
	size_t used = 0;
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
				return -1;
			}
		}
		if (used + 1 >= out.size()) {
			return -1;
		}
		out[used++] = c;
	}
	if (out.empty()) {
		return -1;
	}
	out[used] = 0;
	return static_cast<int32_t>(used);
}

/// Reads `text` as a whole number, optionally negative, into `out`, saturating at ±`most`. False if it is anything
/// else (empty, a sign alone, a stray character).
inline bool wholeNumber(std::string_view text, int64_t most, int64_t& out) {
	bool negative = !text.empty() && text.front() == '-';
	if (negative) {
		text.remove_prefix(1);
	}
	if (text.empty()) {
		return false;
	}
	int64_t value = 0;
	for (char c : text) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value > most ? value : value * 10 + (c - '0');
	}
	value = value > most ? most : value;
	out = negative ? -value : value;
	return true;
}

} // namespace deluge::generative::xml
