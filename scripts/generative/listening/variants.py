"""Table variants: make their diffs, apply and revert them in a worktree, and prove them in the harness.

A variant is a diff against the session's base commit, kept as <session>/diffs/<name>.diff (variants.md quotes it).
A session's content file defines how each diff is made: `Variant(patches, edits, why, parent)`, patch files to `git
apply` and exact (file, old, new) string edits, each of whose `old` must occur once. A variant stacked on another
(its patches include the other's) names it as `parent`; its diff is still against the base commit, so it applies
alone. `make` records the parents in <session>/diffs/parents.json, and in diffs/genres.json the genre of each variant
that names one (`Variant(genre=...)`, #426): prove renders it in that genre, the others in prove's --genre.

Proving renders the variant's harness MIDI and its parent's (the base's when it names none) over N seeds and reports
the seeds where the variant changes nothing: an option that changes nothing there can't be heard there (psytrance
round 1 found three). A render in <scratch>/bin/ built from the same commit and diff is reused, not built again.
"""

import filecmp
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass, field

from . import jobs as _jobs

TREES = ("src", "pack")  # what a variant may change; revert restores them


@dataclass
class Variant:
    patches: list = field(default_factory=list)
    edits: list = field(default_factory=list)  # (path relative to the repo, old, new)
    why: str = ""
    # the variant this one is stacked on: prove compares against it (#422)
    parent: str = "base"
    # the genre prove renders it in (#426); None: prove's --genre
    genre: "str | None" = None


def git(worktree, *args, **kw):
    return subprocess.run(["git", "-C", worktree, *args], check=True, **kw)


def trees(worktree):
    return [t for t in TREES if os.path.isdir(os.path.join(worktree, t))]


def uncommitted(worktree):
    """`git status --porcelain` of src/ and pack/: what revert would throw away."""
    return git(
        worktree,
        "status",
        "--porcelain",
        "--",
        *trees(worktree),
        capture_output=True,
        text=True,
    ).stdout


def require_clean(worktree):
    """Raises ValueError if `worktree` has uncommitted work under src/ or pack/: revert would delete it."""
    dirty = uncommitted(worktree)
    if dirty:
        raise ValueError(
            f"{worktree} has uncommitted work under src/ or pack/, which reverting a variant would delete; commit it, "
            "or leave --worktree out to use a scratch worktree:\n" + dirty.rstrip()
        )


def revert(worktree):
    """Restores src/ and pack/ to the worktree's HEAD, new files included. Only for a worktree this toolkit owns (a
    scratch one), or one require_clean passed before its variant went in."""
    git(worktree, "checkout", "-q", "--", *trees(worktree))
    git(worktree, "clean", "-fdq", "--", *trees(worktree))


