"""python3 scripts/generative/listen.py COMMAND: the listening-session toolkit's commands.

  card     make <scratch>/card_<name>.img: the stock card with pack/ copied over it (--no-pack: stock only; --variant:
           that variant's pack/, for a pack-edit variant), then each --overlay folder, and --palette files as palette
           overrides
  variants make the session's diffs from its content file's VARIANTS (make), or prove them in the harness (prove)
  run      check every job's Recipe, then capture in delugemu on N runners (--dry-run: print the build plan only)
  clips    rough-render jobs offline with each variant's generator_render
  page     cut the clips and write index.html and variants.md into the session folder

A session has two folders: the session folder (<notes dir>/tuning/<genre>/round-<n>/: the page, audio/, midi/,
variants.md, diffs/ and tools/ with the content and job files) and a scratch folder (raw WAVs, card images, worktrees,
build dirs; delete it once the page passes its browser test).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

from . import clips, jobs, page, runner, variants

STOCK = os.path.expanduser("~/source/delugemu-sd/deluge_sd.img")
PALETTE_MAX_BYTES = 8192  # spec 10.1: the firmware ignores a larger override file


def oled_names(repo=None):
    """Every genre's OLED name, the stem of its palette override file (GENERATIVE/PALETTES/<GENRE NAME>.XML)."""
    folder = os.path.join(
        repo or runner.REPO, "src", "deluge", "generative", "core", "genres"
    )
    names = set()
    for name in os.listdir(folder):
        if name.endswith(".cpp"):
            with open(os.path.join(folder, name)) as fh:
                names |= set(re.findall(r'oledName\s*=\s*"([^"]+)"', fh.read()))
    return names


def palette_dest(path, names):
    """Where the palette override file `path` goes on the card. Its file name must be a genre's OLED name and .XML, and
    it must fit the firmware's limit: else the firmware ignores it and the built-in palette plays, unnoticed."""
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    if ext != ".XML" or stem not in names:
        raise ValueError(
            f"{path}: a palette override is named <GENRE NAME>.XML, one of: "
            + ", ".join(sorted(n + ".XML" for n in names))
        )
    if not os.path.isfile(path):
        raise ValueError(f"{path}: no such file")
    if os.path.getsize(path) > PALETTE_MAX_BYTES:
        raise ValueError(
            f"{path}: over {PALETTE_MAX_BYTES} bytes; the firmware would ignore it"
        )
    return "GENERATIVE/PALETTES/" + base


def cmd_card(a):
    from fat_image import FatImage, copy_tree

    names = oled_names() if a.palette else set()
    dests = [(palette_dest(p, names), p) for p in a.palette]
    for folder in a.overlay:
        if not os.path.isdir(folder):
            raise ValueError(f"--overlay {folder}: no such folder")
    os.makedirs(a.scratch, exist_ok=True)
    wt = None
    if a.variant and not a.no_pack:
        # The variant's pack/: its diff applied in a scratch worktree of the repo's HEAD, reverted after.
        if a.variant != "base" and not os.path.exists(
            variants.diff_path(a.session, a.variant)
        ):
            raise ValueError(
                f"--variant {a.variant}: no {variants.diff_path(a.session, a.variant)}"
            )
        wt = variants.scratch_worktree(
            runner.REPO, os.path.join(a.scratch, "wt", "card")
        )
    path = os.path.join(a.scratch, f"card_{a.name}.img")
    if sys.platform == "darwin":
        subprocess.run(
            ["cp", "-c", a.stock, path], check=True
        )  # an APFS clone: instant
    else:
        shutil.copyfile(a.stock, path)
    try:
        if wt:
            variants.apply(wt, a.session, a.variant)
        with FatImage(path) as card:
            if not a.no_pack:
                copy_tree(card, os.path.join(wt or runner.REPO, "pack"))
            for folder in a.overlay:
                copy_tree(card, folder)
            for dest, src in dests:
                with open(src, "rb") as fh:
                    card.write_file(dest, fh.read())
    finally:
        if wt:
            variants.revert(wt)
    print(path)
    return 0


def cmd_variants(a):
    # A scratch worktree by default: make and prove revert src/ and pack/ around each variant.
    if a.worktree:
        wt = a.worktree
    else:
        wt = variants.scratch_worktree(
            runner.REPO, os.path.join(a.scratch, "wt", a.action)
        )
    if a.action == "make":
        for name, n in variants.make(
            wt, a.session, page_module_variants(a.content)
        ).items():
            print(f"{name}: {n} lines")
        return 0
    extra = [w for arg in a.extra for w in arg.split()]
    for line in variants.prove(
        wt, a.session, a.names, a.genre, range(1, a.seeds + 1), a.bars, extra, a.scratch
    ):
        print(line)
    return 0


