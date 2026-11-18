"""Theory review (spec §21.5): a plain-text summary of what a genre plays, for an agent to read against spec §4.5.

    python3 scripts/generative/theory_review.py --genre techno-detroit [--entropies 1,2,3] [--bars 1000]
                                                [--render PATH] [--out DIR] [-- GENERATOR_RENDER_ARGS...]
    python3 scripts/generative/theory_review.py --midi FILE.mid...

For each entropy it renders the genre's default Recipe with the harness's generator_render (seed = the entropy, so each
run plays other draws too), one Arc cycle (`--end-after-drop --midi-only`: Intro, Grooves, Break, Build, the Drop's
phrase, Outro), and reads the MIDI file back: the notes, the Section markers, and the THEORY track (the Recipe, each
part's role, flavour, Density gate and interplay rules, and the key, chord and macros as they change). Arguments after
`--` go to generator_render as they are (`-- --density 40`). --midi summarises files rendered before instead.

Build the harness first (`./dbt test --generator -n`, which builds generator_render too); --render picks another
binary. Renders go to build/renders/theory/ (--out). The summary goes to stdout. The script reports and judges
nothing: what a part ought to do is in spec §4.5, and the review's reader decides. How a genre slice runs the review
and where its findings go: docs/generative/implementation-notes/build-and-tools.md, "Theory review".
"""

import argparse
import glob
import os
import statistics
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

TICKS_PER_STEP = 24  # 96 PPQN, a 16th
STEPS_PER_BAR = 16
TICKS_PER_BAR = TICKS_PER_STEP * STEPS_PER_BAR
PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MAJOR = [0, 2, 4, 5, 7, 9, 11]
ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"]


# ---- Reading the MIDI file ----


@dataclass
class Note:
    tick: int
    key: int  # MIDI note
    velocity: int
    end: int = (
        None  # the note-off's tick; None while it hasn't come (to the end of the track)
    )
    step: int = None  # the 16th it plays on: nearest_step(), or its main hit's for an Ornament hit (fold_ornaments())
    ornament: bool = False  # a ratchet's or roll's later hit, or a flam's grace (THEORY's `ornament` lines)

    def __post_init__(self):
        if self.step is None:
            self.step = nearest_step(self.tick)

    @property
    def bar(self):
        return self.step // STEPS_PER_BAR


