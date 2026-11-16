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
    "b": "SYS-mode LR - 2: inside the last call made, usually the one into the faulting function (check the bl target)",
    "g": "stack hint: a code address found on the stack, - 2 (may be stale)",
    "c": "stack hint: a code address found on the stack, - 2 (may be stale)",
}
SOFT_FAULT_LR = "LR at the FREEZE_WITH_ERROR - 2: inside the last call made before it, or the call into its function"

# Once printPointers() finds one stack hint, it draws all 4 slots. The ones it didn't fill hold 0, drawn as 0 - 2.
UNUSED_STACK_SLOT = 0xFFFFFFFE

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

# The fault entry subtracts 4 from the undefined-instruction and SVC LR, but in Thumb code that LR is only 2 past the
# instruction (ARM ARM, "Link values saved on exception entry"). So the drawn PC is 2 bytes early in Thumb code. In ARM
# code it's exact, and 2 bytes later is still inside the same 4-byte instruction, so +2 is right for both.
PC_EARLY_BY_2 = {"b", "c"}


def majority(colours):
    """The most common colour; one misread pad shouldn't decide what a group means."""
    return max(sorted(set(colours)), key=colours.count)


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
    value, colours = 0, []
    for y in range(ROWS):  # y = 0 is the bottom row
        ch = rows[ROWS - 1 - y][x]
        if ch != ".":
            value |= 1 << (7 - y)
            colours.append(ch)
    return value, colours


