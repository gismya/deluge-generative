"""A whole-song clip's timeline strip (#513): which parts play in each Section, each labelled with its sound.

The data comes from the harness MIDI render of the clip's Recipe (generator_render --midi-only, with the capture job's
flags, End included), read with theory_review.piece_from_midi: its Sections (the conductor's markers) and each part's
notes. The parts' sounds come from `generator_render --list-sounds` for the same Recipe: the palette roll the
firmware makes from the Recipe's entropy, over the repo's pack/ and the stock card (a synth part's preset, a Drums
part's Kit, an FX part's lane samples). A capture on a card with a palette override or other samples plays other
sounds: give the clip's timeline `sounds` then.

Bars are counted from the cut's first bar, so a clip cut from bar 16 starts its strip at bar 16's Section. A part is
on in a bar where any of its notes sounds (a held pad counts); runs of such bars merge into one span. Each bar keeps
its note-on count, for shading.
"""

import math
import os
import shlex
import subprocess
import tempfile

from . import jobs as _jobs
from . import variants as _variants

theory = _jobs.gen_emu.theory_review
# Part names shown as the MIDI track spells them; the rest read capitalised ("BASS" -> "Bass").
KEEP_CASE = {"FX"}
TIMELINE_KEYS = ("args", "variant", "bars", "sounds")
REUSED = "a reused clip's timeline is a dict giving its args and bars"


def stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def sound_labels(text):
    """{PART: (sound, detail)} from generator_render --list-sounds: a synth part's preset name; a Drums part's Kit
    ("909 kit"), its swapped rows in the detail; a part of lanes without a Kit (FX), its lanes' sounds."""
    singles, kits, lanes = {}, {}, {}
    for line in text.splitlines():
        cols = line.split("\t")
        if len(cols) < 2:
            continue
        name, _, lane = cols[0].partition(".")
        if lane == "kit":
            swaps = cols[2] if len(cols) > 2 and cols[2] != "-" else ""
            kits[name] = (cols[1], swaps.replace("=", " ").replace(",", ", "))
        elif lane:
            if cols[1] != "-":
                lanes.setdefault(name, []).append((lane, stem(cols[1])))
        elif cols[1] != "-":
            singles[name] = stem(cols[1])
    out = {name: (sound, "") for name, sound in singles.items()}
    for name, (kit, swaps) in kits.items():
        out[name] = (kit + " kit", "swapped in: " + swaps if swaps else "")
    for name, found in lanes.items():
        if name not in out:
            out[name] = (
                ", ".join(s for _, s in found),
                "; ".join(f"{lane}: {s}" for lane, s in found),
            )
    return out


def section_labels(sections):
    """[(bar, label)] with each Groove a Build lands in named as its Drop (GLOSSARY): "DROP 1" for "GROOVE 2"."""
    out, drops, previous = [], 0, ""
    for bar, label in sections:
        kind = label.split()[0] if label else ""
        if kind == "GROOVE" and previous == "BUILD":
            drops += 1
            label = f"DROP {drops}"
        out.append((bar, label))
        previous = kind
    return out


