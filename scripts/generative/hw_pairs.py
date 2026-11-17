"""Compare a PR's build with its base's on the one Deluge: alternate hardware sessions, then summarise their timing.

    python3 scripts/generative/hw_pairs.py run --base W1 --pr W2 --rounds N --events REGEX --out DIR SCRIPT...
    python3 scripts/generative/hw_pairs.py summary DIR/*.log

`run` runs `deluge_probe.py session --build hardware` in the base worktree W1 and the PR worktree W2, N rounds,
the side that goes first alternating per round (base1, pr1, pr2, base2, base3, pr3, ...), so drift over the run (heat,
the card) and the place in a round hit both sides alike. Each session's output goes to DIR/<side><n>.log, ending with
a `session exit=N` line, and one progress line per session goes to stdout.
SCRIPT is a path in the worktree, or a bare name in scripts/generative/; the smoke script is put first when it's
missing (the deluge-hardware-test skill's rule). Each script must exist in both worktrees. Every session takes the
lock and honours the hung marker itself (hardware_session.py); `run` stops at once:
- on exit 2 or 3 (the device hung, or was already marked hung): exit 2. Never clear the marker; run the scripts in
  delugemu and report the hardware run as pending, as the skill says.
- when no Deluge MIDI port answered, or a session ended before running its scripts (a failed build, the lock held
  past --wait, a load that didn't take, a crash): exit 1.
A session whose scripts ran but had failed expects is still a timing sample: `run` goes on and exits 1 at the end.

A full run takes several minutes a session and can pass Bash's 600 s limit. Start it in the background and wait on
its marker (docs/agents/orchestration.md, the Runner brief):

    python3 scripts/generative/hw_pairs.py run ... > LOG 2>&1; echo "hw_pairs exit=$?" >> LOG

`summary` reads session logs (from `run`, or any `deluge_probe.py session --events ...` log) and prints one line per
log and, when the names start with base/pr, a base vs PR comparison. It reads only the `  event` lines the session
prints for --events, so the events must be among them: `--events 'gen_h1 |gen_h16|gen_transition start'`.
- gen_h1 (H1, a window of steps): the Generator's step time, its mean weighted by steps, and the max.
- gen_h16 (H16): voices culled for CPU (with the parts named by `culled=`) and the slowest render: its max over the
  windows, and its mean over the windows. The max is one window's outlier and moves by about 30% between sessions of
  the same build (#282); compare builds by the mean.
- gen_transition start ... us=: how long a transition's start took, as a range.
- failed: expects failed, over every script. Each side runs its own worktree's probe: a base older than the end
  check for dropped test events (`FAIL end: N test events dropped`, #272) never fails it, so its count can be up to
  one a script lower for the same run.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = "scripts/generative/deluge_probe.py"
SMOKE = "scripts/generative/test_port_smoke.txt"
SIDES = ("base", "pr")

EXIT_HUNG, EXIT_REFUSED = 2, 3  # deluge_probe.py session's (hardware_session.py)
NO_MIDI = re.compile(r"No Deluge MIDI port|RtMidiError")
SCRIPTS_RAN = re.compile(r"^session: \d+ scripts?, \d+ with failures", re.MULTILINE)

EVENT = re.compile(r"^  event [\d.]+: (.*)$", re.MULTILINE)
H1 = re.compile(r"^gen_h1 steps=(\d+) mean_ns=(\d+) max_ns=(\d+)")
H16 = re.compile(r"^gen_h16 culls=(\d+)(?: culled=(\S+))?.* render_max_us=(\d+)")
START = re.compile(r"^gen_transition start style=\d+ us=(\d+)")
SCRIPT_RESULT = re.compile(r"^session: .+: \d+ expects, (\d+) failed", re.MULTILINE)
SESSION_EXIT = re.compile(r"^session exit=(-?\d+)", re.MULTILINE)


# run ------------------------------------------------------------------------------------------------------------


def script_path(name):
    return name if "/" in name else f"scripts/generative/{name}"


def session_command(owner, events, scripts):
    """The ./dbt exec command line for one session, as the skill writes it."""
    inner = ["python3", PROBE, "session", "--build", "hardware", "--owner", owner]
    if events:
        inner += ["--events", events]
    inner += scripts
    return ["./dbt", "exec", shlex.join(inner)]


def run_session(worktree, owner, events, scripts, log):
    """Runs one session in `worktree`, its output to `log` with a closing `session exit=N` line. Returns the code."""
    with open(log, "w") as f:
        code = subprocess.run(
            session_command(owner, events, scripts),
            cwd=worktree,
            stdout=f,
            stderr=subprocess.STDOUT,
            check=False,
        ).returncode
        f.write(f"session exit={code}\n")
    return code


def head(worktree):
    result = subprocess.run(
        ["git", "-C", worktree, "rev-parse", "--short", "HEAD"],
        capture_output=True,
        check=False,
        text=True,
    )
    return result.stdout.strip() or "?"


def why_stop(code, text):
    """Why the pairs must stop after a session that exited `code` with output `text`, or None to go on."""
    if code in (EXIT_HUNG, EXIT_REFUSED):
        return "hung"
    if NO_MIDI.search(text):
        return "no Deluge MIDI port"
    if code != 0 and not SCRIPTS_RAN.search(text):
        return "the session ended before running its scripts"
    return None


def round_order(n):
    """The sides in round `n` (from 1): base first in odd rounds, PR first in even ones."""
    return SIDES if n % 2 else SIDES[::-1]


def run_pairs(args, session=run_session, rev=head, out=print):
    scripts = [script_path(s) for s in args.scripts]
    if SMOKE not in scripts:
        scripts.insert(0, SMOKE)
    worktrees = {"base": os.path.abspath(args.base), "pr": os.path.abspath(args.pr)}
    missing = [
        f"{side}: {os.path.join(w, s)}"
        for side, w in worktrees.items()
        for s in [PROBE] + scripts
        if not os.path.isfile(os.path.join(w, s))
    ]
    if missing:
        out("hw_pairs: missing in a worktree (a script must exist on both sides):")
        out("\n".join("  " + m for m in missing))
        return 1
    os.makedirs(args.out, exist_ok=True)
    shas = {side: rev(w) for side, w in worktrees.items()}
    out(f"hw_pairs: base {shas['base']} {worktrees['base']}")
    out(f"hw_pairs: pr   {shas['pr']} {worktrees['pr']}")
    failed = False
    for n in range(1, args.rounds + 1):
        for side in round_order(n):
            name = f"{side}{n}"
            log = os.path.join(args.out, f"{name}.log")
            code = session(
                worktrees[side], f"{args.owner}-{name}", args.events, scripts, log
            )
            out(f"hw_pairs: {name} exit={code} {shas[side]} {log}")
            with open(log) as f:
                text = f.read()
            stop = why_stop(code, text)
            if stop == "hung":
                out(
                    f"hw_pairs: {name}: the Deluge is marked hung. Stopped; never clear the marker. Run the scripts in "
                    "delugemu (the command is in the log) and report the hardware run as pending."
                )
                return 2
            if stop:
                out(f"hw_pairs: {name}: {stop}. Stopped (see {log}).")
                return 1
            failed = failed or code != 0
    out(
        f"hw_pairs: {args.rounds} rounds done"
        + (", with failed expects" if failed else "")
    )
    return 1 if failed else 0


# summary --------------------------------------------------------------------------------------------------------


class Stats:
    """What a log (or a side's logs together) says about timing."""

    def __init__(self):
        self.steps = 0
        self.weighted_ns = 0  # sum of each window's mean_ns * steps
        self.windows = 0
        self.h1_max_ns = None
        self.culls = 0
        self.culled = []  # part names, in order of first appearance
        self.render_us = []  # each window's slowest render
        self.start_us = []
        self.failed = 0
        self.exits = []

    def add_text(self, text):
        for event in EVENT.findall(text):
            if m := H1.match(event):
                steps, mean, peak = map(int, m.groups())
                self.windows += 1
                self.steps += steps
                self.weighted_ns += mean * steps
                self.h1_max_ns = max(self.h1_max_ns or 0, peak)
            elif m := H16.match(event):
                self.culls += int(m.group(1))
                if m.group(2) and m.group(2) != "-" and m.group(2) not in self.culled:
                    self.culled.append(m.group(2))
                self.render_us.append(int(m.group(3)))
            elif m := START.match(event):
                self.start_us.append(int(m.group(1)))
        self.failed += sum(int(f) for f in SCRIPT_RESULT.findall(text))
        self.exits += [int(c) for c in SESSION_EXIT.findall(text)]
        return self

    def add(self, other):
        self.steps += other.steps
        self.weighted_ns += other.weighted_ns
        self.windows += other.windows
        if other.h1_max_ns is not None:
            self.h1_max_ns = max(self.h1_max_ns or 0, other.h1_max_ns)
        self.culls += other.culls
        self.culled += [c for c in other.culled if c not in self.culled]
        self.render_us += other.render_us
        self.start_us += other.start_us
        self.failed += other.failed
        self.exits += other.exits
        return self

    @property
    def h1_mean_us(self):
        return self.weighted_ns / self.steps / 1000 if self.steps else None

    @property
    def render_max_us(self):
        return max(self.render_us, default=None)

    @property
    def render_mean_us(self):
        return sum(self.render_us) / len(self.render_us) if self.render_us else None

    @property
    def h1_max_us(self):
        return None if self.h1_max_ns is None else self.h1_max_ns / 1000

    def line(self):
        parts = [f"windows {self.windows:3}"]
        parts.append(
            f"H1 mean {fmt(self.h1_mean_us, 6)} us  max {fmt(self.h1_max_us, 6)} us"
        )
        parts.append(
            f"culls {self.culls}"
            + (f" ({','.join(self.culled)})" if self.culled else "")
        )
        parts.append(
            f"render max {fmt(self.render_max_us, 5, 0)} us  mean {fmt(self.render_mean_us, 5, 0)} us"
        )
        if self.start_us:
            parts.append(
                f"start {min(self.start_us)}-{max(self.start_us)} us ({len(self.start_us)})"
            )
        parts.append(f"failed {self.failed}")
        if self.exits:
            parts.append("exit " + ",".join(map(str, self.exits)))
        return "  ".join(parts)


def fmt(value, width, places=1):
    return f"{'-':>{width}}" if value is None else f"{value:{width}.{places}f}"


def side_of(path):
    m = re.match(r"(base|pr)\d*\b", os.path.basename(path))
    return m.group(1) if m else None


def delta(name, base, pr, unit):
    if base is None or pr is None:
        return f"{name} -"
    text = f"{name} {pr - base:+.1f} {unit}"
    return text + (f" ({(pr - base) / base * 100:+.1f}%)" if base else "")


def summarise(paths, out=print):
    sides = {side: Stats() for side in SIDES}
    width = max(len(os.path.basename(p)) for p in paths)
    for path in paths:
        with open(path, errors="replace") as f:
            stats = Stats().add_text(f.read())
        out(f"{os.path.basename(path):{width}}  {stats.line()}")
        if side := side_of(path):
            sides[side].add(stats)
    base, pr = sides["base"], sides["pr"]
    if not (base.exits or base.windows) or not (pr.exits or pr.windows):
        return 0
    out("")
    out(f"{'base':{width}}  {base.line()}")
    out(f"{'pr':{width}}  {pr.line()}")
    out(
        "pr - base: "
        + ", ".join(
            [
                delta("H1 mean", base.h1_mean_us, pr.h1_mean_us, "us"),
                delta("H1 max", base.h1_max_us, pr.h1_max_us, "us"),
                delta("render mean", base.render_mean_us, pr.render_mean_us, "us"),
                delta("render max", base.render_max_us, pr.render_max_us, "us"),
                f"culls {pr.culls - base.culls:+d}",
            ]
        )
    )
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="alternate base and PR sessions on the Deluge")
    run.add_argument("--base", required=True, help="the base worktree")
    run.add_argument("--pr", required=True, help="the PR worktree")
    run.add_argument("--rounds", type=int, default=2, help="base/pr pairs (default 2)")
    run.add_argument(
        "--events",
        default="gen_h1 |gen_h16|gen_transition start",
        help="the session's --events regex (default: what summary reads)",
    )
    run.add_argument("--out", required=True, help="directory for <side><n>.log")
    run.add_argument(
        "--owner",
        default="hw_pairs",
        help="lock owner note; each session adds -<side><n>",
    )
    run.add_argument("scripts", nargs="+", metavar="SCRIPT")
    summary = sub.add_parser("summary", help="summarise session logs, base vs PR")
    summary.add_argument("logs", nargs="+", metavar="LOG")
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(
        line_buffering=True
    )  # progress lines reach a LOG file as they happen
    if args.command == "run":
        if args.rounds < 1:
            parser.error("--rounds must be at least 1")
        return run_pairs(args)
    return summarise(args.logs)


if __name__ == "__main__":
    sys.exit(main())
