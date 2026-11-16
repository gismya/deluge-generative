"""Finds the firmware's ARM toolchain (arm-none-eabi-*) for the scripts that read the firmware's code or types, and
reads arm-none-eabi-nm's listing for them (step_section.py, decode_pad_crash.py, ./dbt sizediff).

On PATH inside `./dbt exec`; outside it, under the toolchain folder ./dbt uses: $DBT_TOOLCHAIN_PATH, else the main
checkout's for a linked worktree (as ./dbt finds it), else this checkout's, at toolchain/v<REQUIRED_VERSION>/<host>/.
"""

import bisect
import collections
import glob
import os
import re
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def toolchain_roots(repo=REPO, environ=None):
    environ = os.environ if environ is None else environ
    roots = []
    if environ.get("DBT_TOOLCHAIN_PATH"):
        roots.append(environ["DBT_TOOLCHAIN_PATH"])
    common = subprocess.run(
        ["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    if common:
        roots.append(os.path.dirname(common))
    roots.append(repo)
    return roots


def required_version(repo=REPO):
    try:
        with open(os.path.join(repo, "toolchain", "REQUIRED_VERSION")) as fh:
            return fh.read().strip()
    except OSError:
        return "*"


def find(name, repo=REPO, environ=None):
    """The path of arm-none-eabi-NAME (g++, objdump, nm ...). Raises FileNotFoundError naming where it looked."""
    tool = f"arm-none-eabi-{name}"
    on_path = shutil.which(tool)
    if on_path:
        return on_path
    version = required_version(repo)
    looked = []
    for root in toolchain_roots(repo, environ):
        pattern = os.path.join(
            root, "toolchain", f"v{version}", "*", "arm-none-eabi-gcc", "bin", tool
        )
        looked.append(pattern)
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    raise FileNotFoundError(
        f"no {tool} on PATH or at "
        + ", ".join(looked)
        + " (run ./dbt once to fetch it)"
    )


# A symbol from nm: its address (a Thumb function's bit 0 cleared), its size (None when nm gives none), its type
# letter and its name (demangled with -C).
Symbol = collections.namedtuple("Symbol", "address size kind name")
CODE_KINDS = "tTwW"
# nm's address and size columns are 8 or 16 hex digits, so a one-letter type can't pass for a size.
NM_LINE = re.compile(r"^([0-9a-f]{8,16}) (?:([0-9a-f]{8,16}) )?(\w) (.+)$")


def parse_nm(text):
    """[Symbol] from `nm [-n] [-S] [-C]` output, in its order."""
    symbols = []
    for line in text.splitlines():
        m = NM_LINE.match(line)
        if m:
            address = int(m[1], 16)
            if m[3] in CODE_KINDS:
                address &= ~1
            symbols.append(Symbol(address, int(m[2], 16) if m[2] else None, m[3], m[4]))
    return symbols


class CodeSymbols:
    """The code symbols of an nm listing, sorted by address and indexed once."""

    def __init__(self, symbols):
        self.symbols = sorted(
            (s for s in symbols if s.kind in CODE_KINDS), key=lambda s: s.address
        )
        self.starts = [s.address for s in self.symbols]

    def at(self, address):
        """The code symbol `address` lies in, by start: the last one at or before it. None before the first."""
        i = bisect.bisect_right(self.starts, address) - 1
        return self.symbols[i] if i >= 0 else None
