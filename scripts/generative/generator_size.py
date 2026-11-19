"""sizeof(Generator) and sizeof(PartSpec) on ARM, against H18 and the figure spec §20 states.

    python3 scripts/generative/generator_size.py

The harness builds for the host, where int32_t is int rather than long, so the size that counts is the firmware
compiler's. It compiles a probe with arm-none-eabi-g++ -fsyntax-only that names Size<sizeof(...)>, an incomplete
template, and reads each size from the error. Exits 1 if the Generator is over the static_assert in generator.h (H18)
or differs from §20's "The core is N B": when it changes, update that sentence (implementation notes, "Capacity
is sized by construction"). check_all.py runs it.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arm_tools

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
HEADER = os.path.join(REPO, "src", "deluge", "generative", "core", "generator.h")
SPEC = os.path.join(REPO, "docs", "generative", "spec", "20-budgets.md")
PROBE = """\
#include "generative/core/generator.h"
template <int N>
struct Size;
Size<sizeof(deluge::generative::Generator)> generatorSize;
Size<sizeof(deluge::generative::PartSpec)> partSpecSize;
"""


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def sizes_from(errors):
    """{variable: size} from the compiler's "aggregate 'Size<N> name' has incomplete type" errors."""
    return {name: int(n) for n, name in re.findall(r"Size<(\d+)>\s+(\w+)", errors)}


def limit_from(header):
    m = re.search(
        r"static_assert\(sizeof\(Generator\)\s*<=\s*(\d+)\s*\*\s*1024", header
    )
    return int(m.group(1)) * 1024


def spec_figure(spec_text):
    m = re.search(r"The core is ([\d,]+) B", spec_text)
    return int(m.group(1).replace(",", ""))


def measure(repo=REPO):
    proc = subprocess.run(
        [
            arm_tools.find("g++"),
            "-std=gnu++23",
            "-fsyntax-only",
            "-mcpu=cortex-a9",
            "-I",
            "src/deluge",
            "-I",
            "src",
            "-x",
            "c++",
            "-",
        ],
        input=PROBE,
        capture_output=True,
        text=True,
        cwd=repo,
        check=False,
    )
    sizes = sizes_from(proc.stderr)
    if "generatorSize" not in sizes:
        raise RuntimeError(
            "the probe didn't compile as expected:\n" + proc.stderr[-2000:]
        )
    return sizes


def verdict(size, limit, spec):
    """(ok, lines) for a Generator of `size` bytes."""
    lines = [
        f"sizeof(Generator) {size:,} B on ARM; H18 limit {limit:,} B, {limit - size:,} B left"
    ]
    ok = True
    if size > limit:
        ok = False
        lines.append(f"FAIL: {size - limit:,} B over H18 (spec §20, H18)")
    if size != spec:
        ok = False
        lines.append(
            f'FAIL: spec §20 says {spec:,} B: update spec §20\'s sentence "The core is N B" and its history'
        )
    return ok, lines


def main():
    sizes = measure()
    ok, lines = verdict(
        sizes["generatorSize"], limit_from(read(HEADER)), spec_figure(read(SPEC))
    )
    lines.insert(1, f"sizeof(PartSpec) {sizes['partSpecSize']:,} B on ARM")
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
