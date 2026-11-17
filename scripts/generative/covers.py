"""Which probe scripts a change needs, from their "# covers:" lines (#314).

    python3 scripts/generative/covers.py [--base BRANCH] [--full] [--hardware]
    python3 scripts/generative/covers.py --check

A probe script names the paths it covers in "# covers: PATH ..." lines (repo-relative, more than one line allowed). A
PATH is a file, a folder (every file under it) or a glob (fnmatch: `*` also crosses `/`). A script runs for a change
when any changed file:
  - is one of its covers paths, or under one;
  - is the script itself or a file it includes (perform_open.txt and the like, followed through their includes);
  - is in EVERY_SCRIPT (the probe, the emulator runner, the pinned delugemu, the firmware's test port): those change
    what every script does.
A script with no "# covers:" line runs whenever any script runs (fail safe), and test_port_smoke.txt always does.

check_all.py picks its delugemu scripts with it, unless --full or --scripts. The CLI prints the scripts a hardware
session or a delugemu run needs for the changes since the merge base with origin/BASE (pr_base.py, as check_all.py
finds it), one repo-relative path a line, test_port_smoke.txt first: --hardware lists the scripts a Deluge can run
(scripts/generative/*.txt with a start_state, but not those whose "# needs:" line names a delugemu card, gen_emu.py or
someone listening); without it, the scripts check_all.py runs in delugemu. --full lists every one. --check fails
naming each covers path that matches no file (check_all.py runs it every time; so do the unit tests).
"""

import argparse
import fnmatch
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

COVERS = re.compile(r"^#\s*covers:(.*)$", re.MULTILINE)
INCLUDE = re.compile(r"^\s*include\s+(\S.*?)\s*(?:#.*)?$", re.MULTILINE)
START_STATE = re.compile(r"^start_state\b", re.MULTILINE)
SMOKE = "scripts/generative/test_port_smoke.txt"
# A change to one of these runs every script: they are how every script runs.
EVERY_SCRIPT = (
    "scripts/generative/deluge_probe.py",
    "scripts/generative/test_port_emu.py",
    "scripts/generative/delugemu.pin",
    "src/deluge/io/midi/test_port.cpp",
    "src/deluge/io/midi/test_port.h",
)
# The firmware's inputs: a change under one of these builds the firmware and runs delugemu.
FIRMWARE = ("src/", "CMakeLists.txt", "cmake/")
# "# needs:" text a hardware session can't give: a delugemu card overlay, the leak check's runner, a listener.
NOT_ON_HARDWARE = re.compile(r"^# needs: (card |gen_emu\.py|.*listening)", re.MULTILINE)


def git(*args, repo=REPO):
    return subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True, check=False
    ).stdout


def changed_paths(merge_base, repo=REPO):
    """Every path changed since merge_base, deleted and untracked ones included, repo-relative."""
    names = git("diff", "--name-only", merge_base, repo=repo).split("\n")
    names += git("ls-files", "--others", "--exclude-standard", repo=repo).split("\n")
    return sorted({n for n in names if n})


def rel(path, repo=REPO):
    return os.path.relpath(os.path.abspath(path), repo).replace(os.sep, "/")


def covers(text):
    """The paths of a script's "# covers:" lines, or None if it has none."""
    lines = COVERS.findall(text)
    if not lines:
        return None
    return [p for line in lines for p in line.split()]


def matches(pattern, path):
    """Whether the repo-relative `path` is the covers path `pattern`, under it (a folder) or matched by it (a glob)."""
    folder = pattern.rstrip("/")
    return (
        path == folder
        or path.startswith(folder + "/")
        or fnmatch.fnmatchcase(path, pattern)
    )


def own_files(path, repo=REPO, depth=0):
    """The script at `path` and every file it includes, followed through their includes, repo-relative."""
    if depth > 8:
        raise ValueError(f"{path}: includes nested too deep")
    found = {rel(path, repo)}
    with open(path) as fh:
        text = fh.read()
    for name in INCLUDE.findall(text):
        included = os.path.join(os.path.dirname(path), name)
        if os.path.isfile(included):
            found |= own_files(included, repo, depth + 1)
    return found


