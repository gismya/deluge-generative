"""The step section check (implementation notes, "The step's cost in the tick"): every branch out of
.text.generative_step whose target is a function in the deluge::generative namespace must land inside the section.
Such a function is a step callee that lost its GEN_STEP_CODE mark (or that LTO outlined anew): mark it.

    python3 scripts/generative/step_section.py [build/Release/deluge.elf]

Reads the section's range from the map beside the ELF (the linker merges the section into .text), disassembles that
range with arm-none-eabi-objdump and takes each bl/b/cbz target by its address, then names it with arm-none-eabi-nm:
the names objdump prints aren't parsed, since a template's '>' can sit anywhere in one. Calls to other code (the
division helpers, memmove, std::) are listed but allowed. Exits 1 if a deluge::generative target lies outside.
check_all.py runs it on build/Release after the release build.
"""

import os
import re
import subprocess
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arm_tools

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_ELF = os.path.join(REPO, "build", "Release", "deluge.elf")
SECTION = ".text.generative_step"
# Thumb-2 branches with a target address: b, bl, blx, the conditional b<cc> (with .n or .w), cbz and cbnz.
BRANCH = re.compile(
    r"^(?:bl|blx|b|cbn?z|b(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al))(?:\.n|\.w)?$"
)
TARGET = re.compile(r"(?:^|,\s*)([0-9a-f]+)(?:\s+<|\s*$)")


def section_range(map_text):
    """(start, end) of the step section in a GNU ld map. Entries of size 0 (an object whose step code was
    discarded) hold no code and are skipped. More than one other entry fails: the check reads one range, so a second
    contributor would go unchecked."""
    matches = [
        m
        for m in re.finditer(
            r"^ " + re.escape(SECTION) + r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)",
            map_text,
            re.MULTILINE,
        )
        if int(m.group(2), 16)
    ]
    if not matches:
        raise ValueError(f"no {SECTION} in the map")
    if len(matches) > 1:
        raise ValueError(
            f"{len(matches)} {SECTION} entries in the map: the check reads one range"
        )
    m = matches[0]
    start, size = int(m.group(1), 16), int(m.group(2), 16)
    return start, start + size


def branch_targets(disassembly):
    """The target address of each branch in `objdump -d` output, in order."""
    targets = []
    for line in disassembly.splitlines():
        parts = line.split("\t")
        if len(parts) < 4 or not BRANCH.match(parts[2].strip()):
            continue
        m = TARGET.search(parts[3])
        if m:
            targets.append(int(m.group(1), 16))
    return targets


def text_symbols(nm_text):
    """The code symbols in `nm -n -C` output, indexed by address (arm_tools.CodeSymbols)."""
    return arm_tools.CodeSymbols(arm_tools.parse_nm(nm_text))


def in_generative(name):
    """Whether a demangled function name is in deluge::generative, not merely taking or instantiated on its types: the
    qualified name before the parameter list, with template arguments dropped, starts with it."""
    head, depth = [], 0
    for ch in name:
        if ch == "(" and depth == 0:
            break
        if ch == "<":
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif depth == 0:
            head.append(ch)
    words = "".join(head).split()
    return bool(words) and words[-1].startswith("deluge::generative::")


@dataclass
class Report:
    generative_outside: dict = field(default_factory=dict)  # name -> its address
    other_outside: set = field(default_factory=set)


def check(section, targets, symbols):
    start, end = section
    report = Report()
    for target in targets:
        if start <= target < end:
            continue
        symbol = symbols.at(target)
        name = symbol.name if symbol else f"0x{target:x}"
        if in_generative(name):
            report.generative_outside[name] = symbol.address
        else:
            report.other_outside.add(name)
    return report


def run(tool, *args):
    return subprocess.run(
        [arm_tools.find(tool), *args], capture_output=True, text=True, check=True
    ).stdout


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) > 1 or (argv and argv[0].startswith("-")):
        print(__doc__.strip())
        return 2
    elf = argv[0] if argv else DEFAULT_ELF
    map_path = os.path.splitext(elf)[0] + ".map"
    with open(map_path) as fh:
        start, end = section_range(fh.read())
    disassembly = run(
        "objdump",
        "-d",
        f"--start-address=0x{start:x}",
        f"--stop-address=0x{end:x}",
        elf,
    )
    report = check(
        (start, end),
        branch_targets(disassembly),
        text_symbols(run("nm", "-n", "-C", elf)),
    )
    print(f"{SECTION}: 0x{start:x}..0x{end:x} ({end - start:,} B)")
    print(f"deluge::generative targets outside it: {len(report.generative_outside)}")
    for name, address in sorted(
        report.generative_outside.items(), key=lambda item: item[1]
    ):
        print(f"  0x{address:x} {name}  (mark it GEN_STEP_CODE)")
    others = sorted({re.split(r"\(", n, maxsplit=1)[0] for n in report.other_outside})
    print("other targets outside it (allowed): " + (", ".join(others) or "none"))
    return 1 if report.generative_outside else 0


if __name__ == "__main__":
    sys.exit(main())
