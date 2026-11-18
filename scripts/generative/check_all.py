"""Every local check for a Generative change in one run, with one results table at the end.

    python3 scripts/generative/check_all.py [--quick] [--full] [--base BRANCH] [--jobs N] [--scripts SCRIPT...]

Run it from the host, in the checkout to check: it calls ./dbt itself. It runs what the change needs: the files changed
since the merge base with origin/BASE (committed, staged, unstaged, untracked or deleted) pick the steps below and the
probe scripts (#314). --full runs every step and every probe script whatever changed: the final run before a merge
into generative or a milestone branch is --full. With no merge base, every step runs too. In order:
  0. the portable Generator code compiled on ARM, syntax only, in about a second (arm_types.py): an int32_t/int mix
     that the host harness accepts fails here before any build
  1. every probe script's "# covers:" paths match a file, and name every src/ file that sends an event it expects
     (covers.py --check)
  2. harness specs, sanitized: ./dbt test --generator --sanitize, ctest -j. The plain run (./dbt test --generator) is
     for iterating: it caught nothing the sanitized one missed. Then the plain harness build without its run
     (./dbt test --generator --no-run) for generator_render, and on it the pitched parts' palette presets against the
     factory card and pack/ (osc_octaves.py): none may sit an octave off on every oscillator (spec §10.6); and every
     pack Riser a genre's Riser lane reaches against its shortest Build (riser_fit.py, spec §10.5)
  3. only for a change under scripts/, pack/ or src/deluge/generative/: the Python unit tests of these scripts and of the dbt tasks
     (scripts/generative/test_*.py, scripts/tasks/test_*.py), each file in its own process, side by side
     (unit_tests.py)
  4. the goldens' version rule against the merge base with origin/BASE (goldens.py check: a changed golden bumps its
     genre's version)
  5. hooks on every file changed since the merge base with origin/BASE that still exists:
     ./dbt exec "pre-commit run --files ..."
  6. ./dbt build release, then on it the step section check (step_section.py: no deluge::generative call target
     outside .text.generative_step); and, on every run, sizeof(Generator) on ARM against H18 and spec §20
     (generator_size.py)
  7. delugemu (skipped by --quick), after the test-port build in build-testport/, --jobs emulators at a time, the
     longest first (by its last passing run here, build/check_all/costs.json, else LISTED_COSTS; one with neither
     right after the longest listed one): gen_emu.py smoke --no-leak (the functional smoke checks); gen_emu.py leak (the memory
     check, one warm-up and one checked cycle); and each probe
     script in delugemu on its own boot (test_port_emu.py, on the test-port build): test_port_smoke.txt (with the
     test port's error-reply checks) and the perform_*.txt checks the change picks by their "# covers:" lines
     (covers.py: a script whose paths, own file or includes changed, and every one with no covers line), or the
     --scripts given. --full picks every perform_*.txt but includes and those with a "# needs:" line (a song, a
     Deluge, another runner). A script whose needs line is "# needs: card DIR" runs on its own card: the factory
     card with the overlay folder DIR (relative to scripts/generative/) put on it (test_port_emu.py reads the line).
The build release and step section of step 6, and step 7, run only for a change to a firmware input (covers.FIRMWARE:
src/, lib/, the root CMakeLists.txt, the linker script, the toolchain and its CMake file (`scripts/cmake/`), the dbt
build tasks), to what the emulator checks run on (gen_emu.py, test_port_emu.py, deluge_probe.py and their imports, the
delugemu pin) or decide what they run (check_all.py, covers.py), to a probe script or an include, or to a file a covers
line names; --scripts runs them whatever changed. A src/ file no covers line names runs smoke, the leak check and
test_port_smoke.txt only (and every script with no covers line): --full, the final run before a merge, is the backstop.
The table has a SKIP row for each step left out, with why, and a "not picked" row naming the probe scripts the change
didn't pick.
Every step runs even when an earlier one fails, except those that need a build that failed. Exits 1 on any failure.
Fetches origin/BASE first and prints "base: origin/BASE (why)"; a --base with no merge base with HEAD (not on origin,
not a commit) is a FAIL row, while a default base that can't be found only skips steps 4 and 5 and runs the rest in
full. At the end says how many commits the branch is behind it (BASE: --base, else the base of the branch's open PR
when gh knows it, else the branch's upstream when it tracks another branch on origin, else generative: pr_base.py).

Emulator boots pick free UDP ports and retry, so a port another run holds doesn't kill a boot, and no two write the
same card image or log file: that is what lets them run side by side. The probe scripts run in real time, so a
busy machine can fail an expect that passes alone. An emulator check that fails beside the others therefore runs
once more on its own when they are done, and that run's result counts: the table's note and the last line say it
passed alone. The failed run's output is kept as NAME.together.log; the emulator's own logs (emu-NAME/ and the like)
are the second run's. Logs: build/check_all/.

Step 6 builds build/, so it needs build/ in the default configuration (no ENABLE_SYSEX_TEST or ENABLE_SYSEX_LOAD). A
hardware run's `deluge_probe.py session --build hardware` builds in build-hardware/ instead, so the two can run at
once. Configuring build/ for the test port yourself (for `--build release`) breaks this run: configure it back with
-DENABLE_SYSEX_TEST=OFF -DENABLE_SYSEX_LOAD=OFF first.

Times on the 12-core Mac with warm builds, at load averages of 10 to 25 from other work (#314): a change outside src/ and the probe scripts about 1 minute (the sanitized specs, and the unit tests
for scripts/: 160 s with a cold sanitized build); a narrow src/ change 2 to 3 minutes plus the scripts it picks (a
sidebar.cpp change: 120 to 170 s with smoke alone, 460 s once it picks perform_preset_browser.txt); --full 9.5 minutes
(569 s, perform_preset_browser.txt's 5.5 minutes the longest path). A run with delugemu can pass an agent's 600 s Bash
limit: start it in the background with its output in a file and poll the
file. Each step prints a "[check-all] NAME: ..." line as it starts and another with its result as it ends, so the file
shows where the run is. Use --quick while iterating.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shlex
import subprocess
import sys
import time
from typing import NamedTuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import covers
import osc_octaves
import pr_base
import test_port_emu

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
TASKS = os.path.join(REPO, "scripts", "tasks")
LOGS = os.path.join(REPO, "build", "check_all")
DBT = os.path.join(REPO, "dbt")
TAIL = 15  # log lines shown for a failing step
ALONE = "passed alone after failing beside the others"
# Emulators at once by default. Each is one busy thread; half the cores leaves the real-time ones room to keep time.
JOBS = max(1, min(6, (os.cpu_count() or 2) // 2))
# A change under one of these runs the Python unit tests (test_make_pack.py checks pack/ as committed;
# test_arm_types.py lists the Generator's files under src/deluge/generative/).
UNIT_TEST_PATHS = ("scripts/", "pack/", "src/deluge/generative/")
# Each emulator job's seconds on its last run here, to start the longest first.
COSTS = os.path.join(LOGS, "costs.json")
# Seconds for the emulator jobs before a run here has recorded them (6 at a time on 12 cores). A job in neither starts
# right after the longest listed one (longest_first()).
LISTED_COSTS = {
    "delugemu perform_preset_browser.txt": 240,
    "delugemu perform_clip.txt": 100,
    "delugemu perform_retire.txt": 133,
    "delugemu perform_genre_change.txt": 124,
    "delugemu perform_transport.txt": 118,
    "delugemu perform_transition.txt": 109,
    "leak check": 55,
    "delugemu perform_sounds_override.txt": 66,
    "delugemu perform_sound_editor.txt": 56,
    "delugemu perform_sound_controls.txt": 53,
}
SIZE_LINE = r"^\s*(\d+\s+\d+\s+\d+)\s+\d+\s+\w+\s+\S*deluge.elf$"  # arm-none-eabi-size after a build


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
    )


class Plan(NamedTuple):
    """What a run checks. A skip_ field is why that part is skipped, or None to run it."""

    skip_unit_tests: "str | None"
    # both firmware builds, the step section, smoke, the leak check and the probe scripts
    skip_firmware: "str | None"
    scripts: list  # the probe scripts to run in delugemu, test_port_smoke.txt first
    not_picked: list  # the probe scripts whose covers paths didn't change
    not_run: list  # the names of the scripts delugemu can't run ("# needs:" lines)


def plan(changed, base_ref, full=False, given=None):
    """The Plan for the paths changed since the merge base with base_ref (None: no merge base, so everything). --full
    runs everything; --scripts (`given`) runs those scripts, with the firmware they need. The leak check runs whenever
    the firmware does."""
    everything, skipped = covers.probe_scripts(HERE)
    smoke = os.path.join(HERE, os.path.basename(covers.SMOKE))
    if full or changed is None:
        return Plan(None, None, given or everything, [], skipped)
    unit = None
    if not any(f.startswith(UNIT_TEST_PATHS) for f in changed):
        unit = f"nothing under {', '.join(UNIT_TEST_PATHS)} changed since {base_ref}"
    if given:
        return Plan(unit, None, given, [], skipped)
    picked = covers.pick(everything, changed, REPO)
    # explicit: a firmware change runs the leg even if every script, test_port_smoke.txt too, had covers lines. Not
    # `if not picked` alone: pick() returns [] then too. firmware_changed() runs only when nothing was picked.
    if not picked and not covers.firmware_changed(changed, REPO):
        why = f"no firmware input, probe script or what one covers changed since {base_ref}"
        return Plan(unit, why, [], [], skipped)
    others = [s for s in picked if s != smoke]
    return Plan(
        unit,
        None,
        [smoke, *others],
        [s for s in everything if s not in picked],
        skipped,
    )


def read_costs(path=None):
    """The emulator jobs' recorded seconds by name; {} if there are none or the file is unreadable."""
    try:
        with open(path or COSTS) as fh:
            costs = json.load(fh)
        return {str(k): float(v) for k, v in costs.items()}
    except (OSError, ValueError, AttributeError, TypeError):
        return {}


