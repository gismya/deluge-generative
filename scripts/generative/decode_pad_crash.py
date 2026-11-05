"""Decodes the crash the Deluge draws on its pads, and looks the addresses up in the build's ELF.

After a CPU fault (or FREEZE_WITH_ERROR), src/OSLikeStuff/fault_handler/fault_handler.c draws code addresses on the
main grid and a commit prefix in the sidebar. Transcribe the pads into a grid file, one line per pad row, top row
first, 18 characters per line (16 main columns, then the 2 sidebar columns):

    .  unlit      m  magenta    b  blue    g  green    c  cyan    r  red    y  yellow    w  white

Run inside ./dbt exec, which puts the arm-none-eabi tools on PATH:

    ./dbt exec "python3 scripts/generative/decode_pad_crash.py crash.txt --elf build/RelWithDebInfo/deluge.elf"

Without --elf it only decodes. With it, it checks that the ELF is the crashed build (its kCommitShort starts with
the sidebar's commit prefix) and names the function at each address.
"""

import argparse
import bisect
import os
import re
import shutil
import subprocess
import sys

ROWS, MAIN_COLUMNS, COLUMNS = 8, 16, 18

# What each group of 4 columns holds, by colour (printPointers() in fault_handler.c).
GROUP_MEANING = {
    "m": "faulting instruction (exception PC)",
    "b": "caller: SYS-mode LR - 2, inside the call instruction",
    "g": "stack hint: a code address found on the stack, - 2 (may be stale)",
    "c": "stack hint: a code address found on the stack, - 2 (may be stale)",
}

# Sidebar colour -> fault type. get_colours() is passed to drawByte() as (r, b, g), so green and blue swap on the
# pads: undefined instruction shows blue, prefetch abort green, reserved yellow, a soft fault magenta.
FAULT_TYPE = {
    "r": "data abort",
    "b": "undefined instruction",
    "g": "prefetch abort",
    "y": "reserved exception",
    "m": "soft fault: FREEZE_WITH_ERROR (the OLED shows the error code; no exception PC is drawn)",
    "c": "SVC",
    "w": "unknown fault type",
}


def read_grid(text):
    rows = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip().replace(" ", "")
        if line:
            rows.append(line.lower())
    if len(rows) != ROWS or any(len(r) != COLUMNS for r in rows):
        sys.exit(
            f"the grid needs {ROWS} lines of {COLUMNS} characters (top row first); got {[len(r) for r in rows]}"
        )
    bad = {ch for r in rows for ch in r} - set(".mbgcryw")
    if bad:
        sys.exit(f"unknown pad characters: {''.join(sorted(bad))}")
    return rows


def column(rows, x):
    """The column's byte and colours. The bottom pad is bit 7, the top pad bit 0 (drawByte sends bit 7 first, and
    the PIC fills each column from the bottom row)."""
    value, colours = 0, set()
    for y in range(ROWS):  # y = 0 is the bottom row
        ch = rows[ROWS - 1 - y][x]
        if ch != ".":
            value |= 1 << (7 - y)
            colours.add(ch)
    return value, colours