def picked_by(path, changed, repo=REPO):
    """Whether the change (repo-relative paths) needs the script at `path` by its covers paths, its own files or
    EVERY_SCRIPT. A script with no covers line is picked only by its own files and EVERY_SCRIPT here: it runs anyway
    once anything does (pick())."""
    changed = set(changed)
    if changed & set(EVERY_SCRIPT) or changed & own_files(path, repo):
        return True
    with open(path) as fh:
        paths = covers(fh.read()) or []
    return any(matches(p, f) for p in paths for f in changed)


def firmware_changed(changed):
    return any(f == d or f.startswith(d) for f in changed for d in FIRMWARE)


def pick(scripts, changed, repo=REPO):
    """The scripts (paths) the change needs, in their order: none if the firmware didn't change and no script is
    picked by its paths; else those picked and every one with no covers line."""
    by_change = [s for s in scripts if picked_by(s, changed, repo)]
    if not by_change and not firmware_changed(changed):
        return []
    out = []
    for s in scripts:
        with open(s) as fh:
            if s in by_change or covers(fh.read()) is None:
                out.append(s)
    return out


def missing(scripts, repo=REPO):
    """(script name, covers path) for each covers path that matches no file in the repo: a renamed or deleted file
    leaves a script that never runs for the code that replaced it."""
    out = []
    for s in scripts:
        with open(s) as fh:
            paths = covers(fh.read()) or []
        for p in paths:
            full = os.path.join(repo, p)
            if os.path.exists(full) or glob.glob(full, recursive=True):
                continue
            out.append((os.path.basename(s), p))
    return out


def check(scripts, repo=REPO):
    """Prints each covers path that matches no file, and how many scripts have covers lines. Returns 1 if any."""
    gone = missing(scripts, repo)
    for name, path in gone:
        print(f"{name}: {path} matches no file: update the covers line")
    with_lines = 0
    for s in scripts:
        with open(s) as fh:
            with_lines += covers(fh.read()) is not None
    print(
        f"{with_lines} of {len(scripts)} scripts have covers lines, {len(gone)} paths missing"
    )
    return 1 if gone else 0


def all_scripts(here=HERE):
    """Every scripts/generative/*.txt that is a check of its own (has a start_state line; not an include)."""
    out = []
    for path in sorted(glob.glob(os.path.join(here, "*.txt"))):
        with open(path) as fh:
            if START_STATE.search(fh.read()):
                out.append(path)
    return out


def hardware_scripts(here=HERE):
    """The scripts a Deluge session can run: see the module docstring."""
    out = []
    for path in all_scripts(here):
        with open(path) as fh:
            if not NOT_ON_HARDWARE.search(fh.read()):
                out.append(path)
    return out


def main(argv=None):
    import check_all
    import pr_base

    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--base", help="as check_all.py's --base")
    p.add_argument("--full", action="store_true", help="every script, whatever changed")
    p.add_argument(
        "--hardware", action="store_true", help="the scripts a Deluge session can run"
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="fail on a covers path that matches no file",
    )
    args = p.parse_args(argv)
    if args.check:
        return check(all_scripts())
    smoke = os.path.join(REPO, SMOKE)
    if args.hardware:
        candidates = [s for s in hardware_scripts() if s != smoke]
    else:
        candidates = [s for s in check_all.probe_scripts()[0] if s != smoke]
    if args.full:
        scripts = candidates
    else:
        base = pr_base.merge_base(args.base, REPO)
        print(f"covers: {base.line()}", file=sys.stderr)
        if base.error or not base.merge_base:
            print("covers: no merge base, so every script", file=sys.stderr)
            scripts = candidates
        else:
            scripts = pick(candidates, changed_paths(base.merge_base))
    for s in [smoke, *scripts]:
        print(rel(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
