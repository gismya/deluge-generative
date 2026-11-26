"""The capture runners: N at once, each with its own worktree, --build-dir and --out folder, all under the scratch
folder. gen_emu.py stays the only thing that boots delugemu.

<scratch>/
  card_<card>.img       the card images the jobs name
  wt/r<N>/              runner N's worktree (a detached worktree of the repo's HEAD, made on first use)
  build/r<N>/           its capture build dir (build/r<N>-variants/ for --variant captures: gen_emu.py capture)
  out/r<N>/             its captures in progress
  raw/<name>.wav        finished captures; a merged capture is hard-linked under each job name it serves
  raw/<name>.log        gen_emu's log of each attempt
  raw/locks/<unit>.lock the runner that claimed a variant's captures, or a chunk of them (its pid)
  raw/<name>.failed     a capture that failed twice
  pause                 while it exists, no capture starts
  run.log

A runner claims a whole variant at a time (costliest first), or a chunk of one that holds most of the work
(plan.groups), applies its diff once, takes its captures in Recipe order, and reverts after. Before any capture, every
job passes the Recipe check (recipe.py). A failed capture is retried once; a retry whose WAV is byte-identical to the
first attempt's is kept, since the check's failure is then the audio's own (its sidecar's `check` says why). Before
each capture the runner waits while the pause file exists or the host runs `cap` or more emulators (qemu-system
processes, any session's).
"""

import contextlib
import filecmp
import hashlib
import json
import os
import subprocess
import sys
import threading
import time

from . import jobs as _jobs
from . import plan as _plan
from . import recipe as _recipe
from . import variants as _variants

REPO = _jobs.gen_emu.REPO
POLL_S = 15
RETRY_WAIT_S = 30
_say_lock = threading.Lock()