def scratch_worktree(repo, path, rev=None):
    """A detached worktree of `repo` at `path` (in the scratch folder, so the toolkit owns it), made if missing, its
    src/ and pack/ reverted and moved to `rev` (default: `repo`'s HEAD now), so it builds what the caller stamps."""
    head = (
        rev
        or git(repo, "rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
    )
    if not os.path.exists(path):
        git(repo, "worktree", "add", "--detach", "-q", path, head)
        return path
    revert(path)
    git(path, "checkout", "--detach", "-q", head)
    return path


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


def parents_path(session):
    return os.path.join(session, "diffs", "parents.json")


def parents(session):
    """{variant: its parent} for the variants `make` wrote that name one other than the base."""
    try:
        with open(parents_path(session)) as fh:
            return json.load(fh)
    except FileNotFoundError:
        return {}


def genres_path(session):
    return os.path.join(session, "diffs", "genres.json")


def genres(session):
    """{variant: its genre} for the variants `make` wrote that name one (#426)."""
    try:
        with open(genres_path(session)) as fh:
            return json.load(fh)
    except FileNotFoundError:
        return {}


def genre_of(names, named, default):
    """{name: the genre prove renders it in}: its own (`named`, from genres()), else `default` (--genre). Raises
    ValueError naming a variant with neither."""
    out = {n: named.get(n) or default for n in names if n != "base"}
    missing = [n for n, g in out.items() if not g]
    if missing:
        raise ValueError(
            ", ".join(f"{n}: no genre" for n in missing)
            + ": give its Variant a genre, or prove --genre"
        )
    return out


def make(worktree, session, variants):
    """Writes each variant's diff to <session>/diffs/, and their parents to diffs/parents.json. Raises ValueError,
    changing nothing, if `worktree` has uncommitted work under src/ or pack/ or a variant names an unknown parent."""
    require_clean(worktree)
    known = parents(session)
    for name, v in variants.items():
        if (
            v.parent != "base"
            and v.parent not in variants
            and not os.path.exists(diff_path(session, v.parent))
        ):
            raise ValueError(f"{name}: its parent {v.parent} is no variant")
        if v.parent == name:
            raise ValueError(f"{name}: its own parent")
        if v.genre is not None and v.genre not in _jobs._names(
            "genres", _jobs.gen_emu.genre_names
        ):
            raise ValueError(f"{name}: unknown genre {v.genre}")
    os.makedirs(os.path.join(session, "diffs"), exist_ok=True)
    for name, v in variants.items():
        known.pop(name, None)
        if v.parent != "base":
            known[name] = v.parent
    with open(parents_path(session), "w") as fh:
        json.dump(known, fh, indent=1, sort_keys=True)
    named = genres(session)
    for name, v in variants.items():
        named.pop(name, None)
        if v.genre:
            named[name] = v.genre
    with open(genres_path(session), "w") as fh:
        json.dump(named, fh, indent=1, sort_keys=True)
    out = {}
    try:
        for name, v in variants.items():
            revert(worktree)
            build_tree(worktree, v)
            d = git(
                worktree, "diff", "--", *trees(worktree), capture_output=True, text=True
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
    """({name: bin_dir/<name>}, {name: its build log}) for `base` and each variant: generator_render built with it
    applied. A render whose bin_dir/<name>.json matches its stamp now (render_stamp: the commit and diff) is reused,
    so a second prove (another genre, #426) builds only what changed. A variant that doesn't apply or build is logged
    to bin_dir/<name>.log and the rest go on; the base's failure raises. Reverts after. Raises ValueError, changing
    nothing, if `worktree` has uncommitted work under src/ or pack/."""
    require_clean(worktree)
    out, failed = {}, {}
    try:
        for name in ["base", *[n for n in names if n != "base"]]:
            dest = os.path.join(bin_dir, name)
            if is_built(dest, render_stamp(worktree, session, name)):
                out[name] = dest
                continue
            revert(worktree)
            try:
                apply(worktree, session, name)
                out[name] = build_render(worktree, dest)
            except subprocess.CalledProcessError as e:
                if name == "base":
                    raise
                os.makedirs(bin_dir, exist_ok=True)
                failed[name] = os.path.join(bin_dir, name + ".log")
                text = e.stderr or e.output or b""
                with open(failed[name], "wb") as fh:
                    fh.write(
                        f"{' '.join(map(str, e.cmd))}: exit {e.returncode}\n".encode()
                    )
                    fh.write(text if isinstance(text, bytes) else text.encode())
                continue
            with open(out[name] + ".json", "w") as fh:
                json.dump(render_stamp(worktree, session, name), fh)
    finally:
        revert(worktree)
    return out, failed


def is_built(render, stamp):
    """Whether `render` exists and its sidecar (render.json) says it was built as `stamp` says."""
    try:
        with open(render + ".json") as fh:
            return os.path.exists(render) and json.load(fh) == stamp
    except (OSError, ValueError):
        return False


def render_stamp(worktree, session, name):
    """What bin/<name> was built from: the commit and the variant's diff. clips.py refuses a render whose stamp
    differs from the repo's now: a diff remade or a new commit since prove."""
    diff = "-"
    if name != "base":
        with open(diff_path(session, name), "rb") as fh:
            diff = hashlib.sha256(fh.read()).hexdigest()
    head = git(
        worktree, "rev-parse", "HEAD", capture_output=True, text=True
    ).stdout.strip()
    return {"variant": name, "diff": diff, "head": head}


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


def unchanged_seeds(renders, name, genre, seeds, bars, extra, scratch, against="base"):
    """The seeds (entropy = seed) where variant `name`'s harness MIDI equals variant `against`'s."""
    folder = os.path.join(scratch, "prove")
    os.makedirs(folder, exist_ok=True)
    same = []
    for seed in seeds:
        a = render_midi(
            renders[against],
            genre,
            seed,
            bars,
            extra,
            os.path.join(folder, f"{against}-{seed}"),
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


def render_names(names, parent_of):
    """The renders proving `names` needs: the base, each name, and each name's parent, once each, in that order."""
    out = ["base"]
    for name in names:
        for n in (name, parent_of.get(name, "base")):
            if n not in out:
                out.append(n)
    return out


def compare(renders, names, parent_of, genre, seeds, bars, extra, scratch, failed=None):
    """Lines: per variant, the seeds where it changes nothing against its parent (`parent_of`, the base by default),
    rendered in its genre: `genre` is one for all, or {variant: genre} (genre_of); a line names it when the variants
    span more than one. `failed`: {variant: its build log or None} for the ones that didn't build: reported, and their
    children not proved."""
    seeds = list(seeds)
    failed = failed or {}
    genre_of_ = genre if isinstance(genre, dict) else dict.fromkeys(names, genre)
    several = len({genre_of_[n] for n in names if n != "base"}) > 1
    lines = []
    for name in names:
        if name == "base":
            continue
        parent = parent_of.get(name, "base")
        if name in failed:
            lines.append(f"{name}: doesn't build ({failed[name]})")
            continue
        if parent in failed:
            lines.append(f"{name}: not proved: its parent {parent} doesn't build")
            continue
        g = genre_of_[name]
        same = unchanged_seeds(
            renders, name, g, seeds, bars, extra, scratch, against=parent
        )
        lines.append(
            f"{name}: changes the notes at {len(seeds) - len(same)}/{len(seeds)} seeds"
            + (f" against {parent}" if parent != "base" else "")
            + (f" ({g})" if several else "")
            + (f"; nothing at seeds {', '.join(map(str, same))}" if same else "")
        )
    return lines


def prove(worktree, session, names, genre, seeds, bars, extra, scratch):
    """(lines, failed): per variant, the seeds where it changes nothing against its parent (diffs/parents.json), in
    its genre (diffs/genres.json, else `genre`); failed: {variant: build log} for the ones that didn't build. Keeps
    each variant's generator_render in <scratch>/bin/<name> for its offline clips, and reuses one still current."""
    parent_of = parents(session)
    genre_by_name = genre_of(names, genres(session), genre)
    renders, failed = build_renders(
        worktree, session, render_names(names, parent_of), os.path.join(scratch, "bin")
    )
    lines = compare(
        renders, names, parent_of, genre_by_name, seeds, bars, extra, scratch, failed
    )
    return lines, failed
