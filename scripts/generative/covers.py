"""Which probe scripts a change needs, from their "# covers:" lines (#314).

    python3 scripts/generative/covers.py [--base BRANCH] [--full] [--hardware]
    python3 scripts/generative/covers.py --check

A probe script names the paths it covers in "# covers: PATH ..." lines (repo-relative, more than one line allowed). A
PATH is a file, a folder (every file under it) or a glob (fnmatch: `*` also crosses `/`). A script runs for a change
when any changed file:
  - is one of its covers paths, or under one;
  - is the script itself or a file it includes (perform_open.txt and the like, followed through their includes);
  - is one of runner_files(): test_port_emu.py, deluge_probe.py and the scripts/generative/ modules they import, the
    pinned delugemu and the firmware's test port. Those change what every script does.
A script with no "# covers:" line runs whenever any script runs (fail safe), and test_port_smoke.txt always does.
A FIRMWARE path (src/, lib/, the root CMakeLists.txt, the linker script, the toolchain, the dbt build tasks) or what
smoke and the leak check run on (gen_emu.py and its imports) builds the firmware and runs delugemu whatever the
covers lines pick.

check_all.py picks its delugemu scripts with it, unless --full or --scripts. The CLI prints the scripts a hardware
session or a delugemu run needs for the changes since the merge base with origin/BASE (pr_base.py, as check_all.py
finds it), one repo-relative path a line, test_port_smoke.txt first: --hardware lists the scripts a Deluge can run
(scripts/generative/*.txt with a start_state, but not those whose "# needs:" line names a delugemu card, gen_emu.py or
someone listening); without it, the scripts check_all.py runs in delugemu. --full lists every one. --check fails
naming each covers path that matches no file, and each src/ file that sends an event a script expects (or rules out)
that its covers lines miss: a string literal whose first word is the event's kind (check_all.py runs it every time;
so do the unit tests).
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
IMPORT = re.compile(r"^\s*(?:import\s+([\w, ]+)|from\s+(\w+)\s+import)", re.MULTILINE)
# An event a script waits on, rules out or keeps to compare: expect [next] [no_]event TEXT, keep events TEXT; TEXT
# plain or /regex/ (its first word).
EXPECTED = re.compile(
    r"^(?:expect (?:next )?(?:no_)?event|keep events) /?\^?([a-z][a-z0-9_]*)",
    re.MULTILINE,
)
SMOKE = "scripts/generative/test_port_smoke.txt"
# The firmware's inputs: a change to one of these (covers paths: a file, a folder, a glob) builds the firmware and runs
# delugemu, with smoke, the leak check and test_port_smoke.txt. The dbt tasks a build runs are among them.
FIRMWARE = (
    "src/",
    "lib/",
    "CMakeLists.txt",
    "*.ld",
    "toolchain/",
    "scripts/toolchain/",
    "dbt",
    "dbt.py",
    "scripts/tasks/task-build.py",
    "scripts/tasks/task-configure.py",
    "scripts/tasks/util.py",
)
# What every script runs on, besides the scripts/generative/ modules test_port_emu.py and deluge_probe.py import
# (runner_files()): a change to one runs every script.
EVERY_SCRIPT_TOO = (
    "scripts/generative/delugemu.pin",
    "src/deluge/io/midi/test_port.cpp",
    "src/deluge/io/midi/test_port.h",
)
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


def module_files(*roots, here=HERE, repo=REPO):
    """The scripts/generative/ modules `roots` (file names) are and import, followed through their imports,
    repo-relative: from their import lines, so the list can't drift from the code."""
    seen, todo = set(), list(roots)
    while todo:
        name = todo.pop()
        path = os.path.join(here, name)
        if name in seen or not os.path.isfile(path):
            continue
        seen.add(name)
        with open(path) as fh:
            for plain, frm in IMPORT.findall(fh.read()):
                for module in plain.split(",") if plain else [frm]:
                    todo.append(module.strip().split(" ")[0] + ".py")
    return {rel(os.path.join(here, n), repo) for n in seen}


def runner_files(here=HERE, repo=REPO):
    """What every probe script runs on: test_port_emu.py, deluge_probe.py and the modules they import, and
    EVERY_SCRIPT_TOO."""
    return module_files(
        "test_port_emu.py", "deluge_probe.py", here=here, repo=repo
    ) | set(EVERY_SCRIPT_TOO)


def emulator_files(here=HERE, repo=REPO):
    """What smoke and the leak check run on: gen_emu.py and the modules it imports (test_port_emu.py's among them)."""
    return module_files("gen_emu.py", here=here, repo=repo) | runner_files(here, repo)


def script_text(path):
    with open(path) as fh:
        return fh.read()


def covers(text):
    """The paths of a script's "# covers:" lines, or None if it has none."""
    lines = COVERS.findall(text)
    if not lines:
        return None
    return [p for line in lines for p in line.split()]


def script_covers(path):
    """covers() of the script at `path`."""
    return covers(script_text(path))


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
    for name in INCLUDE.findall(script_text(path)):
        included = os.path.join(os.path.dirname(path), name)
        if os.path.isfile(included):
            found |= own_files(included, repo, depth + 1)
    return found