def write_costs(path, rows):
    """Records each row's seconds over the costs already recorded."""
    costs = read_costs(path)
    # PASS rows only: a failure (a boot that never answered, an expect that timed out) says nothing about its length.
    costs.update(
        {
            name: round(seconds, 1)
            for name, result, seconds, _, _ in rows
            if result == "PASS"
        }
    )
    with open(path, "w") as fh:
        json.dump(costs, fh, indent=1, sort_keys=True)


def longest_first(jobs, recorded, listed=LISTED_COSTS):
    """The jobs in the order to start them, longest first: by recorded cost, else listed, else just short of the longest
    listed one."""

    # An unknown job counts as just short of the longest listed one: a new script may be long, but on a fresh worktree
    # (nothing recorded) the longest listed job still starts first, not after a dozen short unknown ones.
    unknown = max(listed.values(), default=0.0) - 0.001

    def cost(job):
        name = job[0]
        return recorded.get(name, listed.get(name, unknown))

    return sorted(jobs, key=cost, reverse=True)


def resolve_script(name):
    """A --scripts argument as a path: as given, else under scripts/generative/ (a bare name such as
    perform_homes.txt). Exits naming it if neither exists, rather than failing its delugemu run later."""
    for path in (name, os.path.join(HERE, name)):
        if os.path.isfile(path):
            return os.path.abspath(path)
    sys.exit(f"check_all: no probe script {name} (as given, or in {HERE})")


