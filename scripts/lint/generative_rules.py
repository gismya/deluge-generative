"""Mechanical review rules for Generative mode, run as pre-commit hooks and in CI.

    python3 scripts/lint/generative_rules.py <rule> <files...>

Rules (`.pre-commit-config.yaml` decides which files each one sees):

no-float            no `float`/`double` in the Generator's portable code (ADR 0004).
portable-includes   the portable code includes only its own headers and the standard headers listed below, so it
                    builds in the harness (tests/spec/generator/CMakeLists.txt) as well as in the firmware.
no-stdio            no libc stdio in the portable code: in the firmware, `snprintf` and friends pull in file I/O and
                    break the link.
perform-l10n        Perform's UI passes no string literal straight to an OLED call; shown text comes from l10n.

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
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# OLED calls that take the text to show as their first argument.
OLED_CALL = re.compile(
    r"\b(drawString\w*|drawScreenTitle|displayPopup|popupText\w*|setText|setScrollingText|"
    r"displayLoadingAnimationText)\s*\(\s*(\"(?:[^\"\\]|\\.)*\")"
)

# Known literals, each with the reason it stays. Keep this list short: fix new cases instead.
L10N_EXEMPT = {
    # The status line's external-clock tag, written before this rule existed. Move it to l10n when the file is next
    # touched for the status line.
    ("src/deluge/gui/views/perform_view.cpp", '"EXT"'),
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


def check_no_float(path, lines):
    for no, line in lines:
        if m := FLOAT_TYPE.search(line):
            yield (
                no,
                f"`{m.group(1)}` in portable Generator code: decisions use integer math only (ADR 0004)",
            )


def check_portable_includes(path, lines):
    for no, line in lines:
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


def check_no_stdio(path, lines):
    for no, line in lines:
        if m := STDIO_CALL.search(line) or STDIO_STREAM.search(line):
            yield (
                no,
                f"`{m.group(1)}` in portable code: libc stdio breaks the firmware link; build text by hand",
            )


def check_perform_l10n(path, lines):
    for no, line in lines:
        for m in OLED_CALL.finditer(line):
            literal = m.group(2)
            if re.search(r"[A-Za-z]", literal) and (path, literal) not in L10N_EXEMPT:
                yield (
                    no,
                    f"{m.group(1)}({literal}...): shown text goes through l10n::get(l10n::String::...)",
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
        for no, message in check(path, enumerate(text.splitlines(), 1)):
            print(f"{path}:{no}: {message}")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