def decode(rows):
    """Returns (pointers, commit_prefix, fault_colour, warnings). pointers: [(colour, value)] in drawing order.
    fault_colour is the sidebar's colour, or None when it's unlit."""
    warnings, pointers = [], []
    for group in range(MAIN_COLUMNS // 4):
        values, colours = [], []
        for x in range(group * 4, group * 4 + 4):
            v, c = column(rows, x)
            values.append(v)
            colours += c
        if not colours:
            continue
        colour = majority(colours)
        if len(set(colours)) > 1:
            warnings.append(
                f"columns {group * 4}-{group * 4 + 3} mix colours {sorted(set(colours))}; read as {colour!r},"
                " check the photo"
            )
        if colour not in GROUP_MEANING:
            warnings.append(
                f"columns {group * 4}-{group * 4 + 3} are {colour!r}, which printPointers() never draws"
            )
        pointers.append((colour, int.from_bytes(bytes(values), "big")))
    (b0, c0), (b1, c1) = column(rows, 16), column(rows, 17)
    sidebar = c0 + c1
    fault_colour = majority(sidebar) if sidebar else None
    if len(set(sidebar)) > 1:
        warnings.append(
            f"sidebar mixes colours {sorted(set(sidebar))}; read as {fault_colour!r}, check the photo"
        )
    return pointers, f"{b0:02x}{b1:02x}", fault_colour, warnings


# ---- The ELF ----


def built_commits(elf_path):
    """Short commit hashes compiled into the ELF. kCommitShort points at one, but LTO can drop the pointer itself, so
    this looks for the string: NUL-terminated runs of 7-12 lowercase hex digits (rarely anything else), on their own
    or as the tail of the version string ("1.3.0-dev-3212a869c"). The linker merges a string into the tail of a
    longer one that ends the same, so kCommitShort usually points into the version string."""
    try:
        with open(elf_path, "rb") as fh:
            data = fh.read()
    except OSError as e:
        sys.exit(f"can't read the ELF: {e}")
    if data[:4] != b"\x7fELF":
        sys.exit(f"{elf_path} is not an ELF")
    return sorted(
        {
            m.decode()
            for m in re.findall(
                rb"(?<=\x00)(?:\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)*?-)?([0-9a-f]{7,12})(?=\x00)",
                data,
            )
        }
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


def code_range(table):
    """[program_code_start, program_code_end): where printPointers() accepts code pointers. None if not in the ELF."""
    found = {name: address for address, _, _, name in table}
    if "program_code_start" in found and "program_code_end" in found:
        return found["program_code_start"], found["program_code_end"]
    return None


def locate(table, starts, address):
    """Where the address is. Returns (text, function, pointer_to): text is 'name+0xoff' or a note, function the code
    symbol to disassemble (or None), and pointer_to the name of a function that starts 2 bytes later (or None)."""
    span = code_range(table)
    if span and not span[0] <= address < span[1]:
        return (
            f"(outside the code, 0x{span[0]:x}-0x{span[1]:x}: printPointers() never draws this; check the photo)",
            None,
            None,
        )
    sym = code_symbol(table, starts, address)
    if sym is None:
        return "(before the first code symbol)", None, None
    start, size, name = sym
    following = code_symbol(table, starts, address + 2)
    pointer_to = following[2] if following and following[0] == address + 2 else None
    if size and address >= start + size:
        if pointer_to:
            return f"(2 bytes before {pointer_to})", None, pointer_to
        return (
            f"(past the end of {name}, 0x{address - start:x} into a {size}-byte symbol)",
            None,
            None,
        )
    return f"{name}+0x{address - start:x}", sym, pointer_to


def is_call(instruction):
    """Whether an objdump line ('addr:<tab>bytes<tab>mnemonic<tab>operands') is a bl or blx."""
    fields = instruction.split("\t")
    return len(fields) > 2 and fields[2].split()[0] in ("bl", "blx")


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
    # The stop address can take in the instruction after a 2-byte one at the address; that one didn't run.
    lines = [line for line in lines if int(line.split(":", 1)[0], 16) <= address]
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
        try:
            with open(a.grid) as fh:
                text = fh.read()
        except OSError as e:
            sys.exit(f"can't read the grid: {e}")
    pointers, commit, fault_colour, warnings = decode(read_grid(text))
    fault = (
        FAULT_TYPE.get(fault_colour, "?")
        if fault_colour
        else "? (sidebar unlit: commit 0000, or not a crash)"
    )
    print(f"fault:  {fault}")
    print(f"commit: {commit}... (first 4 hex digits of the crashed build's commit)")
    for w in warnings:
        print(f"warning: {w}")
    if not pointers:
        print("no addresses drawn")

    table = starts = None
    if a.elf:
        commits = built_commits(a.elf)
        table = symbols(a.elf)
        starts = [s[0] for s in table]
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
        if colour in "gc" and value == UNUSED_STACK_SLOT:
            print(
                f"\n0x{value:08x}  unused stack slot: fewer stack hints were found than there was room for"
            )
            continue
        meaning = GROUP_MEANING.get(colour, "?")
        if colour == "b" and fault_colour == "m":
            meaning = SOFT_FAULT_LR
        # Thumb code: an odd value means the instruction at the even address below it.
        address = value & ~1
        if colour == "m" and fault_colour in PC_EARLY_BY_2:
            address += 2
            meaning += (
                f", 2 bytes early for this fault type: looked up at 0x{address:08x}"
            )
        print(f"\n0x{value:08x}  {meaning}")
        if not table:
            continue
        where, function, pointer_to = locate(table, starts, address)
        if colour not in "gc":
            pointer_to = None  # only stack words can be function pointers
        pointer_note = (
            f"(2 bytes before {pointer_to}: the stack held a pointer to that function)"
        )
        instructions = disassemble(a.elf, function, address) if function else []
        # printPointers() subtracts 2 from every stack word, so a function pointer lands 2 below the function. So
        # does the return address of a call that ends the function before it (a call that never returns).
        if pointer_to and not (instructions and is_call(instructions[-1])):
            print(f"            {pointer_note}")
            continue
        print(f"            {where}")
        if pointer_to:
            print(
                f"            or a pointer to {pointer_to}, which starts where this call would return"
            )
        if instructions:
            # LR - 2 lands inside a 4-byte call. Line info hangs off the instruction's first byte.
            containing = int(instructions[-1].split(":", 1)[0], 16)
            for level, line in enumerate(source_lines(a.elf, containing)):
                print(f"            {'inlined in ' if level else 'source: '}{line}")
            for line in instructions:
                print(f"            {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
