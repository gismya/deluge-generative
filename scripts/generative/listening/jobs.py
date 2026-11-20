"""The job file: one capture request a line, `name | variant | card | bars | args`.

- name: the clip source's name (raw/<name>.wav once captured);
- variant: `base`, or a variant whose diff is <session>/diffs/<variant>.diff;
- card: the card image <scratch>/card_<card>.img, or a path ending in .img;
- bars: how many bars from PLAY (bar 0) the clips need; the seconds come from the harness render's tempo;
- args: gen_emu.py capture's Recipe flags and --mute, split with shlex and parsed by gen_emu's own parser, so a bad
  flag fails when the file loads.

A comment is a whole line starting with `#`. Never cut a line at `#`: keys are spelled with it (`--key D#`).
"""

import argparse
import contextlib
import io
import os
import shlex
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import gen_emu

# Flags the runner sets per capture: a job may not give them.
RUNNER_FLAGS = (
    "--out",
    "--build-dir",
    "--card",
    "--seconds",
    "--no-play",
    "--render",
    "--ab",
    "--no-check",
)
RECIPE_FIELDS = (
    "genre",
    "seed",
    "entropy",
    *gen_emu.MACROS,
    "key",
    "scale",
    "tempo",
    "swing",
)


class JobError(ValueError):
    pass


@dataclass(frozen=True)
class Recipe:
    genre: str
    seed: int
    entropy: int
    energy: "int | None" = None
    density: "int | None" = None
    complexity: "int | None" = None
    motion: "int | None" = None
    key: "str | None" = None
    scale: "str | None" = None
    tempo: "float | None" = None
    swing: "int | None" = None

    def flags(self):
        """The Recipe as gen_emu/generator_render flags, only the ones given (and --genre, --seed, --entropy)."""
        out = [
            "--genre",
            self.genre,
            "--seed",
            str(self.seed),
            "--entropy",
            str(self.entropy),
        ]
        for name in RECIPE_FIELDS[3:]:
            value = getattr(self, name)
            if value is not None:
                out += [
                    "--" + name,
                    f"{value:g}" if isinstance(value, float) else str(value),
                ]
        return out

    def sort_key(self):
        return tuple(
            (v is not None, str(v) if v is not None else "")
            for v in (getattr(self, f) for f in RECIPE_FIELDS)
        )


@dataclass(frozen=True)
class Job:
    name: str
    variant: str
    card: str
    bars: float
    recipe: Recipe
    mutes: tuple = ()
    line: int = 0
    args: str = field(default="", compare=False)

    def group(self):
        """What a capture shares with a longer one it is a prefix of: (variant, card, Recipe, mutes)."""
        return (self.variant, self.card, self.recipe, self.mutes)

    def capture_flags(self):
        return self.recipe.flags() + [f for m in self.mutes for f in ("--mute", str(m))]


_PARSER = None
_NAMES = {}


def _names(kind, read):
    """gen_emu's genre or scale names, read from the source once."""
    if kind not in _NAMES:
        _NAMES[kind] = read()
    return _NAMES[kind]


def _recipe_parser():
    global _PARSER
    if _PARSER is None:
        # allow_abbrev=False: argparse would read --sec as --seconds and --rend as --render, which the runner drops
        p = argparse.ArgumentParser(prog="job args", add_help=False, allow_abbrev=False)
        gen_emu.add_recipe_args(p)
        _PARSER = p
    return _PARSER


def parse_args(args, genre):
    """(Recipe, mutes) from a job's args. Raises JobError on a flag gen_emu's capture would refuse, or one the runner
    sets itself."""
    try:
        words = shlex.split(args)
    except ValueError as e:
        raise JobError(f"args: {e}") from None
    for w in words:
        if w.split("=", 1)[0] in RUNNER_FLAGS:
            raise JobError(f"{w}: the runner sets it")
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            ns = _recipe_parser().parse_args(["--genre", genre, *words])
    except SystemExit:
        raise JobError(err.getvalue().strip().splitlines()[-1]) from None
    if ns.key is not None and ns.key not in gen_emu.KEYS:
        raise JobError(f"--key {ns.key}: unknown key")
    if ns.scale is not None and ns.scale not in _names("scales", gen_emu.scale_names):
        raise JobError(
            f"--scale {ns.scale}: unknown scale (have {', '.join(_names('scales', gen_emu.scale_names))})"
        )
    if ns.genre not in _names("genres", gen_emu.genre_names):
        raise JobError(
            f"--genre {ns.genre}: unknown genre (have {', '.join(_names('genres', gen_emu.genre_names))})"
        )
    recipe = Recipe(**{f: getattr(ns, f) for f in RECIPE_FIELDS})
    return recipe, tuple(sorted(set(ns.mute or [])))


def parse_jobs(text, genre, source="jobs"):
    """The Jobs of a job file's text. Raises JobError naming the line on any bad line or a repeated name."""
    out, seen = [], set()
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cols = [c.strip() for c in line.split("|")]
        try:
            if len(cols) != 5:
                raise JobError(
                    f"{len(cols)} columns, not 5 (name | variant | card | bars | args)"
                )
            name, variant, card, bars, args = cols
            if not name or name in seen:
                raise JobError(f"name {name!r} empty or repeated")
            try:
                bars = float(bars)
            except ValueError:
                raise JobError(f"bars {bars!r}: not a number") from None
            if bars <= 0:
                raise JobError("bars must be more than 0")
            recipe, mutes = parse_args(args, genre)
        except JobError as e:
            raise JobError(f"{source}:{n}: {e}") from None
        seen.add(name)
        out.append(Job(name, variant, card, bars, recipe, mutes, n, args))
    return out


def load_jobs(paths, genre):
    out = []
    for path in paths:
        with open(path) as fh:
            out += parse_jobs(fh.read(), genre, source=path)
    names = [j.name for j in out]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        raise JobError("names repeated across job files: " + ", ".join(dupes))
    return out


@dataclass(frozen=True)
class Capture:
    """One delugemu capture: the longest Job of its group, serving every Job of the group (a shorter capture is a
    bit-exact prefix of a longer one, for the same variant, card, Recipe and mutes)."""

    job: Job
    serves: tuple  # the names of the Jobs it serves, its own first

    @property
    def name(self):
        return self.job.name

    @property
    def bars(self):
        return self.job.bars

    @property
    def variant(self):
        return self.job.variant

    @property
    def recipe(self):
        return self.job.recipe


def merge(job_list):
    """The Captures for these Jobs, in the order of each group's first Job."""
    groups = {}
    for j in job_list:
        groups.setdefault(j.group(), []).append(j)
    out = []
    for members in groups.values():
        longest = max(members, key=lambda j: j.bars)  # max keeps the first of equals
        out.append(
            Capture(
                longest, (longest.name, *(j.name for j in members if j is not longest))
            )
        )
    return out
