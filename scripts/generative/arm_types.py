"""Compiles the portable Generator code with the firmware's compiler, syntax only, in about a second.

    python3 scripts/generative/arm_types.py [FILE.cpp ...]

The harness builds the portable code (src/deluge/generative/core/, perform/, runner/) for the host, where int32_t is
int. On ARM it is long, so a call that mixes the two (`std::clamp(x, 0, kMacroFull)` with an int32_t x) passes
./dbt test --generator and CI, and fails only the Release build. This catches that without a build: each .cpp, and
each .h on its own (-x c++: some headers no portable .cpp includes, only the firmware), through arm-none-eabi-g++
-fsyntax-only with the firmware's language flags and its -Werror=switch. Without FILEs it checks every portable .cpp
and .h. Exits 1 on any error. check_all.py runs it first.
"""

import concurrent.futures
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arm_tools

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
# Under src/deluge/generative/; firmware/ is the firmware's own.
PORTABLE = ("core", "perform", "runner")
# The firmware's flags that change what compiles (CMakeLists.txt): its CPU, signed plain char, no RTTI, and a switch
# missing an enumerator as an error.
FLAGS = [
    "-std=gnu++23",
    "-fsyntax-only",
    "-mcpu=cortex-a9",
    "-fsigned-char",
    "-fno-rtti",
    "-D_GNU_SOURCE",
    "-Werror=switch",
    "-I",
    "src/deluge",
    "-I",
    "src",
]


def sources(repo=REPO):
    """Every portable .cpp and .h, relative to the repo, sorted."""
    found = []
    for part in PORTABLE:
        for ext in ("cpp", "h"):
            pattern = os.path.join(
                repo, "src", "deluge", "generative", part, "**", f"*.{ext}"
            )
            found += glob.glob(pattern, recursive=True)
    return sorted(os.path.relpath(path, repo) for path in found)


def command(gxx, path):
    """The compiler call for one file: a header is compiled as C++ on its own."""
    language = ["-x", "c++"] if path.endswith(".h") else []
    return [gxx, *FLAGS, *language, path]


def check(paths, gxx, repo=REPO, jobs=None):
    """{path: the compiler's errors} for each path that fails to compile."""

    def one(path):
        proc = subprocess.run(
            command(gxx, path), cwd=repo, capture_output=True, text=True, check=False
        )
        return path, (proc.stderr.strip() or "failed") if proc.returncode else None

    with concurrent.futures.ThreadPoolExecutor(jobs or os.cpu_count() or 2) as pool:
        return {path: err for path, err in pool.map(one, paths) if err}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    paths = argv or sources()
    try:
        gxx = arm_tools.find("g++")
    except FileNotFoundError as e:
        print(f"arm_types: {e}", file=sys.stderr)
        return 1
    failed = check(paths, gxx)
    for errors in failed.values():
        print(errors, file=sys.stderr)
    print(f"{len(paths)} files on ARM, {len(failed)} with errors")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
