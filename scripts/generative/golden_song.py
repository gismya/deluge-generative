"""The firmware check's reading side (spec §21.3): a Compose song the firmware saved, read back into the golden's text
dump, and the Recipe file saved beside it, against a harness golden (tests/spec/generator/goldens/<genre>.txt).

gen_emu.py golden builds a golden's case through Compose in delugemu (golden_compose.txt) and calls compare() on the
files it reads off the card. Stdlib only; no emulator here, so the unit tests (test_golden_song.py) run it on text.

What is compared: the notes, as the golden's note-on and note-off lines. Compose writes no muted note (spec §15.2), so
a muted note-on and its note-off leave the golden's side; an ornament is a plain note in a song, so the word goes. The
golden stops at its last line, where Perform's stop ends what still sounds, so both sides keep the notes that start
more than kMaxLean before that tick (a later one may come of a decision the golden never made) and cut them there:
compose_driver_spec's "from the Groove, plays each golden's notes" is the harness side of the same rule. Notes that
start together sort by their line's text, as play order within a tick is the core's, not the song's.
"""

import os
import re
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GOLDENS = os.path.join(REPO, "tests", "spec", "generator", "goldens")

# The core's grid (generative/core/types.h); test_golden_song.py checks these against it.
TICKS_PER_STEP = 24
STEPS_PER_BAR = 16
MAX_LEAN = 12
MACRO_UNITS_PER_POINT = 64  # generative/core/recipe.h
# The Drums and FX lanes, in Lane order: the Kit rows' names in a song (generative/core/lane_names.h) and the
# golden's (tests/spec/generator/render/names.cpp kLaneNames).
SONG_LANES = ("KICK", "SNARE", "CLAP", "RIM", "CLOSED HAT", "OPEN HAT", "RIDE", "CRASH", "TOM", "PERC 1", "PERC 2",
              "RISER", "FALL", "IMPACT", "SIREN")  # fmt: skip
GOLDEN_LANES = ("Kick", "Snare", "Clap", "Rim", "HatC", "HatO", "Ride", "Crash", "Tom", "Perc1", "Perc2", "Riser",
                "Fall", "Impact", "Siren")  # fmt: skip
PITCH_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
NOTE_BYTES = 14  # noteDataWithSplitProb: position 4, length 4, velocity, lift, probability, iterance 2, fill

EVENT_LINE = re.compile(r"^\s*(\d+)\s+(\S+)\s+(\S+)\s+(.*?)\s*$")
NOTE_ON = re.compile(r"^(\S+) on v(\d+)((?: ornament)?)((?: muted)?)$")
NOTE_OFF = re.compile(r"^(\S+) off$")
RECIPE_LINE = re.compile(
    r"^recipe entropy \d+: (\S+) (.+), (\d+)\.(\d\d) BPM, swing (-?\d+), energy (\d+), density (\d+), "
    r"complexity (\d+), motion (\d+)$"
)
SEED_LINE = re.compile(r"^#.*\bseed (\d+),")


class GoldenError(Exception):
    """A file that isn't what the check reads: the message says which and why."""


def golden_path(genre):
    return os.path.join(GOLDENS, genre + ".txt")


def note_name(pitch):
    """A MIDI note as the golden names it (render::noteName()): 31 is "G1"."""
    return f"{PITCH_NAMES[pitch % 12]}{pitch // 12 - 1}"


def position(tick):
    """Where a tick falls, as the golden writes it (goldens::positionOf()): "bar.beat.16th", "+ticks" past it."""
    step, rest = divmod(tick, TICKS_PER_STEP)
    out = f"{step // STEPS_PER_BAR + 1}.{step % STEPS_PER_BAR // 4 + 1}.{step % 4 + 1}"
    return out if rest == 0 else f"{out}+{rest}"


def event_line(tick, owner, what):
    """One golden line (goldens::dumpEvents()): "    384  1.4.1     ACID     G2 on v100"."""
    return f"{tick:>7}  {position(tick):<10}{owner:<9}{what}"