def page_module_variants(path):
    import importlib.util

    spec = importlib.util.spec_from_file_location("listening_variants", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.VARIANTS


def cmd_run(a):
    r = runner.Run(
        a.genre,
        a.session,
        a.scratch,
        jobs.load_jobs(a.jobs, a.genre),
        a.runners,
        a.cap,
        a.render,
    )
    for warning in runner.delugemu_warnings():
        print("WARNING " + warning)
    r.check()
    left = len(r.pending())
    if left < len(r.captures):
        print(
            f"{len(r.captures) - left} of {len(r.captures)} captures are current or failed already"
        )
    print(r.plan().text())
    if a.dry_run:
        return 0
    failed = r.run()
    for c in failed:
        why = (
            f"marker {r.failed_marker(c)}; delete it to try again"
            if r.failed(c)
            else "not captured"
        )
        print(f"failed: {c.name} ({why}; log {os.path.join(r.raw, c.name + '.log')})")
    return 1 if failed else 0


def cmd_clips(a):
    names = clips.render_all(
        jobs.load_jobs(a.jobs, a.genre), a.scratch, a.session, a.render
    )
    print(f"rendered {len(names)}: {', '.join(names)}")
    return 0


def cmd_page(a):
    session = page.load(a.content)
    for w in page.build(session, a.scratch, a.session, recut=a.recut):
        print("WARNING " + w)
    print(os.path.join(a.session, "index.html"))
    return 0


def parser():
    p = argparse.ArgumentParser(
        prog="listen.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = p.add_subparsers(dest="command", required=True)

    c = sub.add_parser("card", help="make a card image in the scratch folder")
    c.add_argument("--scratch", required=True)
    c.add_argument("--stock", default=STOCK)
    c.add_argument(
        "--no-pack",
        action="store_true",
        help="the stock card only: a pack preset then plays its factory fallback",
    )
    c.add_argument(
        "--palette",
        action="append",
        default=[],
        help="a palette override file named <GENRE NAME>.XML (spec 10.1), written to GENERATIVE/PALETTES/; repeatable",
    )
    c.add_argument(
        "--variant",
        help="copy this variant's pack/ (its diff from --session applied to the repo's HEAD) instead of the repo's",
    )
    c.add_argument("--session", help="--variant: the session folder (its diffs/)")
    c.add_argument(
        "--overlay",
        action="append",
        default=[],
        help="a card-shaped folder copied over the pack, as the card root; repeatable, in order",
    )
    c.add_argument("name")

    v = sub.add_parser("variants", help="make or prove the table variants")
    v.add_argument("action", choices=("make", "prove"))
    v.add_argument("--session", required=True, help="diffs go to <session>/diffs/")
    v.add_argument(
        "--content",
        help="make: the Python file defining VARIANTS (name: variants.Variant)",
    )
    v.add_argument(
        "--worktree",
        help="default <scratch>/wt/<action>, a worktree of the repo's HEAD; one you give must be clean under src/ and pack/",
    )
    v.add_argument(
        "--scratch",
        help="its worktree (wt/make, wt/prove); prove keeps bin/<name> and the MIDI there",
    )
    v.add_argument("--genre")
    v.add_argument(
        "--seeds", type=int, default=20, help="prove: seeds 1..N, entropy = seed"
    )
    v.add_argument("--bars", type=int, default=320)
    v.add_argument(
        "--extra",
        action="append",
        default=[],
        help="prove: more generator_render flags",
    )
    v.add_argument("names", nargs="*")

    r = sub.add_parser("run", help="capture the jobs in delugemu")
    r.add_argument("--genre", required=True)
    r.add_argument("--session", required=True, help="the session folder (its diffs/)")
    r.add_argument("--scratch", required=True)
    r.add_argument("--runners", type=int, default=2)
    r.add_argument(
        "--cap",
        type=int,
        default=6,
        help="wait while the host runs this many emulators (default 6)",
    )
    r.add_argument(
        "--render",
        help="the generator_render the Recipe check uses (default: the newest in build/tests)",
    )
    r.add_argument(
        "--dry-run",
        action="store_true",
        help="check the Recipes and print the build plan only",
    )
    r.add_argument("jobs", nargs="+")

    o = sub.add_parser("clips", help="rough-render jobs offline")
    o.add_argument("--genre", required=True)
    o.add_argument("--scratch", required=True)
    o.add_argument(
        "--session",
        required=True,
        help="the session folder: its diffs/ check each render is current",
    )
    o.add_argument(
        "--render",
        help="the base generator_render for the Recipe check (default <scratch>/bin/base)",
    )
    o.add_argument("jobs", nargs="+")

    g = sub.add_parser("page", help="cut the clips and write the page")
    g.add_argument("--content", required=True)
    g.add_argument("--session", required=True)
    g.add_argument("--scratch", required=True)
    g.add_argument(
        "--recut", action="store_true", help="cut clips again even if their m4a exists"
    )
    return p


def main(argv=None):
    a = parser().parse_args(argv)
    if a.command == "variants":
        need = {"make": ("content", "scratch"), "prove": ("scratch", "genre")}[a.action]
        missing = [f"--{n}" for n in need if not getattr(a, n)]
        if missing:
            sys.exit(f"variants {a.action} needs {', '.join(missing)}")
    if a.command == "card" and a.variant and not a.session:
        sys.exit("card --variant needs --session")
    for name in ("scratch", "session", "content", "worktree", "render"):
        if getattr(a, name, None):
            setattr(a, name, os.path.abspath(os.path.expanduser(getattr(a, name))))
    if a.command == "card":
        a.palette = [os.path.abspath(os.path.expanduser(p)) for p in a.palette]
        a.overlay = [os.path.abspath(os.path.expanduser(p)) for p in a.overlay]
    try:
        return {
            "card": cmd_card,
            "variants": cmd_variants,
            "run": cmd_run,
            "clips": cmd_clips,
            "page": cmd_page,
        }[a.command](a)
    except (jobs.JobError, ValueError, RuntimeError) as e:
        # RuntimeError: generator_render refused a Recipe (gen_emu.render_piece)
        sys.exit(str(e))
    except subprocess.CalledProcessError as e:
        out = e.stderr or e.output or b""
        out = out.decode(errors="replace") if isinstance(out, bytes) else out
        sys.exit(
            f"{' '.join(map(str, e.cmd))}: exit {e.returncode}\n{out.strip()[-600:]}"
        )