def same_named(scripts):
    """The file names that more than one of `scripts` has: their logs and card images would be the same files."""
    names = [os.path.basename(s) for s in scripts]
    return sorted({n for n in names if names.count(n) > 1})


class Checks:
    def __init__(self):
        self.rows = []
        self.cost_rows = []  # run_together()'s jobs, each with the seconds of its run that counts
        os.makedirs(LOGS, exist_ok=True)
        for stale in glob.glob(os.path.join(LOGS, "*.together.log")):
            os.unlink(stale)  # an earlier run's: this run's retries write their own

    def run(self, name, cmd, detail=None):
        """Runs cmd (a list) in the repo, logging to build/check_all/. detail(log_text) gives the table's note."""
        row = self._run(name, cmd, detail)
        self.rows.append(row)
        return row[1] == "PASS"

    def run_together(self, jobs, workers, start_order=None):
        """Runs (name, cmd, detail) jobs, `workers` at a time, started in start_order (default: as given). Their rows
        keep the jobs' order.

        A job that fails beside others runs once more alone, after them, and that result is its row: the load of
        the others can fail a check that keeps real time. Its first log moves to NAME.together.log."""
        order = start_order or jobs
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            done = dict(
                zip(
                    [job[0] for job in order],
                    pool.map(lambda job: self._run(*job), order),
                )
            )
        rows = [done[job[0]] for job in jobs]
        # what each job costs: the run that counts, so a job that passed alone records the alone run's seconds
        costs = list(rows)
        if workers > 1 and len(jobs) > 1:
            for i, (job, first) in enumerate(zip(jobs, rows)):
                if first[1] != "FAIL":
                    continue
                os.replace(first[4], first[4][: -len(".log")] + ".together.log")
                print(
                    f"[check-all] {job[0]}: failed beside the others, running it alone",
                    flush=True,
                )
                name, result, seconds, note, log_path = self._run(*job)
                if result == "PASS":
                    note = f"{note}; {ALONE}".lstrip("; ")
                rows[i] = (name, result, first[2] + seconds, note, log_path)
                costs[i] = (name, result, seconds, note, log_path)
        self.rows += rows
        self.cost_rows += costs

    def _run(self, name, cmd, detail=None):
        log_path = os.path.join(LOGS, re.sub(r"[^\w.-]+", "_", name) + ".log")
        print(
            f"[check-all] {name}: {' '.join(shlex.quote(c) for c in cmd)}", flush=True
        )
        start = time.monotonic()
        with open(log_path, "w") as fh:
            code = subprocess.run(
                cmd,
                cwd=REPO,
                stdout=fh,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                check=False,
            ).returncode
        seconds = time.monotonic() - start
        with open(log_path, errors="replace") as fh:
            text = fh.read()
        note = detail(text) if detail else ""
        ok = code == 0
        report = [
            f"[check-all] {name}: {'PASS' if ok else 'FAIL'} ({seconds:.0f} s) {note}"
        ]
        if not ok:
            lines = text.replace("\r", "\n").splitlines()
            report += ["    " + line for line in failed_expects(lines) + lines[-TAIL:]]
        # One write, so the reports of jobs running together don't interleave.
        sys.stdout.write("\n".join(report) + "\n")
        sys.stdout.flush()
        return (name, "PASS" if ok else "FAIL", seconds, note, log_path)

    def fail(self, name, why):
        """A FAIL row for a check that couldn't run at all."""
        self.rows.append((name, "FAIL", 0.0, why, ""))
        print(f"[check-all] {name}: FAIL ({why})", flush=True)

    def skip(self, name, why):
        self.rows.append((name, "SKIP", 0.0, why, ""))
        print(f"[check-all] {name}: SKIP ({why})", flush=True)

    def table(self):
        width = max(len(r[0]) for r in self.rows)
        print("\n" + f"{'check':<{width}}  result   time  note")
        for name, result, seconds, note, _ in self.rows:
            print(f"{name:<{width}}  {result:<6} {seconds:5.0f}s  {note}")
        return sum(1 for r in self.rows if r[1] == "FAIL")

    def passed_alone(self):
        """How many checks passed only on their run alone."""
        return sum(1 for r in self.rows if r[1] == "PASS" and r[3].endswith(ALONE))