def decode(rows):
    """Returns (pointers, commit_prefix, fault_type, warnings). pointers: [(colour, value)] in drawing order."""
    warnings, pointers = [], []
    for group in range(MAIN_COLUMNS // 4):
        values, colours = [], set()
        for x in range(group * 4, group * 4 + 4):
            v, c = column(rows, x)
            values.append(v)
            colours |= c
        if not colours:
            continue
        if len(colours) > 1:
            warnings.append(
                f"columns {group * 4}-{group * 4 + 3} mix colours {sorted(colours)}; check the photo"
            )
        colour = min(colours)
        if colour not in GROUP_MEANING:
            warnings.append(
                f"columns {group * 4}-{group * 4 + 3} are {colour!r}, which printPointers() never draws"
            )
        pointers.append((colour, int.from_bytes(bytes(values), "big")))
    (b0, c0), (b1, c1) = column(rows, 16), column(rows, 17)
    sidebar = c0 | c1
    if len(sidebar) > 1:
        warnings.append(f"sidebar mixes colours {sorted(sidebar)}; check the photo")
    fault = (
        FAULT_TYPE.get(min(sidebar), "?")
        if sidebar
        else "? (sidebar unlit: commit 0000, or not a crash)"
    )
    return pointers, f"{b0:02x}{b1:02x}", fault, warnings


# ---- The ELF ----


def built_commits(elf_path):
    """Short commit hashes compiled into the ELF. kCommitShort points at one, but LTO can drop the pointer itself, so
    this looks for the string: NUL-terminated runs of 7-12 lowercase hex digits (rarely anything else)."""
    with open(elf_path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF":
        sys.exit(f"{elf_path} is not an ELF")
    return sorted(
        {m.decode() for m in re.findall(rb"(?<=\x00)([0-9a-f]{7,12})(?=\x00)", data)}
    )


def tool(name):
    path = shutil.which(name)
    if path is None:
        sys.exit(f"{name} not found; run this inside ./dbt exec")
    return path


def symbols(elf_path):
    """Sorted [(address, size, type, name)] from nm. Thumb function addresses have bit 0 set; it's cleared here."""
    out = subprocess.run(
        [tool("arm-none-eabi-nm"), "-n", "-S", "-C", elf_path],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    table = []
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]{8}) (?:([0-9a-f]{8}) )?(\w) (.+)", line)
        if m:
            table.append((int(m[1], 16) & ~1, int(m[2] or "0", 16), m[3], m[4]))
    return table


def code_symbol(table, starts, address):
    """The code symbol (start, size, name) at or before the address, or None."""
    i = bisect.bisect_right(starts, address) - 1
    while i >= 0 and table[i][2] not in "tTwW":
        i -= 1
    if i < 0:
        return None
    start, size, _, name = table[i]
    return start, size, name


def locate(table, starts, address):
    """Where the address is: 'name+0xoff', or a note when it isn't inside a function. Returns (text, function)."""
    sym = code_symbol(table, starts, address)
    if sym is None:
        return "(before the first code symbol)", None
    start, size, name = sym
    following = code_symbol(table, starts, address + 2)
    if following and following[0] == address + 2:
        # printPointers() subtracts 2 from every stack word. A word 2 below a function start was a function
        # pointer on the stack (a callback, a vtable entry), not a return address.
        return (
            f"(2 bytes before {following[2]}: the stack held a pointer to that function)",
            None,
        )
    if size and address >= start + size:
        return (
            f"(past the end of {name}, 0x{address - start:x} into a {size}-byte symbol)",
            None,
        )
    return f"{name}+0x{address - start:x}", sym


def disassemble(elf_path, function, address):
    """The function's instructions up to and including the one at the address (the last few lines of it)."""
    start, size, _ = function
    out = subprocess.run(
        [
            tool("arm-none-eabi-objdump"),
            "-d",
            "-C",
            f"--start-address=0x{start:x}",
            f"--stop-address=0x{min(address + 4, start + size) if size else address + 4:x}",
            elf_path,
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    lines = [
        line.strip()
        for line in out.splitlines()
        if re.match(r"\s*[0-9a-f]+:\t", line) and "out of bounds" not in line
    ]
    return lines[-3:]


def source_lines(elf_path, address):
    """addr2line's inline chain for the address: 'function at file:line' per level, innermost first. LTO leaves
    some levels without line info; those print as the function alone."""
    # binutils' addr2line answers its first query in a run without the inline chain, so ask twice (-a marks each
    # answer with its address) and keep the second answer.
    out = subprocess.run(
        [
            tool("arm-none-eabi-addr2line"),
            "-a",
            "-f",
            "-C",
            "-i",
            "-e",
            elf_path,
            f"0x{address:x}",
            f"0x{address:x}",
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    marks = [i for i, line in enumerate(out) if re.fullmatch(r"0x[0-9a-f]+", line)]
    answer = out[marks[-1] + 1 :] if marks else []
    chain = []
    for function, place in zip(answer[0::2], answer[1::2]):
        if function == "??":
            continue
        place = (
            ""
            if place.startswith("??")
            else " at " + re.sub(r".*?/src/", "src/", place)
        )
        chain.append(f"{function}{place}")
    return chain


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("grid", help="the transcribed pads, or - for stdin")
    ap.add_argument("--elf", help="the ELF of the build that crashed")
    ap.add_argument(
        "--any-commit",
        action="store_true",
        help="look addresses up even if the commit doesn't match",
    )
    a = ap.parse_args()

    if a.grid == "-":
        text = sys.stdin.read()
    else:
        with open(a.grid) as fh:
            text = fh.read()
    pointers, commit, fault, warnings = decode(read_grid(text))
    print(f"fault:  {fault}")
    print(f"commit: {commit}... (first 4 hex digits of the crashed build's commit)")
    for w in warnings:
        print(f"warning: {w}")
    if not pointers:
        print("no addresses drawn")

    table = starts = None
    if a.elf:
        table = symbols(a.elf)
        starts = [s[0] for s in table]
        commits = built_commits(a.elf)
        matching = [c for c in commits if c.startswith(commit)]
        if matching:
            print(
                f"elf:    {os.path.relpath(a.elf)}, built from commit {matching[0]} (matches)"
            )
        else:
            msg = (
                f"no commit string in the ELF starts with {commit} (found: {', '.join(commits) or 'none'}): "
                "it's not the build that crashed"
            )
            if not a.any_commit:
                print(
                    f"error: {msg}. Find that build's ELF, or pass --any-commit if the code is known to be identical."
                )
                return 1
            print(f"warning: {msg}; looking up anyway (--any-commit)")

    for colour, value in pointers:
        print(f"\n0x{value:08x}  {GROUP_MEANING.get(colour, '?')}")
        if table:
            address = (
                value & ~1
            )  # Thumb code: an odd value means the instruction at the even address below it
            where, function = locate(table, starts, address)
            print(f"            {where}")
            if function:
                instructions = disassemble(a.elf, function, address)
                # LR - 2 lands inside a 4-byte call. Line info hangs off the instruction's first byte.
                starts_at = [int(line.split(":", 1)[0], 16) for line in instructions]
                containing = max(
                    (x for x in starts_at if x <= address), default=address
                )
                for level, line in enumerate(source_lines(a.elf, containing)):
                    print(f"            {'inlined in ' if level else 'source: '}{line}")
                for line in instructions:
                    print(f"            {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