class Golden:
    """A golden file's header facts and its notes, (tick, length, part, key, velocity), paired as the harness pairs
    Perform's stream (compose_driver_spec's pairNotes(): a note-off ends the earliest open note-on of its part and key;
    a muted note-on opens nothing)."""

    def __init__(self, text):
        self.genre = self.version = self.seed = None
        self.recipe = None
        self.parts = []
        self.notes = []
        self.last_tick = 0
        open_notes = []
        for line in text.splitlines():
            if line.startswith("genre "):
                _, self.genre, _, version = line.split()
                self.version = int(version)
            elif line.startswith("parts "):
                self.parts = [
                    p.split(" ", 1)[1] for p in line[len("parts ") :].split(", ")
                ]
            elif RECIPE_LINE.match(line):
                self.recipe = RECIPE_LINE.match(line).groups()
            elif SEED_LINE.match(line) and self.seed is None:
                self.seed = int(SEED_LINE.match(line).group(1))
            event = EVENT_LINE.match(line)
            if not event or not line.startswith(" "):
                continue
            tick, _, owner, what = event.groups()
            tick = int(tick)
            self.last_tick = max(self.last_tick, tick)
            on, off = NOTE_ON.match(what), NOTE_OFF.match(what)
            if on and not on.group(4):
                open_notes.append((tick, owner, on.group(1), int(on.group(2))))
            elif off:
                for i, (start, part, key, velocity) in enumerate(open_notes):
                    if (part, key) == (owner, off.group(1)):
                        self.notes.append((start, tick - start, part, key, velocity))
                        del open_notes[i]
                        break
        if (
            self.genre is None
            or not self.parts
            or self.recipe is None
            or self.seed is None
        ):
            raise GoldenError(
                "not a golden: no genre, parts, recipe or seed line in its header"
            )


def read_note_data(hex_text):
    """The notes of a noteRow's noteDataWithSplitProb: [(position, length, velocity)]."""
    if not hex_text.startswith("0x"):
        raise GoldenError(f"noteDataWithSplitProb without 0x: {hex_text[:20]}")
    data = bytes.fromhex(hex_text[2:])
    if len(data) % NOTE_BYTES:
        raise GoldenError(
            f"noteDataWithSplitProb of {len(data)} bytes, not whole {NOTE_BYTES}-byte notes"
        )
    return [
        (
            int.from_bytes(data[i : i + 4], "big"),
            int.from_bytes(data[i + 4 : i + 8], "big"),
            data[i + 8],
        )
        for i in range(0, len(data), NOTE_BYTES)
    ]


def recipe_sounds(recipe_xml):
    """A Recipe file's synth parts' sounds, {part name: preset name} ("SYNTHS/005 Sweet Mono Bass.XML" gives
    "005 Sweet Mono Bass"), and the file's root element."""
    root = ET.fromstring(recipe_xml)
    if root.tag != "generativeRecipe":
        raise GoldenError(f"not a Recipe file: its root is <{root.tag}>")
    sounds = {}
    for part in root.findall("part"):
        path = part.get("sound")
        if path:
            sounds[part.get("name")] = os.path.splitext(os.path.basename(path))[0]
    return sounds, root