def failed_expects(lines):
    """A probe script's FAIL lines, each with the "got ..." lines the probe prints under it: they may sit above the
    log's tail, and the next run overwrites the log."""
    picked, keep = [], False
    for line in lines:
        if re.search(r": FAIL (line \d+|end):", line):
            picked.append(line)
            keep = True
        elif keep and re.search(r":?\s+got ", line):
            picked.append(line)
        else:
            keep = False
    return picked


def last_match(pattern):
    def detail(text):
        found = re.findall(pattern, text, re.MULTILINE)
        return found[-1] if found else ""

    return detail


# The ARM types row: its count line, or how to get a toolchain on a fresh clone.
ARM_TYPES_DETAIL = last_match(
    r"^(?:\d+ files on ARM.*|arm_types: no ARM toolchain yet.*)"
)


def step_section_detail(text):
    """The step section check's note: the section's size and how many Generator call targets lie outside it."""
    size = last_match(r"^\.text\.generative_step: \S+ \(([\d,]+ B)\)")(text)
    outside = last_match(r"^deluge::generative targets outside it: (\d+)")(text)
    return f"{size}, {outside} outside" if size else ""


def leak_detail(text):
    """The leak check's note: its problem count (as gen_emu.py counts them) and each cycle's deltas, or why it stopped
    before any cycle."""
    deltas = re.findall(
        r"cycle (\d+): internal \d+ -> \d+ \(([+-]\d+) B\), external \d+ -> \d+ \(([+-]\d+) B\)",
        text,
    )
    summary = ", ".join(f"c{n} {i}/{e}" for n, i, e in deltas)
    summary = f"internal/external B per cycle: {summary}" if summary else ""
    problems = re.findall(r"leak check: FAIL, (\d+) problems", text)
    if problems:
        count = f"{problems[-1]} problems"
        return f"{count}; {summary}" if summary else count
    stopped = re.findall(r"FAIL leak check: (.*)", text)
    if stopped:
        return stopped[-1]
    return summary


