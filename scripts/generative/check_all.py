"""Every local check for a Generative change in one run, with one results table at the end.

    python3 scripts/generative/check_all.py [--quick] [--base BRANCH] [--jobs N] [--scripts SCRIPT...]

Run it from the host, in the checkout to check: it calls ./dbt itself. In order:
  1. harness specs, plain and sanitized: ./dbt test --generator, ./dbt test --generator --sanitize
  2. the Python unit tests of these scripts (scripts/generative/test_*.py): python3 -m unittest discover
  3. hooks on every file changed since the merge base with origin/BASE (committed, staged, unstaged or untracked):
     ./dbt exec "pre-commit run --files ..."
  4. ./dbt build release
  5. delugemu (skipped by --quick), after the test-port build in build-testport/, --jobs emulators at a time:
     gen_emu.py smoke --no-leak (the functional smoke checks), gen_emu.py leak (the memory check), and each probe
     script in delugemu on its own boot (test_port_emu.py, on the test-port build): by default test_port_smoke.txt
     (with the test port's error-reply checks) and every scripts/generative/perform_*.txt except includes and the
     ones with a "# needs:" line (their own card, or another runner); or the --scripts given.
Every step runs even when an earlier one fails, except those that need a build that failed. Exits 1 on any failure.
Also says how many commits the branch is behind origin/BASE (BASE: --base, else the base of the branch's open PR
when gh knows it, else generative).

Emulator boots pick free UDP ports and retry, so a port another run holds doesn't kill a boot, and each has its own
card image and log directory: that is what lets them run side by side. The probe scripts run in real time, so a
busy machine can fail an expect that passes alone. An emulator check that fails beside the others therefore runs
once more on its own when they are done, and that run's result counts: the table's note says it passed alone, and
the failed run's log is kept as NAME.together.log. Logs: build/check_all/.

Step 4 builds build/, so it needs build/ in the default configuration (no ENABLE_SYSEX_TEST or ENABLE_SYSEX_LOAD). A
hardware run's `deluge_probe.py session --build hardware` builds in build-hardware/ instead, so the two can run at
once. Configuring build/ for the test port yourself (for `--build release`) breaks this run: configure it back with
-DENABLE_SYSEX_TEST=OFF -DENABLE_SYSEX_LOAD=OFF first.

The full run takes 5 minutes with warm builds and 10 with cold ones, which can pass an agent's 600 s Bash limit: start
it in the background with its output in a file and poll the file. Each step prints a "[check-all] NAME: ..." line as
it starts and another with its result as it ends, so the file shows where the run is. Use --quick while iterating.
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

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_port_emu

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LOGS = os.path.join(REPO, "build", "check_all")
DBT = os.path.join(REPO, "dbt")
TAIL = 15  # log lines shown for a failing step
# Emulators at once by default. Each is one busy thread; half the cores leaves the real-time ones room to keep time.
JOBS = max(1, min(6, (os.cpu_count() or 2) // 2))
SIZE_LINE = r"^\s*(\d+\s+\d+\s+\d+)\s+\d+\s+\w+\s+\S*deluge.elf$"  # arm-none-eabi-size after a build


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
    )


def infer_base():
    """The open PR's base branch, if gh knows one for this branch; else generative."""
    try:
        proc = subprocess.run(
            ["gh", "pr", "view", "--json", "baseRefName"],
            cwd=REPO,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        if proc.returncode == 0:
            return json.loads(proc.stdout)["baseRefName"]
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        pass
    return "generative"


def changed_files(base_ref):
    """Files changed since the merge base with base_ref that still exist, untracked ones included."""
    merge_base = git("merge-base", "HEAD", base_ref).stdout.strip()
    if not merge_base:
        return None
    names = git("diff", "--name-only", "--diff-filter=d", merge_base).stdout.split()
    names += git("ls-files", "--others", "--exclude-standard").stdout.split()
    return sorted({n for n in names if os.path.isfile(os.path.join(REPO, n))})


def resolve_script(name):
    """A --scripts argument as a path: as given, else under scripts/generative/ (a bare name such as
    perform_homes.txt). Exits naming it if neither exists, rather than failing its delugemu run later."""
    for path in (name, os.path.join(HERE, name)):
        if os.path.isfile(path):
            return os.path.abspath(path)
    sys.exit(f"check_all: no probe script {name} (as given, or in {HERE})")


def default_scripts():
    """test_port_smoke.txt, then every perform_*.txt that is a check on its own: not an include, and no "# needs:"
    line."""
    scripts = [os.path.join(HERE, "test_port_smoke.txt")]
    for path in sorted(glob.glob(os.path.join(HERE, "perform_*.txt"))):
        with open(path) as fh:
            text = fh.read()
        if re.search(r"^# needs:", text, re.MULTILINE):
            continue
        if not re.search(r"^start_state\b", text, re.MULTILINE):
            continue  # an include (perform_open.txt and the like)
        scripts.append(path)
    return scripts


def skipped_scripts():
    return [
        os.path.basename(p)
        for p in sorted(glob.glob(os.path.join(HERE, "perform_*.txt")))
        if re.search(r"^# needs:", open(p).read(), re.MULTILINE)
    ]


class Checks:
    def __init__(self):
        self.rows = []
        os.makedirs(LOGS, exist_ok=True)

    def run(self, name, cmd, detail=None):
        """Runs cmd (a list) in the repo, logging to build/check_all/. detail(log_text) gives the table's note."""
        row = self._run(name, cmd, detail)
        self.rows.append(row)
        return row[1] == "PASS"

    def run_together(self, jobs, workers):
        """Runs (name, cmd, detail) jobs, `workers` at a time. Their rows keep the jobs' order.

        A job that fails beside others runs once more alone, after them, and that result is its row: the load of
        the others can fail a check that keeps real time. Its first log moves to NAME.together.log."""
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            rows = list(pool.map(lambda job: self._run(*job), jobs))
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
                    note = (
                        f"{note}; passed alone after failing beside the others".lstrip(
                            "; "
                        )
                    )
                rows[i] = (name, result, first[2] + seconds, note, log_path)
        self.rows += rows

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
        print(
            "\n".join(report), flush=True
        )  # one write, so jobs running together don't interleave
        return (name, "PASS" if ok else "FAIL", seconds, note, log_path)

    def skip(self, name, why):
        self.rows.append((name, "SKIP", 0.0, why, ""))
        print(f"[check-all] {name}: SKIP ({why})", flush=True)

    def table(self):
        width = max(len(r[0]) for r in self.rows)
        print("\n" + f"{'check':<{width}}  result   time  note")
        for name, result, seconds, note, _ in self.rows:
            print(f"{name:<{width}}  {result:<6} {seconds:5.0f}s  {note}")
        return sum(1 for r in self.rows if r[1] == "FAIL")


def failed_expects(lines):
    """A probe script's FAIL lines, each with the "got ..." lines the probe prints under it: they may sit above the
    log's tail, and the next run overwrites the log."""
    picked, keep = [], False
    for line in lines:
        if re.search(r": FAIL line \d+:", line):
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


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--base",
        help="branch to compare with on origin (default: the PR's base, else generative)",
    )
    p.add_argument(
        "--quick", action="store_true", help="skip everything that runs delugemu"
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
        help="probe scripts to run in delugemu instead of every perform_*.txt: paths, or names in "
        "scripts/generative/",
    )
    args = p.parse_args()
    if args.jobs < 1:
        p.error("--jobs must be at least 1")
    started = time.monotonic()
    base = args.base or infer_base()
    base_ref = f"origin/{base}"
    git("fetch", "--quiet", "origin", base)
    checks = Checks()
    py = sys.executable

    checks.run(
        "harness specs",
        [DBT, "test", "--generator"],
        last_match(r"\d+% tests passed.*"),
    )
    checks.run(
        "harness specs, sanitized",
        [DBT, "test", "--generator", "--sanitize"],
        last_match(r"\d+% tests passed.*"),
    )

    checks.run(
        "python unit tests",
        [py, "-m", "unittest", "discover", "-s", HERE, "-p", "test_*.py"],
        last_match(r"^Ran \d+ tests.*"),
    )

    files = changed_files(base_ref)
    if files is None:
        checks.skip("hooks", f"no merge base with {base_ref}")
    elif not files:
        checks.skip("hooks", f"no files changed since {base_ref}")
    else:
        command = "pre-commit run --files " + " ".join(shlex.quote(f) for f in files)
        checks.run("hooks", [DBT, "exec", command], lambda text: f"{len(files)} files")

    built = checks.run(
        "build release",
        [DBT, "build", "release"],
        lambda text: "text/data/bss " + "/".join(last_match(SIZE_LINE)(text).split()),
    )

    if args.quick:
        checks.skip("delugemu", "--quick")
    else:
        testport = checks.run(
            "build test-port firmware",
            [
                py,
                "-c",
                "import sys; sys.path.insert(0, sys.argv[1]); import test_port_emu; test_port_emu.build_testport()",
                HERE,
            ],
        )
        scripts = (
            [resolve_script(s) for s in args.scripts]
            if args.scripts
            else default_scripts()
        )
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
            for script in scripts:
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
                if name != "test_port_smoke.txt":
                    cmd.append(
                        "--no-protocol-checks"
                    )  # the error replies are checked once, with the smoke script
                jobs.append(
                    (f"delugemu {name}", cmd, last_match(r"\d+ checks, \d+ failed"))
                )
        else:
            checks.skip("leak check and probe scripts", "the test-port build failed")
        checks.run_together(jobs, args.jobs)
        if not args.scripts and skipped_scripts():
            checks.skip("not run", ", ".join(skipped_scripts()) + ' ("# needs:" line)')

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
    print("check_all: " + ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