def picked_by(path, changed, repo=REPO):
    """Whether the change (repo-relative paths) needs the script at `path` by its covers paths, its own files or
    runner_files(). A script with no covers line is picked only by its own files and runner_files() here: it runs
    anyway once anything does (pick())."""
    changed = set(changed)
    here = os.path.join(repo, "scripts", "generative")
    if changed & runner_files(here, repo) or changed & own_files(path, repo):
        return True
    return any(matches(p, f) for p in script_covers(path) or [] for f in changed)


def firmware_changed(changed, repo=REPO):
    """Whether the change needs the firmware built and delugemu run whatever scripts it picks: a FIRMWARE path, or
    what smoke and the leak check run on."""
    here = os.path.join(repo, "scripts", "generative")
    emulator = emulator_files(here, repo)
    return any(f in emulator or any(matches(d, f) for d in FIRMWARE) for f in changed)


def pick(scripts, changed, repo=REPO):
    """The scripts (paths) the change needs, in their order: none if the firmware didn't change and no script is
    picked by its paths; else those picked and every one with no covers line."""
    by_change = [s for s in scripts if picked_by(s, changed, repo)]
    if not by_change and not firmware_changed(changed, repo):
        return []
    return [s for s in scripts if s in by_change or script_covers(s) is None]


def missing(scripts, repo=REPO):
    """(script name, covers path) for each covers path that matches no file in the repo: a renamed or deleted file
    leaves a script that never runs for the code that replaced it."""
    out = []
    for s in scripts:
        for p in script_covers(s) or []:
            full = os.path.join(repo, p)
            if os.path.exists(full) or glob.glob(full, recursive=True):
                continue
            out.append((os.path.basename(s), p))
    return out


def expected_events(path):
    """The kinds of event (first word) the script at `path` and its includes expect or rule out."""
    kinds = set()
    for name in own_files(path, os.path.dirname(path)):
        kinds |= set(
            EXPECTED.findall(script_text(os.path.join(os.path.dirname(path), name)))
        )
    return kinds


def emitters(repo=REPO):
    """{event kind: the files under src/ that have it as a string literal's first word}: where the firmware sends it
    (TEST_EVENT("gen_phrase bar=%ld"), out.add("gen_sound_swap part=")). Read with git grep, tracked files only."""
    found = {}
    out = git(
        "grep", "-n", "-E", '"[a-z][a-z0-9]*_[a-z0-9_]+[ "%]', "--", "src", repo=repo
    )
    for line in out.splitlines():
        path = line.split(":", 1)[0]
        for kind in re.findall(r'"([a-z][a-z0-9]*_[a-z0-9_]+)[ "%]', line):
            found.setdefault(kind, set()).add(path)
    return found


def uncovered(scripts, repo=REPO):
    """(script name, event kind, file) for each file that sends an event a script expects but that neither its covers
    lines nor runner_files() name. A script with no covers line runs with every firmware change: none for it."""
    sent = emitters(repo)
    here = os.path.join(repo, "scripts", "generative")
    every = runner_files(here, repo)
    out = []
    for s in scripts:
        paths = script_covers(s)
        if paths is None:
            continue
        for kind in sorted(expected_events(s)):
            for f in sorted(sent.get(kind, ())):
                if f not in every and not any(matches(p, f) for p in paths):
                    out.append((os.path.basename(s), kind, f))
    return out


def check(scripts, repo=REPO):
    """Prints each covers path that matches no file and each file that sends an event a script expects but its
    covers lines miss, and how many scripts have covers lines. Returns 1 if any."""
    gone = missing(scripts, repo)
    for name, path in gone:
        print(f"{name}: {path} matches no file: update the covers line")
    unseen = uncovered(scripts, repo)
    for name, kind, path in unseen:
        print(f"{name}: expects {kind}, which {path} sends: add it to the covers lines")
    with_lines = sum(script_covers(s) is not None for s in scripts)
    print(
        f"{with_lines} of {len(scripts)} scripts have covers lines, {len(gone)} paths missing, "
        f"{len(unseen)} event senders uncovered"
    )
    return 1 if gone or unseen else 0


def all_scripts(here=HERE):
    """Every scripts/generative/*.txt that is a check of its own (has a start_state line; not an include)."""
    return [
        p
        for p in sorted(glob.glob(os.path.join(here, "*.txt")))
        if START_STATE.search(script_text(p))
    ]


def probe_scripts(here=HERE):
    """(scripts, skipped): test_port_smoke.txt, then every perform_*.txt that is a check on its own (not an include)
    and that delugemu can run (deluge_probe.script_needs(): no "# needs:" line but "# needs: card DIR"); and the names
    of the checks it can't run."""
    import deluge_probe

    scripts, skipped = [os.path.join(here, os.path.basename(SMOKE))], []
    for path in sorted(glob.glob(os.path.join(here, "perform_*.txt"))):
        text = script_text(path)
        if not START_STATE.search(text):
            continue  # an include (perform_open.txt and the like)
        if deluge_probe.script_needs(path, text).delugemu:
            scripts.append(path)
        else:
            skipped.append(os.path.basename(path))
    return scripts, skipped


def hardware_scripts(here=HERE):
    """The scripts a Deluge session can run: see the module docstring."""
    return [p for p in all_scripts(here) if not NOT_ON_HARDWARE.search(script_text(p))]


def main(argv=None):
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
        candidates = [s for s in probe_scripts()[0] if s != smoke]
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
