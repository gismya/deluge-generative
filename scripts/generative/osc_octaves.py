"""Flags pitched parts' palette presets whose oscillators all sit an octave or more off the note (#311, spec §10.6).

    python3 scripts/generative/osc_octaves.py [--card DIR] [--pack DIR] [--render BIN]

A pitched part (Bass, Lead, Chords, Pad) loads its preset with the whole-sound transpose at 0 (ADR 0007), so its
Register is the pitch heard. That doesn't reach an oscillator's own transpose: a preset whose sounding oscillators all
carry one of 12 semitones or more (165 Acid Arp's -36 and -24) still plays that far off its Register. This lists
every preset the built-in genres' pitched parts can pick (`generator_render --list-palettes`, the harness's build of
the genre tables), reads each from pack/ and then the factory card image, as the render does, and flags those. A
flagged preset is dropped from its palette, or kept in ALLOWED below with the reason, for one genre's part: the
same preset in another part (another Register) is flagged again.

An oscillator sounds when its volume (oscAVolume, oscBVolume) is above 0; in ring mod both always do. Cents, the
modulators of an FM preset, the noise and patch cables (a cable onto an oscillator's volume or pitch) are ignored.
"Off" is abs(transpose) >= 12 on every sounding oscillator, wider than a whole non-zero octave on purpose: a pair
such as -12 and +19 has no oscillator at the note either. An ALLOWED entry its part no longer lists, or one no
longer flagged, is STALE and fails, so the list stays short. Local only: CI has no card; check_all.py runs it after the
harness specs, which build generator_render, and skips it with no card. Exit 0 when nothing is flagged, 1 otherwise, 2
when it can't run (no card or generator_render, generator_render failing, a preset it can't read).
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CARD = os.path.join(os.path.expanduser("~"), "source", "delugemu-sd", "sdcard")
PACK = os.path.join(REPO, "pack")

# Presets kept though they're flagged: (genre, part, path) -> why. Each must still be listed by that genre's part.
ALLOWED = {}

SILENT = 0x80000000  # a param at its minimum: the oscillator's volume is off


class CantCheck(Exception):
    """A preset or listing the check can't read: exit 2, not a finding."""


def _field(element, name):
    """A preset's value: a child element's text (newer presets) or an attribute (older ones)."""
    if element is None:
        return None
    child = element.find(name)
    if child is not None:
        return (child.text or "").strip()
    return element.get(name)


def parse(text):
    """The <sound> of a preset file's text: the firmware writes version tags beside it, so it isn't one document."""
    body = re.sub(r"<\?xml[^>]*\?>", "", text)
    root = ET.fromstring("<preset>" + body + "</preset>")
    sound = root.find("sound")
    if sound is None:
        raise ValueError("no <sound>")
    return sound


def sounding_transposes(sound):
    """Each sounding oscillator's transpose, in semitones."""
    ring = _field(sound, "mode") == "ringmod"
    params = sound.find("defaultParams")
    out = []
    for osc, volume in (("osc1", "oscAVolume"), ("osc2", "oscBVolume")):
        level = _field(params, volume)
        if not ring and level is not None and int(level, 16) == SILENT:
            continue
        out.append(int(_field(sound.find(osc), "transpose") or 0))
    return out


def off_octave(sound):
    """True when every sounding oscillator is 12 semitones or more off the note."""
    transposes = sounding_transposes(sound)
    return bool(transposes) and all(abs(t) >= 12 for t in transposes)


def find_preset(path, roots):
    for root in roots:
        full = os.path.join(root, path)
        if os.path.isfile(full):
            return full
    return None


def check(listing, roots, allowed):
    """(ok, lines, count) for `listing`, generator_render --list-palettes' output; count is the distinct presets."""
    lines = []
    ok = True
    listed = set()
    keys = set()
    flagged_allowed = set()
    missing = set()
    for row in listing.splitlines():
        if not row.strip():
            continue
        fields = row.split("\t")
        if len(fields) != 3:
            raise CantCheck(f"not a genre<TAB>part<TAB>path line: {row!r}")
        genre, part, path = fields
        listed.add(path)
        key = (genre, part, path)
        keys.add(key)
        where = f"{genre} {part} {path}"
        full = find_preset(path, roots)
        if full is None:
            lines.append(f"MISSING {where}: on neither {' nor '.join(roots)}")
            missing.add(key)
            ok = False
            continue
        try:
            with open(full, encoding="utf-8", errors="replace") as fh:
                sound = parse(fh.read())
            off = off_octave(sound)
        except (ET.ParseError, ValueError, OSError) as error:
            raise CantCheck(f"can't read {full}: {error}") from error
        if not off:
            continue
        detail = "osc " + " ".join(str(t) for t in sounding_transposes(sound))
        if key in allowed:
            if key not in flagged_allowed:
                lines.append(f"allowed {where} ({detail}): {allowed[key]}")
            flagged_allowed.add(key)
            continue
        lines.append(
            f"FLAGGED {where} ({detail}): drop it from the palette or allow it in osc_octaves.ALLOWED"
        )
        ok = False
    for genre, part, path in sorted(set(allowed) - keys):
        lines.append(
            f"STALE {genre} {part} {path}: its part no longer lists it; remove it from osc_octaves.ALLOWED"
        )
        ok = False
    for genre, part, path in sorted((set(allowed) & keys) - flagged_allowed - missing):
        lines.append(
            f"STALE {genre} {part} {path}: no longer off an octave; remove it from osc_octaves.ALLOWED"
        )
        ok = False
    return ok, lines, len(listed)


def find_render():
    """The harness's generator_render, as ./dbt test --generator builds it (multi-config builds add a folder)."""
    build = os.path.join(REPO, "build", "tests", "spec", "generator")
    for path in [os.path.join(build, "generator_render")] + sorted(
        glob.glob(os.path.join(build, "*", "generator_render"))
    ):
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--card", default=CARD, help=f"the factory card's files (default {CARD})"
    )
    p.add_argument(
        "--pack", default=PACK, help="the Generative pack (default the repo's pack/)"
    )
    p.add_argument("--render", help="generator_render (default: the harness build's)")
    args = p.parse_args(argv)
    render = args.render or find_render()
    if render is None:
        print(
            "osc_octaves: no generator_render; run ./dbt test --generator first",
            file=sys.stderr,
        )
        return 2
    if not os.path.isdir(args.card):
        print(f"osc_octaves: no card at {args.card}", file=sys.stderr)
        return 2
    listed = subprocess.run(
        [render, "--list-palettes"], capture_output=True, text=True, check=False
    )
    if listed.returncode != 0:
        print(
            f"osc_octaves: {render} --list-palettes exited {listed.returncode}: {listed.stderr.strip()}",
            file=sys.stderr,
        )
        return 2
    try:
        ok, lines, count = check(listed.stdout, [args.pack, args.card], ALLOWED)
    except CantCheck as error:
        print(f"osc_octaves: {error}", file=sys.stderr)
        return 2
    for line in lines:
        print(line)
    print(
        f"{count} pitched palette presets, {'none' if ok else 'some'} off by an octave on every oscillator"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
