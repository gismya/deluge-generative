"""Spec §10.5's Riser fit: every pack Riser a genre's Riser lane can pick fits its shortest Build, or its fallback does.

    python3 scripts/generative/riser_fit.py [--pack DIR] [--render BIN]

A sample Riser longer than the Build would overshoot the Drop, and starting partway into the file isn't built, so the
firmware plays a fallback in such a Build: the longest pack short Riser shorter than the drawn one, if that fits, else
no Riser (spec §9.5). `generator_render --list-risers` prints the fallbacks (path and length, longest first) and each
built-in genre's lane: the shortest Build in ms at the top of its tempo range (over the genre as written and each of
its variants), the lane's folders and its tags. This reads the WAVs in those folders under pack/ that the tags reach
(any file when the lane has none; a tag matches anywhere in the name, ignoring case, and one holding a '/' names one
file, ignoring case too, as the firmware's do; each file counts once) and fails on one longer than the Build whose
fallback doesn't fit it either, on a fallback missing from pack/ or not as long as listed (the firmware picks by the
listed length), on an AIFF (it can't measure one), and on a lane that reaches no pack Riser at all. Riser lanes list
only pack Risers (spec §10.5), so the factory card isn't read. check_all.py runs it after the harness build. Exit 0
when every Riser fits, 1 otherwise, 2 when it can't run.
"""

import argparse
import os
import subprocess
import sys
import wave
from typing import NamedTuple

import osc_octaves

WAV = ".wav"
AIFF = (".aif", ".aiff")  # the firmware plays them too; this check can't measure them


class Lane(NamedTuple):
    """A genre's Riser lane from --list-risers: its shortest Build in ms, its folders and its tags."""

    genre: str
    ms: int
    folders: list
    tags: list


class CantCheck(Exception):
    """A listing or file the check can't read: exit 2, not a finding."""


class Fallback(NamedTuple):
    """A pack Riser the firmware falls back to (spec §9.5), and the length it takes it to be."""

    path: str
    ms: int


def parse_listing(text):
    """([Lane], [Fallback] longest first) from --list-risers' lines."""
    lanes = []
    fallbacks = []
    for line in text.splitlines():
        if not line:
            continue
        fields = line.split("\t")
        if fields[0] == "fallback" and len(fields) == 3:
            fallbacks.append(Fallback(fields[1], int(fields[2])))
            continue
        if len(fields) != 4:
            raise CantCheck(f"bad --list-risers line: {line!r}")
        genre, ms, folders, tags = fields
        lanes.append(
            Lane(
                genre,
                int(ms),
                [f for f in folders.split("|") if f],
                [t for t in tags.split("|") if t],
            )
        )
    return lanes, fallbacks


def seconds(path):
    """A WAV's length in seconds."""
    try:
        with wave.open(path, "rb") as f:
            return f.getnframes() / f.getframerate()
    except (wave.Error, EOFError, OSError) as error:
        raise CantCheck(f"can't read {path}: {error}") from error


def find_ignoring_case(root, card_path):
    """The card path under root as the pack spells it, matching each part ignoring case, or None."""
    found = []
    here = root
    for part in card_path.split("/"):
        if not os.path.isdir(here):
            return None
        name = next(
            (n for n in sorted(os.listdir(here)) if n.lower() == part.lower()), None
        )
        if name is None:
            return None
        found.append(name)
        here = os.path.join(here, name)
    return "/".join(found) if os.path.isfile(here) else None


def candidates(pack, folders, tags):
    """The pack files the lane can pick, each once: (card path, file path), exact paths first, then by name order."""
    out = []
    seen = set()

    def add(card_path):
        if card_path.lower() not in seen:
            seen.add(card_path.lower())
            out.append((card_path, os.path.join(pack, card_path)))

    for tag in tags:
        found = find_ignoring_case(pack, tag) if "/" in tag else None
        if found is not None:
            add(found)
    for folder in folders:
        full = os.path.join(pack, folder)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if not name.lower().endswith((WAV,) + AIFF) or name.startswith("."):
                continue
            stem = os.path.splitext(name)[0].lower()
            if tags and not any("/" not in tag and tag.lower() in stem for tag in tags):
                continue
            add(f"{folder}/{name}")
    return out


def fallback_for(fallbacks, took):
    """The fallback the firmware loads beside a Riser `took` seconds long: the longest one shorter, or None."""
    return next((f for f in fallbacks if f.ms < took * 1000), None)


def check(listing, pack, length=seconds):
    """(ok, report lines, Risers checked) for a --list-risers listing against pack/."""
    ok = True
    lines = []
    count = 0
    lanes, fallbacks = parse_listing(listing)
    measured = {}
    for f in fallbacks:
        found = find_ignoring_case(pack, f.path)
        if found is None:
            ok = False
            lines.append(f"FALLBACK {f.path}: not in the pack")
            continue
        took = length(os.path.join(pack, found))
        measured[f.path] = took
        # The firmware picks a fallback by its listed length: it must be the file's.
        if abs(took * 1000 - f.ms) > 50:
            ok = False
            lines.append(
                f"FALLBACK {f.path}: is {took:.2f} s, listed as {f.ms / 1000:.2f} s"
            )
    for genre, ms, folders, tags in lanes:
        picks = candidates(pack, folders, tags)
        if not picks:
            ok = False
            lines.append(f"NONE {genre}: its Riser lane reaches no pack Riser")
            continue
        for card_path, path in picks:
            count += 1
            if path.lower().endswith(AIFF):
                ok = False
                lines.append(
                    f"AIFF {genre}: {card_path} isn't a WAV, so its length can't be checked"
                )
                continue
            took = length(path)
            if took * 1000 <= ms:
                continue
            # Longer than the shortest Build: the firmware plays its fallback there, which must fit (spec §9.5).
            fallback = fallback_for(fallbacks, took)
            if (
                fallback is not None
                and fallback.path in measured
                and measured[fallback.path] * 1000 <= ms
            ):
                continue
            ok = False
            lines.append(
                f"LONG {genre}: {card_path} is {took:.2f} s, its shortest Build {ms / 1000:.2f} s, and no fallback fits"
            )
    return ok, lines, count


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--pack",
        default=osc_octaves.PACK,
        help="the Generative pack (default the repo's pack/)",
    )
    p.add_argument("--render", help="generator_render (default: the harness build's)")
    args = p.parse_args(argv)
    render = args.render or osc_octaves.find_render()
    if render is None:
        print(
            "riser_fit: no generator_render; run ./dbt test --generator first",
            file=sys.stderr,
        )
        return 2
    listed = subprocess.run(
        [render, "--list-risers"], capture_output=True, text=True, check=False
    )
    if listed.returncode != 0:
        print(
            f"riser_fit: {render} --list-risers exited {listed.returncode}: {listed.stderr.strip()}",
            file=sys.stderr,
        )
        return 2
    try:
        ok, lines, count = check(listed.stdout, args.pack)
    except CantCheck as error:
        print(f"riser_fit: {error}", file=sys.stderr)
        return 2
    for line in lines:
        print(line)
    print(
        f"{count} pack Risers, {'every one fits' if ok else 'not every one fits'} its genre's shortest Build or has a "
        "fallback that does"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
