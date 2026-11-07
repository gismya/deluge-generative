"""Mechanical review rules for Generative mode, run as pre-commit hooks and in CI.

    python3 scripts/lint/generative_rules.py <rule> <files...>

Rules (`.pre-commit-config.yaml` decides which files each one sees):

no-float            no `float`/`double` and no float literals (`0.5`, `1e3`, `.5f`) in the Generator's portable code
                    (ADR 0004).
portable-includes   the portable code includes only its own headers and the standard headers listed below, so it
                    builds in the harness (tests/spec/generator/CMakeLists.txt) as well as in the firmware.
no-stdio            no libc stdio in the portable code: in the firmware, `snprintf` and friends pull in file I/O and
                    break the link.
perform-l10n        Perform's UI passes no string literal to an OLED call as the text to show, also as an operand of a ?:
                    there (`displayPopup(on ? "ON" : "OFF")`) or wrapped onto the next line; shown text comes from l10n.

Comments are ignored by every rule, and string literals by all but perform-l10n.
"""

import argparse
import re
import sys
from pathlib import Path

# Quoted includes the portable code may use: the harness builds only these trees.
PORTABLE_ROOTS = ("generative/core/", "generative/perform/", "generative/runner/")

# Standard headers the portable code may use. Each is header-only or freestanding in both builds, with no I/O,
# allocation or float math. Add one here on purpose, in review.
PORTABLE_STD_HEADERS = {
    "algorithm",
    "array",
    "bit",
    "cassert",
    "cstddef",
    "cstdint",
    "cstring",
    "climits",
    "compare",
    "concepts",
    "initializer_list",
    "iterator",
    "limits",
    "numeric",
    "optional",
    "span",
    "string_view",
    "tuple",
    "type_traits",
    "utility",
}

STDIO_CALL = re.compile(
    r"(?<![.>\w])(?:std::)?(printf|sprintf|snprintf|vprintf|vsprintf|vsnprintf|fprintf|vfprintf|puts|fputs|fgets|getchar|"
    r"putchar|fopen|fclose|fread|fwrite|fseek|ftell|fflush|scanf|sscanf|fscanf|perror)\s*\("
)
STDIO_STREAM = re.compile(r"\bstd::(cout|cerr|clog|cin)\b")
FLOAT_TYPE = re.compile(r"\b(float|double)\b")
# A float literal: digits with a point (`0.5`, `1.`, `.5f`) or an exponent (`1e3`). Not after a word character or a
# point, so member access (`a.b`), hex (`0x1e3`) and identifiers (`x1e3`) don't match, nor do integers (`10u`). Run
# on text with strings blanked, so version strings don't match either.
FLOAT_LITERAL = re.compile(
    r"(?<![\w.'])(?:\d[\d']*\.[\d']*(?:[eE][+-]?\d+)?|\.\d[\d']*(?:[eE][+-]?\d+)?|\d[\d']*[eE][+-]?\d+)[fFlL]?"
)
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# Display calls that take the text to show as their first argument (hid/display/: oled.h, display.h, canvas.h).
# freezeWithError() is left out: it takes an error code, not text for the player. The rule reads the whole file, so a
# literal clang-format wrapped onto the next line is caught too.
OLED_CALL = re.compile(
    r"\b(drawString\w*|drawScreenTitle|drawPermanentPopupLookingText|displayPopup|displayNotification|"
    r"displayLoadingAnimationText|displayWorkingAnimation|popupText\w*|setText\w*|setScrollingText|consoleText\w*)"
    r"\s*\("
)

# Known literals, each keyed by its file and its line's code (stripped of indentation and comments), with the reason
# it stays. Keep this list short: fix new cases instead.
L10N_EXEMPT = {
    # The status line's external-clock tag, written before this rule existed. Move it to l10n when the file is next
    # touched for the status line.
    (
        "src/deluge/gui/views/perform_view.cpp",
        'canvas.drawStringAlignRight("EXT", kStatusY, kTextSpacingX, kTextSpacingY, right);',
    ),
}


def strip(source: str, keep_strings: bool) -> str:
    """Blanks out comments (and, unless keep_strings, string and char literals), keeping newlines and columns."""
    out = []
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = source.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in source[i:j]))
            i = j
        elif c == '"' or (
            c == "'" and not (i and source[i - 1].isalnum())
        ):  # not a 1'000 digit separator
            j = i + 1
            while j < n and source[j] != c and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            j = min(j + 1, n)
            literal = source[i:j]
            out.append(
                literal if keep_strings else c + " " * max(len(literal) - 2, 0) + c
            )
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_no_float(path, text):
    for no, line in enumerate(text.splitlines(), 1):
        if m := FLOAT_TYPE.search(line):
            yield (
                no,
                f"`{m.group(1)}` in portable Generator code: decisions use integer math only (ADR 0004)",
            )
        elif m := FLOAT_LITERAL.search(line):
            yield (
                no,
                f"float literal `{m.group(0)}` in portable Generator code: decisions use integer math only (ADR 0004)",
            )