def part_bars(part, bars):
    """(on, onsets) per bar 0..bars-1: whether any note of the part sounds in it, and its note-ons there."""
    tpb = theory.TICKS_PER_BAR
    on, onsets = [False] * bars, [0] * bars
    for n in part.notes:
        first = n.tick // tpb
        if 0 <= first < bars:
            onsets[first] += 1
        end = n.end if n.end is not None and n.end > n.tick else n.tick + 1
        for b in range(max(first, 0), min((end - 1) // tpb + 1, bars)):
            on[b] = True
    return on, onsets


def strip(piece, sounds, first, end, muted=()):
    """The page's timeline for a clip cut [first, end) bars from `piece` (theory_review.Piece). Bars are from the
    cut's start. `sounds` is sound_labels()'s; `muted` holds the part numbers (1 = the first) the clip leaves out."""
    tempo = float(piece.recipe["tempo"])
    length = end - first
    lo, hi = math.floor(first), math.ceil(end)  # whole bars the cut touches
    sections = []
    labels = section_labels(piece.sections)
    for i, (start, label) in enumerate(labels):
        stop = labels[i + 1][0] if i + 1 < len(labels) else piece.bars
        s, e = max(start, first), min(stop, end)
        if s < e:
            sections.append([round(s - first, 3), round(e - first, 3), label])
    parts = []
    for i, part in enumerate(piece.parts):
        if i + 1 in muted:
            continue
        on, onsets = part_bars(part, max(hi, 1))
        spans, current = [], None
        for b in range(lo, hi):
            if on[b] and current is None:
                current = [b, []]
            if on[b]:
                current[1].append(onsets[b])
            if current is not None and (not on[b] or b == hi - 1):
                stop = b + 1 if on[b] else b
                spans.append(
                    [
                        round(max(current[0], first) - first, 3),
                        round(min(stop, end) - first, 3),
                        current[1],
                    ]
                )
                current = None
        sound, detail = sounds.get(part.name, ("", ""))
        parts.append(
            {
                "name": part.name if part.name in KEEP_CASE else part.name.capitalize(),
                "sound": sound,
                "detail": detail,
                "spans": spans,
            }
        )
    return {
        "barSeconds": round(_jobs.gen_emu.bar_seconds(tempo), 6),
        "bars": round(length, 3),
        "first": first,
        "sections": sections,
        "parts": parts,
    }


def spec(clip, info=None):
    """(args, variant, [first, end], sounds) for a clip's timeline: True takes the capture's sidecar `info`; a dict
    gives args (and optionally variant, bars, sounds) itself, as a reused clip must."""
    t = clip["timeline"]
    given = t if isinstance(t, dict) else {}
    if info is None and "args" not in given:
        raise ValueError(REUSED)
    args = given.get("args") or info["args"]
    variant = given.get("variant") or (info or {}).get("variant") or "base"
    bars = list(given.get("bars") or clip.get("bars") or [])
    if len(bars) != 2 or not bars[0] < bars[1]:
        raise ValueError("a timeline needs bars [first, end)")
    return args, variant, bars, given.get("sounds")


def problems(where, clip):
    """What's wrong with a clip's `timeline` key, one line each."""
    t = clip.get("timeline")
    if t is None:
        return []
    if t is not True and not isinstance(t, dict):
        return [f"{where}: timeline is True or a dict"]
    given = t if isinstance(t, dict) else {}
    out = [
        f"{where}: timeline key {k} unknown" for k in given if k not in TIMELINE_KEYS
    ]
    if "reuse" in clip and not (given.get("args") and given.get("bars")):
        out.append(f"{where}: {REUSED}")
    sounds = given.get("sounds", {})
    if not isinstance(sounds, dict) or not all(
        isinstance(k, str) and isinstance(v, str) for k, v in sounds.items()
    ):
        out.append(f"{where}: timeline sounds is {{PART: name}}")
    return out


def run(cmd):
    """generator_render's stdout; RuntimeError with the command and its stderr if it fails."""
    done = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise RuntimeError(
            f"{shlex.join(cmd)}: {(done.stderr or done.stdout).strip()[-300:]}"
        )
    return done.stdout


def merged_sounds(listed, given, parts):
    """The names --list-sounds gave (`listed`, sound_labels()'s), each part in `given` ({PART: name}) overriding its
    own. ValueError naming a `given` key that is none of the piece's `parts`."""
    unknown = sorted(set(given) - set(parts))
    if unknown:
        raise ValueError(
            f"timeline sounds: {', '.join(unknown)} isn't a part (have {', '.join(parts)})"
        )
    return {**listed, **{k: (v, "") for k, v in given.items()}}


def build(clip, scratch, session, genre, info=None):
    """A clip's timeline: renders its Recipe's MIDI and lists its sounds, with the clip's generator_render (a
    current bin/<variant> of the session folder `session`, or the harness build's for base)."""
    args, variant, (first, end), given_sounds = spec(clip, info)
    recipe, mutes, _quiet, end_flags = _jobs.parse_args(args, genre)
    render = _variants.current_render(_jobs.gen_emu.REPO, scratch, session, variant)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "timeline")
        cmd = [
            render,
            *recipe.flags(),
            *end_flags,
            "--bars",
            str(math.ceil(end) + 1),
            "--midi-only",
            "--out",
            out,
        ]
        run(cmd)
        if not os.path.exists(out + ".mid"):
            raise RuntimeError(f"{shlex.join(cmd)}: wrote no {out}.mid")
        with open(out + ".mid", "rb") as fh:
            piece = theory.piece_from_midi(fh.read())
    listed = sound_labels(run([render, "--list-sounds", *recipe.flags()]))
    sounds = merged_sounds(listed, given_sounds or {}, [p.name for p in piece.parts])
    return strip(piece, sounds, first, end, mutes)