def load_warning(load, cores):
    """A warning when the host is busier than it has cores, or None: delugemu's timing checks fail under a load
    average of 21-38 (#181)."""
    if load <= cores:
        return None
    return (
        f"WARNING: the load average is {load:.0f} on {cores} cores: delugemu's timing checks may fail for that "
        "alone; rerun a failure when the host is quieter"
    )


def host_load_warning():
    try:
        return load_warning(os.getloadavg()[0], os.cpu_count() or 1)
    except OSError:
        return None


def summary_load_warning(at_start, at_end):
    """The one load warning the summary repeats: the end's, else the start's, which has scrolled away by then."""
    return at_end or at_start


def find_base(given, checks):
    """pr_base.merge_base(given), printed. A --base given that has no merge base with HEAD is a FAIL row: the
    checks that need it are only skipped, and the run must not end PASS on the base asked for. The default base
    missing only skips them."""
    base = pr_base.merge_base(given, REPO)
    print(f"[check-all] {base.line()}", flush=True)
    if given and (base.error or not base.merge_base):
        checks.fail("base", base.line())
    return base


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--base",
        help="branch to compare with on origin, bare or as origin/NAME, or a commit (default: the PR's base, else the "
        "upstream it was made from, else generative)",
    )
    p.add_argument(
        "--quick", action="store_true", help="skip everything that runs delugemu"
    )
    p.add_argument(
        "--full",
        action="store_true",
        help="run every check and every probe script, whatever changed: the final run before a merge into "
        "generative or a milestone branch",
    )
    p.add_argument(
        "--jobs",
        type=int,
        default=JOBS,
        help=f"emulators to run at once (default: {JOBS}, half the cores up to 6)",
    )
    p.add_argument(
        "--scripts",
        nargs="+",
        help="probe scripts to run in delugemu instead of those the change picks: paths, or names in "
        "scripts/generative/",
    )
    args = p.parse_args()
    if args.jobs < 1:
        p.error("--jobs must be at least 1")
    # Before anything runs: a bad --scripts stops the run here, not after the builds.
    given = [resolve_script(s) for s in args.scripts] if args.scripts else None
    if given and same_named(given):
        sys.exit(
            "check_all: --scripts names these more than once: "
            + ", ".join(same_named(given))
        )
    started = time.monotonic()
    busy_at_start = host_load_warning()
    if busy_at_start:
        print(f"[check-all] {busy_at_start}", flush=True)
    checks = Checks()
    base = find_base(args.base, checks)
    base_ref, merge_base = base.ref, base.merge_base
    py = sys.executable
    changed = covers.changed_paths(merge_base, REPO) if merge_base else None
    todo = plan(changed, base_ref, args.full, given)
    if args.full:
        print("[check-all] --full: every check and every probe script", flush=True)
    elif changed is None:
        print(
            f"[check-all] no merge base with {base_ref}: every check and every probe script",
            flush=True,
        )

    checks.run(
        "ARM types",
        [py, os.path.join(HERE, "arm_types.py")],
        ARM_TYPES_DETAIL,
    )

    checks.run(
        "covers paths",
        [py, os.path.join(HERE, "covers.py"), "--check"],
        last_match(r"^\d+ of \d+ scripts have covers lines.*"),
    )

    checks.run(
        "harness specs, sanitized",
        [DBT, "test", "--generator", "--sanitize"],
        last_match(r"\d+% tests passed.*"),
    )
    # osc_octaves.py reads the plain harness build's generator_render: built here, the plain specs not run (#314).
    render = checks.run(
        "harness build (generator_render)",
        [DBT, "test", "--generator", "--no-run"],
    )
    if not render:
        stale = "the harness build failed (generator_render may be stale)"
        checks.skip("riser fit", stale)
        checks.skip("oscillator octaves", stale)
    else:
        checks.run(
            "riser fit",
            [py, os.path.join(HERE, "riser_fit.py")],
            last_match(r"^\d+ pack Risers.*"),
        )
        if not os.path.isdir(osc_octaves.CARD):
            checks.skip("oscillator octaves", f"no card at {osc_octaves.CARD}")
        else:
            checks.run(
                "oscillator octaves",
                [py, os.path.join(HERE, "osc_octaves.py")],
                last_match(r"^\d+ pitched palette presets.*"),
            )

    if todo.skip_unit_tests:
        checks.skip("python unit tests", todo.skip_unit_tests)
    else:
        checks.run(
            "python unit tests",
            [py, os.path.join(HERE, "unit_tests.py"), HERE, TASKS],
            last_match(r"^Ran \d+ tests.*"),
        )

    if merge_base:
        checks.run(
            "golden versions",
            [py, os.path.join(HERE, "goldens.py"), "check", "--base", merge_base],
            lambda text: "; ".join(text.strip().splitlines()[-4:]) or "unchanged",
        )
    else:
        checks.skip("golden versions", f"no merge base with {base_ref}")

    files = (
        [f for f in changed if os.path.isfile(os.path.join(REPO, f))]
        if changed is not None
        else None
    )
    if not merge_base:
        checks.skip("hooks", f"no merge base with {base_ref}")
    elif files is None:
        checks.skip("hooks", f"git diff against {base_ref} failed")
    elif not files:
        checks.skip("hooks", f"no files changed since {base_ref}")
    else:
        command = "pre-commit run --files " + " ".join(shlex.quote(f) for f in files)
        checks.run("hooks", [DBT, "exec", command], lambda text: f"{len(files)} files")

    built = False
    if todo.skip_firmware:
        checks.skip("build release", todo.skip_firmware)
        checks.skip("step section", "no build")
    else:
        built = checks.run(*RELEASE_BUILD)
        if built:
            checks.run(
                "step section",
                [
                    py,
                    os.path.join(HERE, "step_section.py"),
                    os.path.join(REPO, "build", "Release", "deluge.elf"),
                ],
                step_section_detail,
            )
        else:
            checks.skip("step section", "build release failed")
    checks.run(
        "generator size",
        [py, os.path.join(HERE, "generator_size.py")],
        last_match(r"^sizeof\(Generator\) ([\d,]+ B)"),
    )

    if args.quick:
        checks.skip("delugemu", "--quick")
    elif todo.skip_firmware:
        checks.skip("delugemu", todo.skip_firmware)
    else:
        run_delugemu(checks, todo, built, args.jobs, given)

    failed = checks.table()
    behind = git("rev-list", "--count", f"HEAD..{base_ref}").stdout.strip()
    if behind:
        print(
            f"\nbehind {base_ref} by {behind} commits"
            + (" (merge it before reporting done)" if behind != "0" else "")
        )
    else:
        print(f"\ncan't tell how far behind {base_ref} the branch is")
    print(f"logs in {LOGS}; the run took {time.monotonic() - started:.0f} s")
    busy = summary_load_warning(busy_at_start, host_load_warning())
    if busy:
        print(busy)
    alone = checks.passed_alone()
    print(
        "check_all: "
        + ("FAIL" if failed else "PASS")
        + (f" ({alone} passed only alone, see the notes)" if alone else "")
    )
    return 1 if failed else 0


