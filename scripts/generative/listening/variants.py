"""Table variants: make their diffs, apply and revert them in a worktree, and prove them in the harness.

A variant is a diff against the session's base commit, kept as <session>/diffs/<name>.diff (variants.md quotes it).
A session's content file defines how each diff is made: `Variant(patches, edits, why)`, patch files to `git apply`
and exact (file, old, new) string edits, each of whose `old` must occur once.

Proving renders the variant's harness MIDI and the base's over N seeds and reports the seeds where the variant
changes nothing: an option that changes nothing there can't be heard there (psytrance round 1 found three).
"""

import filecmp
import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass, field

TREES = ("src", "pack")  # what a variant may change; revert restores them


@dataclass
class Variant:
    patches: list = field(default_factory=list)
    edits: list = field(default_factory=list)  # (path relative to the repo, old, new)
    why: str = ""


def git(worktree, *args, **kw):
    return subprocess.run(["git", "-C", worktree, *args], check=True, **kw)


def revert(worktree):
    """Restores src/ and pack/ to the worktree's HEAD, new files included."""
    git(worktree, "checkout", "-q", "--", *TREES)
    git(worktree, "clean", "-fdq", "--", *TREES)


def diff_path(session, name):
    return os.path.join(session, "diffs", name + ".diff")


def apply(worktree, session, name):
    """Applies variant `name`'s diff in `worktree`. `base` applies nothing."""
    if name != "base":
        git(worktree, "apply", "--whitespace=nowarn", diff_path(session, name))


def build_tree(worktree, variant):
    for p in variant.patches:
        git(worktree, "apply", os.path.abspath(p))
    for rel, old, new in variant.edits:
        path = os.path.join(worktree, rel)
        with open(path) as fh:
            text = fh.read()
        if text.count(old) != 1:
            raise ValueError(
                f"{rel}: edit found {text.count(old)} times, not once: {old[:60]!r}"
            )
        with open(path, "w") as fh:
            fh.write(text.replace(old, new))


def make(worktree, session, variants):
    """Writes each variant's diff to <session>/diffs/. `worktree` must be clean under src/ and pack/."""
    os.makedirs(os.path.join(session, "diffs"), exist_ok=True)
    out = {}
    try:
        for name, v in variants.items():
            revert(worktree)
            build_tree(worktree, v)
            d = git(
                worktree, "diff", "--", *TREES, capture_output=True, text=True
            ).stdout
            if not d:
                raise ValueError(f"{name}: an empty diff")
            with open(diff_path(session, name), "w") as fh:
                fh.write(d)
            out[name] = len(d.splitlines())
    finally:
        revert(worktree)
    return out


def build_render(worktree, dest):
    """Builds generator_render in `worktree`'s harness build (only that target) and copies it to `dest`."""
    build = os.path.join(worktree, "build", "tests")
    dbt = os.path.join(worktree, "dbt")
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        subprocess.run(
            [
                dbt,
                "exec",
                f"cmake -S tests -B {shlex.quote(build)} -G 'Ninja Multi-Config'",
            ],
            cwd=worktree,
            check=True,
            capture_output=True,
        )
    subprocess.run(
        [
            dbt,
            "exec",
            f"cmake --build {shlex.quote(build)} --config Debug --target generator_render_cli",
        ],
        cwd=worktree,
        check=True,
        capture_output=True,
    )
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(
        os.path.join(build, "spec", "generator", "Debug", "generator_render"), dest
    )
    return dest


def build_renders(worktree, session, names, bin_dir):
    """bin_dir/<name> for `base` and each variant: generator_render built with it applied. Reverts after."""
    out = {}
    try:
        for name in ["base", *[n for n in names if n != "base"]]:
            revert(worktree)
            apply(worktree, session, name)
            out[name] = build_render(worktree, os.path.join(bin_dir, name))
    finally:
        revert(worktree)
    return out


def render_midi(render, genre, seed, bars, extra, out_prefix):
    subprocess.run(
        [
            render,
            "--genre",
            genre,
            "--seed",
            str(seed),
            "--entropy",
            str(seed),
            "--bars",
            str(bars),
            "--midi-only",
            "--out",
            out_prefix,
            *extra,
        ],
        check=True,
        capture_output=True,
    )
    return out_prefix + ".mid"


def unchanged_seeds(renders, name, genre, seeds, bars, extra, scratch):
    """The seeds (entropy = seed) where variant `name`'s harness MIDI equals the base's."""
    folder = os.path.join(scratch, "prove")
    os.makedirs(folder, exist_ok=True)
    same = []
    for seed in seeds:
        a = render_midi(
            renders["base"],
            genre,
            seed,
            bars,
            extra,
            os.path.join(folder, f"base-{seed}"),
        )
        b = render_midi(
            renders[name],
            genre,
            seed,
            bars,
            extra,
            os.path.join(folder, f"{name}-{seed}"),
        )
        if filecmp.cmp(a, b, shallow=False):
            same.append(seed)
    return same


def prove(worktree, session, names, genre, seeds, bars, extra, scratch):
    """Lines: per variant, the seeds where it changes nothing. Keeps each variant's generator_render in
    <scratch>/bin/<name> for its offline clips."""
    renders = build_renders(worktree, session, names, os.path.join(scratch, "bin"))
    lines = []
    for name in names:
        if name == "base":
            continue
        same = unchanged_seeds(renders, name, genre, seeds, bars, extra, scratch)
        lines.append(
            f"{name}: changes the notes at {len(seeds) - len(same)}/{len(seeds)} seeds"
            + (f"; nothing at seeds {', '.join(map(str, same))}" if same else "")
        )
    return lines
