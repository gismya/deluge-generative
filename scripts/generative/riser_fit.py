"""Spec §10.5's Riser fit: every pack Riser a genre's Riser lane can pick is no longer than its shortest Build.

    python3 scripts/generative/riser_fit.py [--pack DIR] [--render BIN]

A sample Riser longer than the Build would start on the Build's first step and overshoot the Drop, and starting
partway into the file isn't built, so each genre's Riser lane lists only Risers that fit its shortest Build at the
top of its tempo range. `generator_render --list-risers` prints each built-in genre's lane from the genre tables:
the shortest Build in ms, the lane's folders and its tags. This reads the WAVs in those folders under pack/ that the
tags reach (any file when the lane has none; a tag matches anywhere in the name, ignoring case, and one holding a
'/' names one file, as the firmware's do) and fails on one longer than the Build, and on a lane that reaches no pack Riser at all. Riser lanes list only
pack Risers (spec §10.5), so the factory card isn't read. check_all.py runs it after the harness build. Exit 0 when
every Riser fits, 1 otherwise, 2 when it can't run.
"""

import argparse
import os
import subprocess
import sys
import wave

import osc_octaves

AUDIO = (".wav", ".aif", ".aiff")


class CantCheck(Exception):
    """A listing or file the check can't read: exit 2, not a finding."""


def parse_listing(text):
    """[(genre, ms, [folders], [tags])] from --list-risers' lines."""
    lanes = []
    for line in text.splitlines():
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise CantCheck(f"bad --list-risers line: {line!r}")
        genre, ms, folders, tags = fields
        lanes.append(
            (
                genre,
                int(ms),
                [f for f in folders.split("|") if f],
                [t for t in tags.split("|") if t],
            )
        )
    return lanes


def seconds(path):
    """A WAV's length in seconds."""
    try:
        with wave.open(path, "rb") as f:
            return f.getnframes() / f.getframerate()
    except (wave.Error, EOFError, OSError) as error:
        raise CantCheck(f"can't read {path}: {error}") from error


def candidates(pack, folders, tags):
    """The pack files the lane can pick: (card path, file path), by name order."""
    out = []
    for tag in tags:
        if "/" in tag and os.path.isfile(os.path.join(pack, tag)):
            out.append((tag, os.path.join(pack, tag)))
    for folder in folders:
        full = os.path.join(pack, folder)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if not name.lower().endswith(AUDIO) or name.startswith("."):
                continue
            stem = os.path.splitext(name)[0].lower()
            if tags and not any("/" not in tag and tag.lower() in stem for tag in tags):
                continue
            out.append((f"{folder}/{name}", os.path.join(full, name)))
    return out


def check(listing, pack, length=seconds):
    """(ok, report lines, Risers checked) for a --list-risers listing against pack/."""
    ok = True
    lines = []
    count = 0
    for genre, ms, folders, tags in parse_listing(listing):
        picks = candidates(pack, folders, tags)
        if not picks:
            ok = False
            lines.append(f"NONE {genre}: its Riser lane reaches no pack Riser")
            continue
        for card_path, path in picks:
            count += 1
            took = length(path)
            if took * 1000 > ms:
                ok = False
                lines.append(
                    f"LONG {genre}: {card_path} is {took:.2f} s, its shortest Build {ms / 1000:.2f} s"
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
        f"{count} pack Risers, {'every one fits' if ok else 'not every one fits'} its genre's shortest Build"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