def song_notes(song_xml, parts, sounds):
    """The notes of a Compose song, (tick, length, part, key, velocity), the part and key named as the golden names
    them. `parts` is the genre's roster (the golden's parts line); `sounds` the synth parts' presets (recipe_sounds()).
    A Compose song's instruments are its parts' in roster order (a part that never sounds has none): a Kit is named
    after its part, a synth after its preset."""
    root = ET.fromstring(song_xml)
    instruments = root.find("instruments")
    clips = root.find("sessionClips")
    if root.tag != "song" or instruments is None or clips is None:
        raise GoldenError("not a song: no <song> with <instruments> and <sessionClips>")
    found = list(instruments)
    owners = []  # (part, instrument) in roster order
    for part in parts:
        if found and (
            (found[0].tag == "kit" and found[0].get("presetName") == part)
            or (
                found[0].tag == "sound"
                and found[0].get("presetName") == sounds.get(part)
            )
        ):
            owners.append((part, found.pop(0)))
    if found:
        names = ", ".join(f"{i.tag} {i.get('presetName')}" for i in found)
        raise GoldenError(
            f"instruments no part of {', '.join(parts)} owns, in roster order: {names}"
        )
    notes = []
    for part, instrument in owners:
        kind = "instrumentClip"
        clip = next(
            (
                c
                for c in clips
                if c.tag == kind
                and c.get("instrumentPresetName") == instrument.get("presetName")
            ),
            None,
        )
        if clip is None:
            raise GoldenError(f"no clip for {part} ({instrument.get('presetName')})")
        drums = [s.get("name") for s in instrument.findall("soundSources/sound")]
        for row in clip.findall("noteRows/noteRow"):
            data = row.get("noteDataWithSplitProb")
            if data is None:
                continue  # a Kit row with no notes; a song with notes in another format fails below
            if row.get("drumIndex") is not None:
                name = drums[int(row.get("drumIndex"))]
                if name not in SONG_LANES:
                    raise GoldenError(f"{part}'s Kit row {name!r} is no lane's")
                key = GOLDEN_LANES[SONG_LANES.index(name)]
            else:
                key = note_name(int(row.get("y")))
            notes += [
                (tick, length, part, key, velocity)
                for tick, length, velocity in read_note_data(data)
            ]
        if any(
            r.get("noteData") or r.get("noteDataWithLift")
            for r in clip.findall("noteRows/noteRow")
        ):
            raise GoldenError(
                f"{part}'s clip has notes in an older format than noteDataWithSplitProb"
            )
    return notes


def dump(notes, cut, parts):
    """The notes as the golden's note lines, those starting more than MAX_LEAN before `cut` and cut there, sorted by
    tick, then by the part's place in the roster and the line's text."""
    events = []
    for tick, length, part, key, velocity in notes:
        if tick >= cut - MAX_LEAN:
            continue
        end = min(tick + length, cut)
        events.append(
            (tick, parts.index(part), event_line(tick, part, f"{key} on v{velocity}"))
        )
        events.append((end, parts.index(part), event_line(end, part, f"{key} off")))
    return [line for _, _, line in sorted(events)]


def recipe_problems(root, golden):
    """How the Recipe file differs from the golden's case: [text], empty when it is the same Recipe."""
    key, scale, bpm, cents, swing, *macros = golden.recipe
    want = {
        "genre": golden.genre,
        "genreVersion": str(golden.version),
        "tempo": str(int(bpm) * 100 + int(cents)),
        "swing": swing,
        "key": str(PITCH_NAMES.index(key)),
        "seed": str(golden.seed),
    }
    problems = [
        f"{name}={root.get(name)!r}, the golden's is {value!r}"
        for name, value in want.items()
        if root.get(name) != value
    ]
    if re.sub(r"[^a-z]", "", (root.get("scale") or "").lower()) != re.sub(
        r"[^a-z]", "", scale.lower()
    ):
        problems.append(f"scale={root.get('scale')!r}, the golden's is {scale!r}")
    held = root.find("macros")
    for name, points in zip(("energy", "density", "complexity", "motion"), macros):
        units = None if held is None else held.get(name)
        if units is None or int(units) != int(points) * MACRO_UNITS_PER_POINT:
            problems.append(f"{name}={units!r} units, the golden's is {points} points")
    problems += [
        f"part {p.get('name')} take={p.get('take')!r}, the golden plays take 0"
        for p in root.findall("part")
        if p.get("take") != "0"
    ]
    return problems


def compare(golden_text, song_xml, recipe_xml):
    """(problems, song_lines, golden_lines): the Recipe file's differences from the golden's case, then both note
    dumps. The check passes when there are no problems and the dumps are equal."""
    golden = Golden(golden_text)
    sounds, recipe_root = recipe_sounds(recipe_xml)
    problems = recipe_problems(recipe_root, golden)
    cut = golden.last_tick
    song = dump(song_notes(song_xml, golden.parts, sounds), cut, golden.parts)
    expected = dump(golden.notes, cut, golden.parts)
    return problems, song, expected