class Run:
    def __init__(
        self, genre, session, scratch, job_list, runners, cap=6, render=None, repo=REPO
    ):
        self.genre, self.session, self.scratch = (
            genre,
            os.path.abspath(session),
            os.path.abspath(scratch),
        )
        self.jobs, self.runners, self.cap, self.repo = job_list, runners, cap, repo
        self.render = render
        self.raw = os.path.join(self.scratch, "raw")
        self.captures = _jobs.merge(job_list)
        self.tempos = None
        self.head = None  # the repo's HEAD: what every runner's worktree builds
        self._turn = threading.Lock()
        self._active = 0  # this run's captures between their turn and their end (their qemu may not be up yet)

    # -- planning --

    def check(self):
        """The Recipe check over every job; sets self.tempos. Raises RecipeMismatch."""
        render = self.render or _jobs.gen_emu.theory_review.find_render()
        if not render:
            raise _jobs.JobError(
                "the Recipe check needs generator_render: " + _jobs.gen_emu.NO_RENDER
            )
        self.tempos = _recipe.check(self.jobs, render)
        missing = sorted(
            {
                j.variant
                for j in self.jobs
                if j.variant != "base"
                and not os.path.exists(_variants.diff_path(self.session, j.variant))
            }
        )
        if missing:
            raise _jobs.JobError("no diff for variants: " + ", ".join(missing))
        cards = sorted(
            {
                self.card_path(j.card)
                for j in self.jobs
                if not os.path.exists(self.card_path(j.card))
            }
        )
        if cards:
            raise _jobs.JobError("no card image: " + ", ".join(cards))

    def pending(self):
        """The captures still to take: not current and not failed twice with these arguments."""
        return [c for c in self.captures if not self.done(c) and not self.failed(c)]

    def plan(self):
        """The plan for what is left to capture: a rerun splits the work left, not the work done."""
        return _plan.plan(self.pending(), self.runners, lambda c: self.tempos[c.name])

    def card_path(self, card):
        return (
            card
            if card.endswith(".img")
            else os.path.join(self.scratch, f"card_{card}.img")
        )

    # -- running --

    def say(self, msg):
        line = time.strftime("%H:%M:%S ") + msg
        with _say_lock:
            os.makedirs(self.scratch, exist_ok=True)
            with open(os.path.join(self.scratch, "run.log"), "a") as fh:
                fh.write(line + "\n")
            print(line, flush=True)

    def stamp(self, capture):
        """What makes a WAV this capture's: the variant and its diff's hash, the card image (path, size, mtime), the
        capture's flags and the source commit. A WAV whose sidecar differs is captured again."""
        card = self.card_path(capture.job.card)
        st = os.stat(card) if os.path.exists(card) else None
        diff = "-"
        if capture.variant != "base":
            with open(_variants.diff_path(self.session, capture.variant), "rb") as fh:
                diff = hashlib.sha256(fh.read()).hexdigest()
        return {
            "variant": capture.variant,
            "diff": diff,
            "card": f"{card} {st.st_size} {st.st_mtime_ns}" if st else card,
            "args": " ".join(capture.job.capture_flags()),
            "head": self.head or self.repo_head(),
        }

    def repo_head(self):
        return subprocess.run(
            ["git", "-C", self.repo, "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()

    def seconds(self, capture):
        return _plan.seconds(capture.bars, self.tempos[capture.name])

    def done(self, capture):
        """Whether every name the capture serves has a WAV of these arguments, source and card, long enough. A WAV kept
        with its check failed (capture()) is current only for the quiet bars it was checked with: other ones may make
        the check pass."""
        stamp, need = self.stamp(capture), self.seconds(capture)
        quiet = list(capture.quiet)

        def current(info):
            return is_current(info, stamp, need) and (
                not info.get("check") or info.get("quiet", []) == quiet
            )

        return all(
            os.path.exists(os.path.join(self.raw, n + ".wav"))
            and current(read_info(self.raw, n))
            for n in capture.serves
        )

    def failed_marker(self, capture):
        return os.path.join(self.raw, capture.name + ".failed")

    def failed_stamp(self, capture):
        """The stamp a failure is kept under: the capture's, and the bars its check let be silent."""
        return {**self.stamp(capture), "quiet": list(capture.quiet)}

    def failed(self, capture):
        """Whether this capture, with these arguments, already failed twice. A marker from other arguments is old."""
        try:
            with open(self.failed_marker(capture)) as fh:
                return json.load(fh) == self.failed_stamp(capture)
        except (OSError, ValueError):
            return False

    def claim(self, variant):
        """Takes variant's lock, or one whose pid is gone. True if this process holds it."""
        locks = os.path.join(self.raw, "locks")
        os.makedirs(locks, exist_ok=True)
        path = os.path.join(locks, variant.replace(os.sep, "_") + ".lock")
        for _ in range(5):
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                try:
                    with open(path) as fh:
                        pid = int(fh.read().strip() or 0)
                except FileNotFoundError:
                    continue  # released or taken over just now: try again
                except (OSError, ValueError):
                    pid = 0  # being written: a crash between create and write leaves it empty
                if pid and (pid == os.getpid() or _jobs.gen_emu.pid_running(pid)):
                    return False
                try:
                    os.replace(path, path + ".stale")  # its runner died
                except FileNotFoundError:
                    pass  # another runner took it over first: try again
                continue
            with os.fdopen(fd, "w") as fh:
                fh.write(str(os.getpid()))
            return True
        return False

    def release(self, variant):
        path = os.path.join(self.raw, "locks", variant.replace(os.sep, "_") + ".lock")
        with contextlib.suppress(FileNotFoundError):
            os.remove(path)

    def wait_turn(self):
        """Waits while the pause file exists or the host would pass the cap, then counts this capture as active.
        Other sessions' emulators come from ps; this run's are counted by self._active from their turn on, since a
        variant's first capture links for a minute before its qemu starts. Call end_turn() after the capture."""
        pause = os.path.join(self.scratch, "pause")
        said = False
        while True:
            with self._turn:
                others = emulators(exclude_children_of=os.getpid())
                if not os.path.exists(pause) and others + self._active < self.cap:
                    self._active += 1
                    return
            if not said:
                self.say(
                    f"waiting: pause={os.path.exists(pause)} emulators: {others} others', {self._active} this "
                    f"run's (cap {self.cap})"
                )
                said = True
            time.sleep(POLL_S)

    def end_turn(self):
        with self._turn:
            self._active -= 1

    def worktree(self, index):
        """Runner index's worktree, at the HEAD this run started from (the one its stamps record)."""
        return _variants.scratch_worktree(
            self.repo, os.path.join(self.scratch, "wt", f"r{index}"), self.head
        )

    def command(self, index, wt, capture, tmp, seconds):
        """gen_emu.py capture's command line for `capture` on runner `index`, writing `tmp`."""
        return [
            sys.executable,
            os.path.join(wt, "scripts", "generative", "gen_emu.py"),
            "capture",
            "--no-play",
            "--card",
            self.card_path(capture.job.card),
            "--build-dir",
            os.path.join(self.scratch, "build", f"r{index}"),
            "--seconds",
            f"{seconds:g}",
            "--out",
            tmp,
            *capture.job.capture_flags(),
            *(
                ["--quiet-bars", ",".join(map(str, capture.quiet))]
                if capture.quiet
                else []
            ),
        ]

    def capture(self, index, wt, capture):
        seconds = self.seconds(capture)
        own = os.path.join(self.scratch, "out", f"r{index}")
        os.makedirs(own, exist_ok=True)
        tmp = os.path.join(own, capture.name + ".wav")
        cmd = self.command(index, wt, capture, tmp, seconds)
        bad = os.path.join(self.raw, "bad")
        first = os.path.join(bad, f"{capture.name}-attempt1.wav")
        with contextlib.suppress(FileNotFoundError):
            os.remove(first)  # an earlier run's: only this run's first attempt counts
        for attempt in (1, 2):
            self.wait_turn()
            t0 = time.time()
            log = os.path.join(self.raw, f"{capture.name}.log")
            try:
                with open(log, "a") as fh:
                    fh.write(f"== attempt {attempt}: {' '.join(cmd)}\n")
                    fh.flush()
                    r = subprocess.run(
                        cmd, cwd=wt, stdout=fh, stderr=subprocess.STDOUT, check=False
                    )
            finally:
                self.end_turn()
            with open(log) as fh:
                tail = (fh.read().strip().splitlines() or [""])[-1][-160:]
            # A retry the same to the byte: the emulator is deterministic, so the check's failure is the audio's own
            same = (
                attempt == 2
                and r.returncode == 1
                and os.path.exists(tmp)
                and os.path.exists(first)
                and filecmp.cmp(tmp, first, shallow=False)
            )
            if (r.returncode == 0 or same) and os.path.exists(tmp):
                final = os.path.join(self.raw, capture.name + ".wav")
                os.replace(tmp, final)
                for other in capture.serves[1:]:
                    link = os.path.join(self.raw, other + ".wav")
                    if os.path.exists(link):
                        os.replace(link, link + ".old")
                    os.link(final, link)
                write_info(
                    self.raw,
                    capture,
                    self.tempos[capture.name],
                    seconds,
                    "delugemu",
                    dict(
                        self.stamp(capture),
                        check=f"failed: {tail}",
                        quiet=list(capture.quiet),
                    )
                    if same
                    else self.stamp(capture),
                )
                if same:
                    self.say(
                        f"{capture.name}: kept, the retry is byte-identical, so the check's failure is in the audio "
                        f"itself (a riser from silence? --quiet-bars); listen before using it | {tail}"
                    )
                    with contextlib.suppress(FileNotFoundError):
                        os.remove(first)
                    return True
                self.say(
                    f"{capture.name}: ok, {seconds:g} s in {time.time() - t0:.0f} s, runner {index}, "
                    f"{capture.variant}, attempt {attempt} | {tail}"
                )
                return True
            self.say(
                f"{capture.name}: BAD rc={r.returncode} in {time.time() - t0:.0f} s, runner {index}, "
                f"attempt {attempt} | {tail}"
            )
            if os.path.exists(tmp):
                os.makedirs(bad, exist_ok=True)
                os.replace(
                    tmp, os.path.join(bad, f"{capture.name}-attempt{attempt}.wav")
                )
            if attempt == 1:
                time.sleep(RETRY_WAIT_S)
        with open(self.failed_marker(capture), "w") as fh:
            json.dump(self.failed_stamp(capture), fh)
        return False

    def runner(self, index, groups):
        """Claims units (variants, or chunks of one) in `groups` order until none is left."""
        wt, applied = None, None
        try:
            for unit, captures in groups:
                todo = [c for c in captures if not self.done(c) and not self.failed(c)]
                if not todo or not self.claim(unit):
                    continue
                variant = captures[0].variant
                wt = wt or self.worktree(index)
                # a second chunk of the variant applied already: no relink
                if applied != variant:
                    _variants.revert(wt)
                    _variants.apply(wt, self.session, variant)
                    applied = variant
                self.say(f"runner {index}: {unit}, {len(todo)} captures")
                try:
                    for c in todo:
                        self.capture(index, wt, c)
                finally:
                    self.release(unit)
        finally:
            if wt:
                _variants.revert(wt)
            self.say(f"runner {index}: done")

    def run(self):
        """Runs the captures on self.runners threads. Returns the names of the failed captures."""
        os.makedirs(self.raw, exist_ok=True)
        self.head = self.repo_head()
        pending = self.pending()
        secs = {c.name: _plan.seconds(c.bars, self.tempos[c.name]) for c in pending}
        order = _plan.groups(pending, secs, self.runners)
        threads = [
            threading.Thread(target=self.runner, args=(i + 1, order))
            for i in range(self.runners)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        return [c for c in self.captures if not self.done(c)]


def delugemu_warnings():
    """gen_emu's warnings when the installed delugemu (its default paths) isn't the pinned one (delugemu.pin)."""
    gen_emu = _jobs.gen_emu
    installed = gen_emu.installed_versions(
        gen_emu.QEMU, os.path.join(gen_emu.HOME, "source", "delugemu")
    )
    return gen_emu.pin_warnings(gen_emu.read_pin(), installed)


def write_info(raw, capture, tempo, seconds, kind, stamp=None):
    """raw/<name>.json beside each WAV the capture serves: what the cutter and the page read, and the stamp done()
    compares (Run.stamp)."""
    for name in capture.serves:
        info = dict(
            stamp
            or {
                "variant": capture.variant,
                "args": " ".join(capture.job.capture_flags()),
            }
        )
        info.update(
            {"tempo": tempo, "seconds": seconds, "kind": kind, "capture": capture.name}
        )
        with open(os.path.join(raw, name + ".json"), "w") as fh:
            json.dump(info, fh, indent=1)


def read_info(raw, name):
    try:
        with open(os.path.join(raw, name + ".json")) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def is_current(info, stamp, seconds):
    """Whether a WAV's sidecar `info` shows it was captured as `stamp` says (Run.stamp), for `seconds` or more."""
    if not info:
        return False
    return (
        all(info.get(k) == v for k, v in stamp.items())
        and info.get("seconds", 0) >= seconds - 1e-6
    )


def emulators(exclude_children_of=None):
    """The qemu-system processes on the host, any session's, less those under process `exclude_children_of`."""
    out = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,command="],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    procs = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            procs.append((int(parts[0]), int(parts[1]), parts[2]))
    mine = set()
    if exclude_children_of is not None:
        mine = {exclude_children_of}
        grew = True
        while grew:
            grew = False
            for pid, ppid, _ in procs:
                if ppid in mine and pid not in mine:
                    mine.add(pid)
                    grew = True
    return sum(
        1
        for pid, _, cmd in procs
        if "qemu-system" in cmd.split(" ", 1)[0] and pid not in mine
    )