RELEASE_BUILD = (
    "build release",
    [DBT, "build", "release"],
    lambda text: "text/data/bss " + "/".join(last_match(SIZE_LINE)(text).split()),
)
TESTPORT_BUILD = (
    "build test-port firmware",
    [
        sys.executable,
        "-c",
        "import sys; sys.path.insert(0, sys.argv[1]); import test_port_emu; test_port_emu.build_testport()",
        HERE,
    ],
)


def run_delugemu(checks, todo, built, workers, given):
    """The test-port build, then smoke, the leak check and the plan's probe scripts, `workers` at a time, the longest
    first."""
    py = sys.executable
    # Not started beside the harness specs: two A/B runs of a narrow src/ change saved 1 and 6 s, within the noise
    # (#314). Both builds are ninja-parallel already.
    testport = checks.run(*TESTPORT_BUILD)
    elf = test_port_emu.TESTPORT_ELF
    jobs = []
    if built:
        jobs.append(
            (
                "smoke",
                [py, os.path.join(HERE, "gen_emu.py"), "smoke", "--no-leak"],
                last_match(r"\d+ checks, \d+ failed"),
            )
        )
    else:
        checks.skip("smoke", "build release failed")
    if testport:
        jobs.append(
            (
                "leak check",
                [
                    py,
                    os.path.join(HERE, "gen_emu.py"),
                    "leak",
                    "--testport-elf",
                    elf,
                ],
                leak_detail,
            )
        )
        for script in todo.scripts:
            name = os.path.basename(script)
            cmd = [
                py,
                os.path.join(HERE, "test_port_emu.py"),
                "--elf",
                elf,
                "--script",
                script,
            ]
            cmd += ["--out", os.path.join(LOGS, "emu-" + os.path.splitext(name)[0])]
            if name != os.path.basename(covers.SMOKE):
                # the error replies are checked once, with the smoke script
                cmd.append("--no-protocol-checks")
            jobs.append(
                (f"delugemu {name}", cmd, last_match(r"\d+ checks, \d+ failed"))
            )
    else:
        checks.skip("leak check and probe scripts", "the test-port build failed")
    checks.run_together(jobs, workers, longest_first(jobs, read_costs()))
    write_costs(COSTS, checks.cost_rows)
    if todo.not_picked:
        names = ", ".join(os.path.basename(s) for s in todo.not_picked)
        checks.skip(
            "not picked",
            f"{names}: nothing their # covers: lines name changed (--full runs them)",
        )
    if todo.not_run and not given:
        checks.skip("not run", ", ".join(todo.not_run) + ' ("# needs:" line)')


if __name__ == "__main__":
    sys.exit(main())