def check_portable_includes(path, text):
    for no, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        kind, header = m.groups()
        if kind == '"' and not header.startswith(PORTABLE_ROOTS):
            yield (
                no,
                f'#include "{header}": portable code includes only {", ".join(PORTABLE_ROOTS)} headers',
            )
        elif kind == "<" and header not in PORTABLE_STD_HEADERS:
            yield (
                no,
                f"#include <{header}>: not in the portable standard headers (scripts/lint/generative_rules.py)",
            )


def check_no_stdio(path, text):
    for no, line in enumerate(text.splitlines(), 1):
        if m := STDIO_CALL.search(line) or STDIO_STREAM.search(line):
            yield (
                no,
                f"`{m.group(1)}` in portable code: libc stdio breaks the firmware link; build text by hand",
            )


def _skip_literal(text, i):
    """The index just past the string or char literal that starts at text[i]."""
    quote, i = text[i], i + 1
    while i < len(text) and text[i] != quote and text[i] != "\n":
        i += 2 if text[i] == "\\" else 1
    return i + 1


ENCODING_PREFIX = re.compile(r"(?<![\w])(u8|u|U|L)$")


def shown_literals(text, start):
    """(start, end) of each string literal in the first argument of the call whose "(" ends just before `start`, that
    the call would show: the argument itself, or an operand of a ?: there, also nested, in parentheses, behind a cast
    (`(char const*)"HOLD"`) or with an encoding prefix (`u8"HOLD"`). A literal passed to another call inside the
    argument (`l10n::get(...)`, `lookup("KEY")`) isn't shown text."""
    found = []
    # One per open bracket inside the argument: (whether it only groups, not a call or index; the code character
    # before it). A grouping one can be a cast.
    groups = []
    prev = "("  # the last code character before i, as if the argument opened a group
    before_cast = None  # what came before a group that just closed: if a literal follows, the group was a cast
    before_word = prev  # what came before the identifier i is in
    i = start
    while i < len(text):
        c = text[i]
        if c in "\"'" and not (c == "'" and text[i - 1].isalnum()):
            end = _skip_literal(text, i)
            ahead = prev
            if c == '"' and ENCODING_PREFIX.search(text, start, i):
                ahead = before_word
            elif prev == ")" and before_cast is not None:
                ahead = before_cast
            if c == '"' and all(g for g, _ in groups) and ahead in "(?:":
                found.append((i, end))
            prev, before_cast, i = c, None, end
            continue
        if (c.isalnum() or c == "_") and not (
            text[i - 1].isalnum() or text[i - 1] == "_"
        ):
            before_word = prev
        closed = None
        if c in "([{":
            groups.append((c == "(" and not (prev.isalnum() or prev in "_)]>"), prev))
        elif c in ")]}":
            if not groups:
                break  # the end of the first argument
            closed = groups.pop()
        elif c == "," and not groups:
            break
        if not c.isspace():
            prev = c
            before_cast = closed[1] if closed and closed[0] else None
        i += 1
    return found


def check_perform_l10n(path, text):
    lines = text.splitlines()
    for call in OLED_CALL.finditer(text):
        for begin, end in shown_literals(text, call.end()):
            literal = text[begin:end]
            no = text.count("\n", 0, begin) + 1
            if (
                re.search(r"[A-Za-z]", literal)
                and (path, lines[no - 1].strip()) not in L10N_EXEMPT
            ):
                yield (
                    no,
                    f"{call.group(1)}({literal}...): shown text goes through l10n::get(l10n::String::...)",
                )


RULES = {
    "no-float": (check_no_float, False),
    "portable-includes": (check_portable_includes, True),
    "no-stdio": (check_no_stdio, False),
    "perform-l10n": (check_perform_l10n, True),
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("rule", choices=RULES)
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    check, keep_strings = RULES[args.rule]
    failed = False
    for name in args.files:
        path = Path(name).as_posix()
        text = strip(
            Path(name).read_text(encoding="utf-8", errors="replace"), keep_strings
        )
        for no, message in check(path, text):
            print(f"{path}:{no}: {message}")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
