// The line diff the goldens show a failing dump with (line_diff.h): the shortest edit script, within a bound.

#include "line_diff.h"

#include "cppspec.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace harness;

namespace {

using Lines = std::vector<std::string>;

/// The script as text: each edit's op and line, "-b +x".
std::string scriptOf(const Lines& a, const Lines& b, int32_t maxEdits = 2000) {
	const std::optional<std::vector<DiffLine>> script = lineDiff(a, b, maxEdits);
	if (!script) {
		return "(none)";
	}
	std::string out;
	for (const DiffLine& edit : *script) {
		out += (out.empty() ? "" : " ") + std::string(1, edit.op) + (edit.op == '+' ? b[edit.b] : a[edit.a]);
	}
	return out;
}

/// A line per number, each its own text.
Lines numbered(int32_t from, int32_t count) {
	Lines lines;
	for (int32_t i = 0; i < count; i++) {
		lines.push_back(std::to_string(from + i));
	}
	return lines;
}

} // namespace

// clang-format off
describe line_diff("Line diff", $ {
	it("keeps every line of equal texts, and finds nothing to do for two empty ones", _ {
		expect(scriptOf({"a", "b"}, {"a", "b"})).to_equal(std::string(" a  b"));
		expect(scriptOf({}, {})).to_equal(std::string());
	});

	it("lists the shortest script: deletions before insertions where a line was replaced", _ {
		expect(scriptOf({"a", "b", "c"}, {"a", "x", "c"})).to_equal(std::string(" a -b +x  c"));
		expect(scriptOf({"a", "b", "c"}, {"a", "c"})).to_equal(std::string(" a -b  c"));
		expect(scriptOf({"a", "c"}, {"a", "b", "c"})).to_equal(std::string(" a +b  c"));
		expect(scriptOf({}, {"a"})).to_equal(std::string("+a"));
		expect(scriptOf({"a"}, {})).to_equal(std::string("-a"));
	});

	it("gives each edit its line in both texts", _ {
		const std::optional<std::vector<DiffLine>> script = lineDiff({"a", "b", "c"}, {"x", "a", "c"});
		expect(script.has_value()).to_be_true();
		const std::vector<DiffLine>& edits = *script;
		expect(edits.size()).to_equal(size_t{4});
		expect(edits[0].op).to_equal('+');
		expect(edits[0].b).to_equal(size_t{0});
		expect(edits[1].op).to_equal(' ');
		expect(edits[1].a).to_equal(size_t{0});
		expect(edits[1].b).to_equal(size_t{1});
		expect(edits[2].op).to_equal('-');
		expect(edits[2].a).to_equal(size_t{1});
		expect(edits[3].a).to_equal(size_t{2});
		expect(edits[3].b).to_equal(size_t{2});
	});

	it("gives up past maxEdits, and takes any maxEdits without sizing by it", _ {
		expect(scriptOf({"a", "b"}, {"x", "y"}, 3)).to_equal(std::string("(none)"));
		expect(scriptOf({"a", "b"}, {"x", "y"}, 4)).to_equal(std::string("-a -b +x +y"));
		// Bound by the texts' n + m: a maxEdits this size never reaches memory.
		expect(scriptOf({"a"}, {"b"}, std::numeric_limits<int32_t>::max())).to_equal(std::string("-a +b"));
	});

	it("diffs long texts with few changes", _ {
		Lines a = numbered(0, 20000);
		Lines b = a;
		b[10000] = "changed";
		const std::optional<std::vector<DiffLine>> script = lineDiff(a, b);
		expect(script.has_value()).to_be_true();
		int32_t changed = 0;
		for (const DiffLine& edit : *script) {
			changed += edit.op != ' ' ? 1 : 0;
		}
		expect(changed).to_equal(2);
		expect(scriptOf(numbered(0, 1500), numbered(5000, 1500))).to_equal(std::string("(none)"));
	});
});

CPPSPEC_SPEC(line_diff)