def nearest_step(tick):
    """The 16th a note on `tick` plays on: Lean moves a note up to half a 16th (12 ticks) off its step, so the nearest
    one, with exactly half a 16th late counting on the next (v1 genres lean a few ticks at most)."""
    return (tick + TICKS_PER_STEP // 2) // TICKS_PER_STEP


def fold_ornaments(notes, ornaments):
    """Marks the notes THEORY lists as Ornament hits, {(tick, key): step}, and puts each on the step that decided it:
    its main hit's. Ticks alone can't tell a ratchet's last hit from the next 16th's hit on the same key, so the step
    comes from the render; for a line without one (an older render) the hit falls back to the last main hit on its key
    before it, or the one after it for a hit before every main hit (a flam's grace on the first)."""
    by_key = {}
    for n in notes:
        n.ornament = (n.tick, n.key) in ornaments
        by_key.setdefault(n.key, []).append(n)
    for hits in by_key.values():
        hits.sort(key=lambda n: n.tick)
        for i, n in enumerate(hits):
            if not n.ornament:
                continue
            step = ornaments[(n.tick, n.key)]
            before = next((m for m in reversed(hits[:i]) if not m.ornament), None)
            after = next((m for m in hits[i + 1 :] if not m.ornament), None)
            if step is not None:
                n.step = step
            elif before is not None:
                n.step = before.step
            elif after is not None:
                n.step = after.step


@dataclass
class Track:
    name: str = ""
    notes: list = field(default_factory=list)
    texts: list = field(default_factory=list)  # (tick, text): FF 01
    markers: list = field(default_factory=list)  # (tick, text): FF 06
    end: int = 0  # End of Track tick


def read_midi(data):
    """The tracks of a Standard MIDI File as generator_render writes it (no running status, no SysEx)."""
    if data[:4] != b"MThd":
        raise ValueError("not a MIDI file")
    count = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    if division != 4 * TICKS_PER_STEP:
        raise ValueError(f"expected 96 PPQN, got {division}")
    at = 8 + int.from_bytes(data[4:8], "big")
    tracks = []
    for _ in range(count):
        if data[at : at + 4] != b"MTrk":
            raise ValueError("bad track chunk")
        end = at + 8 + int.from_bytes(data[at + 4 : at + 8], "big")
        at += 8
        track, tick = Track(), 0
        sounding = {}  # key -> its notes still waiting for a note-off, oldest first

        def vlq():
            nonlocal at
            value = 0
            while True:
                b = data[at]
                at += 1
                value = (value << 7) | (b & 0x7F)
                if not b & 0x80:
                    return value

        while at < end:
            tick += vlq()
            status = data[at]
            if status == 0xFF:
                kind = data[at + 1]
                at += 2
                length = vlq()
                payload = data[at : at + length].decode("latin-1")
                at += length
                if kind == 0x03:
                    track.name = payload
                elif kind == 0x01:
                    track.texts.append((tick, payload))
                elif kind == 0x06:
                    track.markers.append((tick, payload))
                elif kind == 0x2F:
                    track.end = tick
            elif status & 0xF0 in (0x80, 0x90):
                key = data[at + 1]
                if status & 0xF0 == 0x90 and data[at + 2] > 0:
                    note = Note(tick, key, data[at + 2])
                    track.notes.append(note)
                    sounding.setdefault(key, []).append(note)
                elif sounding.get(key):
                    sounding[key].pop(0).end = tick
                at += 3
            else:
                raise ValueError(f"unexpected status byte {status:#x}")
        for note in track.notes:
            if note.end is None:
                note.end = track.end
        tracks.append(track)
        at = end
    return tracks


def fields(text):
    """ "word a=1 b=x" -> ("word", {"a": "1", "b": "x"}); bare words after the first go under their own name."""
    words = text.split()
    out = {}
    for w in words[1:]:
        k, _, v = w.partition("=")
        out[k] = v
    return words[0], out


# ---- The piece ----


@dataclass
class Part:
    name: str
    role: str
    flavour: str
    gate: float
    notes: list = field(default_factory=list)
    lanes: dict = field(
        default_factory=dict
    )  # lane name -> gate; Drums and FX parts only
    lane_notes: dict = field(default_factory=dict)  # MIDI note -> lane name

    @property
    def kit(self):
        return bool(self.lanes)


@dataclass
class Rule:
    source: str  # PART or PART.LANE
    kind: str
    target: str
    off: bool
    strength: float


@dataclass
class Piece:
    recipe: dict
    parts: list
    rules: list
    bars: int
    sections: list  # (bar, label), in order
    keys: list  # (tick, root pc, scale name, [pcs])
    chords: list  # (tick, [scale steps])
    macros: list  # (tick, energy, density)

    def section_at(self, bar):
        label = None
        for start, name in self.sections:
            if start > bar:
                break
            label = name
        return label or "?"

    def kind_at(self, bar):
        return self.section_at(bar).split()[0]

    def _last(self, items, tick):
        found = items[0] if items else None
        for item in items:
            if item[0] > tick:
                break
            found = item
        return found

    def key_at(self, tick):
        return self._last(self.keys, tick)

    def chord_at(self, tick):
        found = self._last(self.chords, tick)
        return found[1] if found else None

    def density_at_bar(self, bar):
        found = self._last(self.macros, bar * TICKS_PER_BAR)
        return found[2] if found else None

    def section_spans(self):
        """(first bar, end bar, label) per Section, in order."""
        for i, (start, label) in enumerate(self.sections):
            end = self.sections[i + 1][0] if i + 1 < len(self.sections) else self.bars
            yield start, end, label

    def onsets(self, source):
        """The 16th steps a part or PART.LANE has a note-on on: a ratchet's, roll's or flam's hits count once, on the
        main hit's step."""
        part_name, _, lane = source.partition(".")
        part = self.part(part_name)
        if part is None:
            return set()
        return {
            n.step for n in part.notes if not lane or part.lane_notes.get(n.key) == lane
        }

    def part(self, name):
        return next((p for p in self.parts if p.name == name), None)


def piece_from_midi(data):
    tracks = read_midi(data)
    theory = next((t for t in tracks if t.name == "THEORY"), None)
    if theory is None:
        raise ValueError(
            "no THEORY track: render it with a generator_render that writes one"
        )
    recipe, parts, rules, keys, chords, macros = {}, [], [], [], [], []
    ornaments = {}  # part name -> {(tick, MIDI note): the step that decided it, or None}
    for tick, text in theory.texts:
        word, f = fields(text)
        if word == "recipe":
            recipe = f
        elif word == "part":
            parts.append(
                Part(text.split()[1], f["role"], f["flavour"], float(f["gate"]))
            )
        elif word == "lane":
            part_name, lane = text.split()[1].split(".", 1)
            part = next(p for p in parts if p.name == part_name)
            part.lanes[lane] = float(f["gate"])
            part.lane_notes[int(f["note"])] = lane
        elif word == "rule":
            rules.append(
                Rule(
                    f["source"],
                    f["kind"],
                    f["target"],
                    f["off"] == "1",
                    float(f["strength"]),
                )
            )
        elif word == "key":
            root = PITCH_NAMES.index(f["root"])
            keys.append((tick, root, f["scale"], [int(p) for p in f["pcs"].split(",")]))
        elif word == "chord":
            chords.append((tick, [int(s) for s in text.split()[1].split(",")]))
        elif word == "macros":
            macros.append((tick, float(f["energy"]), float(f["density"])))
        elif word == "ornament":
            step = int(f["step"]) if "step" in f else None
            ornaments.setdefault(f["part"], {})[(tick, int(f["note"]))] = step
    by_name = {t.name: t for t in tracks}
    for part in parts:
        if part.name in by_name:
            part.notes = by_name[part.name].notes
            fold_ornaments(part.notes, ornaments.get(part.name, {}))
    conductor = tracks[0]
    sections = [(tick // TICKS_PER_BAR, label) for tick, label in conductor.markers]
    bars = max(t.end for t in tracks) // TICKS_PER_BAR
    return Piece(recipe, parts, rules, bars, sections, keys, chords, macros)


# ---- Music helpers ----


def note_name(key):
    return f"{PITCH_NAMES[key % 12]}{key // 12 - 1}"


def scale_pc(key_entry, steps):
    """The pitch class `steps` scale steps above the key's tonic."""
    pcs = key_entry[3]
    return pcs[steps % len(pcs)]


INTERVAL_NAMES = ["1", "b2", "2", "b3", "3", "4", "#4", "5", "b6", "6", "b7", "7"]


def degree_label(key_entry, pc):
    """A pitch class as a degree of the key: "1", "b3", "5"; None when it isn't in the scale. A 7-note scale names
    each note by its degree against the major scale's ("b5" in locrian); any other size by its interval from the
    root, so a 5-note scale reads "1 b3 4 5 b7"."""
    root, pcs = key_entry[1], key_entry[3]
    if pc not in pcs:
        return None
    semitones = (pc - root) % 12
    d = pcs.index(pc)
    if len(pcs) == 7:
        accidental = {0: "", 11: "b", 10: "bb", 1: "#", 2: "##"}.get(
            (semitones - MAJOR[d]) % 12
        )
        if accidental is not None:
            return f"{accidental}{d + 1}"
    return INTERVAL_NAMES[semitones]


def degree_order(key_entry, label_pc):
    """A sort key for (label, pitch class): by semitones above the root, out-of-scale notes last."""
    label, pc = label_pc
    return (pc not in key_entry[3], (pc - key_entry[1]) % 12, label)


def chord_name(key_entry, steps):
    """A chord's Roman numeral in the key: its root's degree, upper case for a major 3rd, ° for a diminished 5th,
    + for an augmented one, b or # where the root sits off the major scale. "i", "bVII", "ii°"."""
    pcs, root_pc = key_entry[3], key_entry[1]
    n = len(pcs)
    r = steps[0]
    third = (scale_pc(key_entry, r + 2) - scale_pc(key_entry, r)) % 12
    fifth = (scale_pc(key_entry, r + 4) - scale_pc(key_entry, r)) % 12
    d = r % n
    numeral = ROMAN[d] if third == 4 else ROMAN[d].lower()
    off = (pcs[d] - root_pc - MAJOR[d]) % 12
    accidental = {0: "", 11: "b", 1: "#"}.get(off, "?")
    suffix = "°" if fifth == 6 else "+" if fifth == 8 else ""
    return accidental + numeral + suffix


def pct(n, of):
    return 100.0 * n / of if of else 0.0


def runs(items):
    """Consecutive equal items as [item, count] pairs."""
    out = []
    for item in items:
        if out and out[-1][0] == item:
            out[-1][1] += 1
        else:
            out.append([item, 1])
    return out


def span_text_of(changes, unit, start, end):
    """The spans from `start` to each change and between changes, in bars, with their counts, then the last one, which
    the piece's end cuts off: "2 bars x30, 4 bars x2; last 2 bars to the end"."""
    spans, prev = Counter(), start
    for t in changes:
        spans[(t - prev) / unit] += 1
        prev = t
    whole = (
        ", ".join(f"{s:g} bars x{n}" for s, n in sorted(spans.items())) or "no change"
    )
    return f"{whole}; last {(end - prev) / unit:g} bars to the end"


def heard_per_bar(piece):
    """Per bar, the pitch classes the Chords and Pad parts sound in it, as "[1 b3 5]"; [] when the piece has none."""
    parts = [p for p in piece.parts if p.role in ("Chords", "Pad")]
    if not parts:
        return []
    sets = [set() for _ in range(piece.bars)]
    for part in parts:
        for n in part.notes:
            for bar in range(
                n.bar, min(piece.bars, max(n.bar, (n.end - 1) // TICKS_PER_BAR) + 1)
            ):
                sets[bar].add(n.key % 12)
    out = []
    for bar, pcs in enumerate(sets):
        key = piece.key_at(bar * TICKS_PER_BAR)
        named = sorted(
            ((degree_label(key, pc) or f"{PITCH_NAMES[pc]}!", pc) for pc in pcs),
            key=lambda item: degree_order(key, item),
        )
        labels = [label for label, _ in named]
        out.append("[" + " ".join(labels) + "]")
    return out


# ---- The summary ----


def summarise_piece(piece):
    """The per-entropy block: the Arc, the key, chords, ranges, notes per bar, gates. Lines, no trailing newline."""
    r = piece.recipe
    lines = []
    key0 = piece.keys[0]
    lines.append(
        f"== entropy {r.get('entropy')} (seed {r.get('seed')}): {PITCH_NAMES[key0[1]]} {key0[2]}, {r.get('tempo')} BPM,"
        f" swing {r.get('swing')}; energy {r.get('energy')} density {r.get('density')} complexity"
        f" {r.get('complexity')} motion {r.get('motion')}; {piece.bars} bars"
    )
    arc = []
    for bar, end, label in piece.section_spans():
        arc.append(f"{label} {bar}-{end - 1}")
    lines.append("Arc (bars): " + " | ".join(arc))
    if len(piece.keys) > 1:
        lines.append(
            "Key changes: "
            + ", ".join(
                f"bar {t // TICKS_PER_BAR} {PITCH_NAMES[k]} {s}"
                for t, k, s, _ in piece.keys[1:]
            )
        )

    # Chords: the sequence per Section, the harmonic rhythm, each degree's share of bars.
    lines.append("Chords (Roman numeral x bars, per Section):")
    per_bar = []
    for bar in range(piece.bars):
        steps = piece.chord_at(bar * TICKS_PER_BAR)
        per_bar.append(
            chord_name(piece.key_at(bar * TICKS_PER_BAR), steps) if steps else "?"
        )
    for start, end, label in piece.section_spans():
        lines.append(
            f"  {label}: " + " ".join(f"{c}x{n}" for c, n in runs(per_bar[start:end]))
        )
    changes = [t for t, _ in piece.chords[1:]]
    off_bar = [t for t in changes if t % TICKS_PER_BAR]
    lines.append(
        f"Harmonic rhythm: {len(changes)} changes, spans"
        f" {span_text_of(changes, TICKS_PER_BAR, 0, piece.bars * TICKS_PER_BAR)};"
        f" {len(off_bar)} off a bar line"
        + (f" (first at bar {off_bar[0] / TICKS_PER_BAR:.2f})" if off_bar else "")
    )
    shares = Counter(per_bar)
    lines.append(
        "Degree share of bars: "
        + ", ".join(f"{c} {pct(n, piece.bars):.0f}%" for c, n in shares.most_common())
    )

    # What the Chords and Pad parts sound, from their notes: the harmony heard, beside the Generator's chord above.
    heard = heard_per_bar(piece)
    if heard:
        lines.append(
            "Heard (pitch classes the Chords and Pad parts sound in each bar, from the notes, as scale degrees;"
            " ! = out of scale; [] = silent; x bars, per Section):"
        )
        for start, end, label in piece.section_spans():
            lines.append(
                f"  {label}: " + " ".join(f"{c}x{n}" for c, n in runs(heard[start:end]))
            )
        # From the first bar they sound in: silence before it is no harmonic rhythm.
        first = next((bar for bar, h in enumerate(heard) if h != "[]"), None)
        if first is None:
            lines.append("Heard harmonic rhythm: the Chords and Pad parts never sound")
        else:
            changes = [
                bar
                for bar in range(first + 1, piece.bars)
                if heard[bar] != heard[bar - 1]
            ]
            lines.append(
                f"Heard harmonic rhythm (from bar {first}): {len(changes)} changes, spans"
                f" {span_text_of(changes, 1, first, piece.bars)}"
            )

    # Ranges and overlaps.
    lines.append("Range (lowest-highest, median) and velocity per pitched part:")
    ranges = {}
    for part in piece.parts:
        if part.kit:
            continue
        if not part.notes:
            lines.append(f"  {part.name}: no notes")
            continue
        keys = [n.key for n in part.notes]
        vel = [n.velocity for n in part.notes]
        ranges[part.name] = (min(keys), max(keys))
        lines.append(
            f"  {part.name}: {note_name(min(keys))}-{note_name(max(keys))}, median {note_name(int(statistics.median_low(keys)))};"
            f" velocity {min(vel)}-{max(vel)}"
        )
    names = list(ranges)
    overlaps = []
    for i, a in enumerate(names):
        for b in names[i + 1 :]:
            lo, hi = max(ranges[a][0], ranges[b][0]), min(ranges[a][1], ranges[b][1])
            if lo <= hi:
                overlaps.append(
                    f"{a}/{b} {note_name(lo)}-{note_name(hi)} ({hi - lo} st)"
                )
    lines.append("Range overlaps: " + ("; ".join(overlaps) if overlaps else "none"))

    # Notes per bar per part, per Section.
    lines.append(
        "Notes per bar (note-ons, per Section as it played, so the Drop is the Groove after the Build; ramp: the"
        " Section's first bar -> last bar, all parts):"
    )
    header = (
        "  "
        + "Section".ljust(10)
        + "".join(p.name[:8].rjust(9) for p in piece.parts)
        + "   ramp"
    )
    lines.append(header)
    for start, end, label in piece.section_spans():
        bars = max(end - start, 1)
        row = "  " + label.ljust(10)
        for part in piece.parts:
            n = sum(1 for note in part.notes if start <= note.bar < end)
            row += f"{n / bars:9.1f}"
        first = sum(1 for p in piece.parts for n in p.notes if n.bar == start)
        last = sum(1 for p in piece.parts for n in p.notes if n.bar == end - 1)
        lines.append(row + f"   {first}->{last}")

    # Density gates: notes in bars whose bar-line Density is below the gate, in its hysteresis band, at or above it.
    hysteresis = float(r.get("hysteresis", "2.0"))
    gated = []
    for part in piece.parts:
        if part.gate > 0:
            gated.append((part.name, part.gate))
        for lane, gate in part.lanes.items():
            if gate > 0:  # a lane plays while its part's gate and its own are both open
                gated.append((f"{part.name}.{lane}", max(gate, part.gate)))
    if gated:
        lines.append(
            f"Density gates (notes per bucket of bars, by the effective Density sampled on each bar line, where gates"
            f" open and close; below = at or under gate - {hysteresis:g}, where every gate is closed; Density can move"
            " within a bar, as in a Build's ramp):"
        )
        for source, gate in gated:
            buckets = {"below": [0, 0], "band": [0, 0], "open": [0, 0]}
            part_name, _, lane = source.partition(".")
            part = piece.part(part_name)
            per_bar = Counter(
                n.bar
                for n in part.notes
                if not lane or part.lane_notes.get(n.key) == lane
            )
            for bar in range(piece.bars):
                d = piece.density_at_bar(bar)
                if d is None:
                    continue
                # As gateOpens(): a closed gate opens at the gate; an open one closes at gate - hysteresis or under.
                b = (
                    "below"
                    if d <= gate - hysteresis
                    else "open"
                    if d >= gate
                    else "band"
                )
                buckets[b][0] += 1
                buckets[b][1] += per_bar.get(bar, 0)
            lines.append(
                f"  {source} gate {gate:g}: "
                + "; ".join(
                    f"{k} {v[0]} bars, {v[1]} notes" for k, v in buckets.items()
                )
            )
    return lines


def onset_grids(pieces):
    """Per Section kind, across every piece: for each Drums lane and pitched part, the % of the kind's bars with an
    onset on each 16th."""
    lines = []
    kinds = []
    for piece in pieces:
        for _, label in piece.sections:
            k = label.split()[0]
            if k not in kinds:
                kinds.append(k)
    sources = []
    for part in pieces[0].parts:
        if part.kit:
            sources += [f"{part.name}.{lane}" for lane in part.lanes]
        else:
            sources.append(part.name)
    lines.append(
        "Onsets by 16th (% of the Section kind's bars with an onset there, so a cell's count is % x bars / 100; 1 e & a"
        " per beat; '.' = none; the last column counts the row's onsets; rows with no onsets left out):"
    )
    lines.append("  " + " " * 14 + "".join(f"{c:>4}" for c in "1e&a" * 4) + "  onsets")
    for kind in kinds:
        total_bars = sum(
            1 for p in pieces for bar in range(p.bars) if p.kind_at(bar) == kind
        )
        lines.append(f"  {kind} ({total_bars} bars)")
        for source in sources:
            counts = [0] * STEPS_PER_BAR
            onsets = 0
            for piece in pieces:
                for s in piece.onsets(source):
                    bar = s // STEPS_PER_BAR
                    if bar < piece.bars and piece.kind_at(bar) == kind:
                        counts[s % STEPS_PER_BAR] += 1
                        onsets += 1
            if onsets == 0:
                continue
            cells = "".join(
                f"{max(1, round(pct(c, total_bars))):>4}" if c else "   ."
                for c in counts
            )
            lines.append(f"    {source[:12]:<12}{cells}  {onsets}")
    return lines


def pitch_classes(pieces):
    """Each pitched part's notes by scale degree, its out-of-scale notes (a bug), and its share on a chord tone."""
    lines = [
        "Pitch classes (scale degree: % of the part's notes; on chord = a tone of the chord sounding then):"
    ]
    names = [p.name for p in pieces[0].parts if not p.kit]
    for name in names:
        degrees, out, on_chord, total = Counter(), [], 0, 0
        semitones = {}  # label -> semitones above the root, to order the labels
        for piece in pieces:
            for n in piece.part(name).notes:
                total += 1
                key = piece.key_at(n.step * TICKS_PER_STEP)
                label = degree_label(key, n.key % 12)
                if label is None:
                    out.append(
                        f"{note_name(n.key)}@bar{n.bar}(e{piece.recipe.get('entropy')})"
                    )
                else:
                    degrees[label] += 1
                    semitones.setdefault(label, (n.key - key[1]) % 12)
                chord = piece.chord_at(n.step * TICKS_PER_STEP)
                if chord and n.key % 12 in {scale_pc(key, s) for s in chord}:
                    on_chord += 1
        if total == 0:
            lines.append(f"  {name}: no notes")
            continue
        order = sorted(degrees, key=lambda d: (semitones[d], d))
        lines.append(
            f"  {name}: "
            + " ".join(f"{d}:{pct(degrees[d], total):.0f}%" for d in order)
            + f"; out of scale {len(out)}"
            + (
                f" ({', '.join(out[:5])}{' ...' if len(out) > 5 else ''})"
                if out
                else ""
            )
            + f"; on chord {pct(on_chord, total):.0f}%"
        )
    return lines


def interplay(pieces):
    """For each rule: its source's onsets on the target's onsets, on the target's offbeats (an onset one 8th before)
    and in the target's against half-bar as Respond reads it (the busier half, the first on a non-zero tie, spec
    §5.5), over the bars where the target plays (a rule has nothing to act on in the others), and on and offbeat over
    all bars. Halves are counted from the target's heard onsets, not the planned ones Respond counts, so a muted,
    kept-out or filled target bar can read differently."""
    rules = pieces[0].rules
    if not rules:
        return ["Interplay rules: none"]
    lines = [
        (
            "Interplay rules (the source's onsets in bars where the target has one: on the target's onsets / offbeat,"
            " an 8th after a target onset / in the target's against half-bar, the busier one or the first on a tie;"
            " then over all bars. Lock wants on, Lock off wants offbeat, Avoid wants on 0%, Respond at 100% wants"
            " against half 0%):"
        )
    ]
    for index, rule in enumerate(rules):
        total = counted = on = offbeat = against = on_all = offbeat_all = 0
        # Each render's own strength: it moves with Complexity, which --midi files may not share.
        strengths = [p.rules[index].strength for p in pieces if index < len(p.rules)]
        strength = (
            f"{strengths[0]:g}%"
            if len(set(strengths)) == 1
            else "/".join(f"{x:g}" for x in strengths) + "% by render"
        )
        for piece in pieces:
            src, tgt = piece.onsets(rule.source), piece.onsets(rule.target)
            target_bars = {t // STEPS_PER_BAR for t in tgt}
            for s in src:
                total += 1
                on_all += s in tgt
                offbeat_all += (s - 2) in tgt
                bar, pos = divmod(s, STEPS_PER_BAR)
                if bar not in target_bars:
                    continue
                counted += 1
                on += s in tgt
                offbeat += (s - 2) in tgt
                first = sum(1 for p in range(8) if bar * STEPS_PER_BAR + p in tgt)
                second = sum(1 for p in range(8, 16) if bar * STEPS_PER_BAR + p in tgt)
                first_against = first > second or (first == second and first > 0)
                if first_against if pos < 8 else second > first:
                    against += 1
        lines.append(
            f"  {rule.source} {rule.kind}{' off' if rule.off else ''} -> {rule.target} at {strength}:"
            f" {counted} of {total} onsets in bars the target plays: on {pct(on, counted):.0f}%"
            f" / offbeat {pct(offbeat, counted):.0f}% / against half {pct(against, counted):.0f}%;"
            f" all bars: on {pct(on_all, total):.0f}% / offbeat {pct(offbeat_all, total):.0f}%"
        )
    return lines


def velocities(pieces):
    lines = ["Velocity (min-max, median) per Drums lane and part:"]
    for part in pieces[0].parts:
        groups = (
            [(f"{part.name}.{lane}", lane) for lane in part.lanes]
            if part.kit
            else [(part.name, None)]
        )
        for label, lane in groups:
            vel = [
                n.velocity
                for piece in pieces
                for n in piece.part(part.name).notes
                if lane is None or piece.part(part.name).lane_notes.get(n.key) == lane
            ]
            if vel:
                lines.append(
                    f"  {label}: {min(vel)}-{max(vel)}, median {statistics.median_low(vel)}"
                )
    return lines


def summary(pieces):
    """The whole review text for pieces of one genre."""
    genre = pieces[0].recipe.get("genre", "?")
    parts = ", ".join(
        f"{p.name} ({p.role}{'' if p.flavour == 'None' else ' ' + p.flavour}{f', gate {p.gate:g}' if p.gate else ''})"
        for p in pieces[0].parts
    )
    lines = [
        (
            f"THEORY REVIEW: {genre}, {len(pieces)} renders of its default Recipe, one Arc cycle each"
            " (generator_render; spec §21.5)"
        ),
        f"Parts: {parts}",
        "Bars count from 0; a step is a 16th. Macros and gates are OLED points (0-50).",
        "",
    ]
    for piece in pieces:
        lines += summarise_piece(piece) + [""]
    lines.append(f"== all {len(pieces)} renders, by Section kind")
    for block in (onset_grids, pitch_classes, interplay, velocities):
        lines += block(pieces) + [""]
    return "\n".join(lines).rstrip() + "\n"


# ---- Rendering ----


def find_render():
    """The newest generator_render the harness build made."""
    found = glob.glob(
        os.path.join(REPO, "build", "tests", "**", "generator_render"), recursive=True
    )
    found = [f for f in found if os.path.isfile(f) and os.access(f, os.X_OK)]
    return max(found, key=os.path.getmtime) if found else None


def render_command(render, genre, entropy, bars, out, extra):
    return [
        render,
        "--genre",
        genre,
        "--entropy",
        str(entropy),
        "--seed",
        str(entropy),
        "--bars",
        str(bars),
        "--end-after-drop",
        "--midi-only",
        "--out",
        out,
        *extra,
    ]


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    extra = []
    if "--" in argv:
        at = argv.index("--")
        argv, extra = argv[:at], argv[at + 1 :]
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--genre", help="a genre's stable name, e.g. techno-detroit")
    source.add_argument(
        "--midi", nargs="+", help="summarise these generator_render MIDI files instead"
    )
    parser.add_argument(
        "--entropies",
        default="1,2,3",
        help="comma-separated, at least 3 for a review (default 1,2,3)",
    )
    parser.add_argument(
        "--bars",
        type=int,
        default=1000,
        help="the most bars a render plays (default 1000)",
    )
    parser.add_argument(
        "--render",
        help="the generator_render binary (default: the newest under build/tests/)",
    )
    parser.add_argument(
        "--out",
        default=os.path.join(REPO, "build", "renders", "theory"),
        help="where renders go",
    )
    args = parser.parse_args(argv)

    paths = args.midi or []
    if args.genre:
        render = args.render or find_render()
        if not render:
            sys.exit(
                "theory_review: no generator_render under build/tests/: run ./dbt test --generator -n first"
            )
        os.makedirs(args.out, exist_ok=True)
        for entropy in [int(e) for e in args.entropies.split(",")]:
            out = os.path.join(args.out, f"{args.genre}-e{entropy}")
            cmd = render_command(render, args.genre, entropy, args.bars, out, extra)
            done = subprocess.run(cmd, capture_output=True, text=True, check=False)
            if done.returncode != 0:
                sys.exit(f"theory_review: {' '.join(cmd)} failed:\n{done.stderr}")
            for warning in done.stderr.splitlines():
                print(f"theory_review: {warning}", file=sys.stderr)
            paths.append(out + ".mid")
    pieces = []
    for path in paths:
        with open(path, "rb") as f:
            piece = piece_from_midi(f.read())
        # Not one Arc cycle: the summary's Outro, Drop and Arc lines would mislead.
        if not piece.sections or not piece.sections[-1][1].startswith("OUTRO"):
            message = f"theory_review: {path} has no Outro at its end, so it isn't one Arc cycle"
            if args.genre:
                sys.exit(f"{message}: no Drop came in --bars {args.bars}; raise --bars")
            print(f"{message}; summarising it anyway", file=sys.stderr)
        pieces.append(piece)
    genres = {p.recipe.get("genre") for p in pieces}
    if len(genres) > 1:
        sys.exit(
            f"theory_review: the files are of different genres: {', '.join(sorted(map(str, genres)))}"
        )
    sys.stdout.write(summary(pieces))


if __name__ == "__main__":
    main()
