"""The capture runners: N at once, each with its own worktree, --build-dir and --out folder, all under the scratch
folder. gen_emu.py stays the only thing that boots delugemu.

<scratch>/
  card_<card>.img       the card images the jobs name
  wt/r<N>/              runner N's worktree (a detached worktree of the repo's HEAD, made on first use)
  build/r<N>/           its capture build dir
  out/r<N>/             its captures in progress
  raw/<name>.wav        finished captures; a merged capture is hard-linked under each job name it serves
  raw/<name>.log        gen_emu's log of each attempt
  raw/locks/<variant>.lock   the runner that claimed a variant's captures (its pid)
  raw/<name>.failed     a capture that failed twice
  pause                 while it exists, no capture starts
  run.log

A runner claims a whole variant at a time (costliest first), applies its diff once, takes its captures in Recipe
order, and reverts after. Before any capture, every job passes the Recipe check (recipe.py). A failed capture is
retried once; before each capture the runner waits while the pause file exists or the host runs `cap` or more
emulators (qemu-system processes, any session's).
"""

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

    def plan(self):
        return _plan.plan(self.captures, self.runners, lambda c: self.tempos[c.name])

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

    def done(self, capture):
        return all(
            os.path.exists(os.path.join(self.raw, n + ".wav")) for n in capture.serves
        )

    def failed(self, capture):
        return os.path.exists(os.path.join(self.raw, capture.name + ".failed"))

    def claim(self, variant):
        """Takes variant's lock, or one whose pid is gone. True if this process holds it."""
        locks = os.path.join(self.raw, "locks")
        os.makedirs(locks, exist_ok=True)
        path = os.path.join(locks, variant.replace(os.sep, "_") + ".lock")
        for _ in range(2):
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                try:
                    with open(path) as fh:
                        pid = int(fh.read().strip() or 0)
                except (OSError, ValueError):
                    pid = 0
                if pid and (pid == os.getpid() or _jobs.gen_emu.pid_running(pid)):
                    return False
                os.replace(path, path + ".stale")  # its runner died
                continue
            with os.fdopen(fd, "w") as fh:
                fh.write(str(os.getpid()))
            return True
        return False

    def wait_turn(self):
        pause = os.path.join(self.scratch, "pause")
        said = False
        while os.path.exists(pause) or emulators() >= self.cap:
            if not said:
                self.say(
                    f"waiting: pause={os.path.exists(pause)} emulators={emulators()} (cap {self.cap})"
                )
                said = True
            time.sleep(POLL_S)

    def worktree(self, index):
        wt = os.path.join(self.scratch, "wt", f"r{index}")
        if not os.path.exists(wt):
            subprocess.run(
                [
                    "git",
                    "-C",
                    self.repo,
                    "worktree",
                    "add",
                    "--detach",
                    "-q",
                    wt,
                    "HEAD",
                ],
                check=True,
            )
        return wt

    def capture(self, index, wt, capture):
        seconds = _plan.seconds(capture.bars, self.tempos[capture.name])
        own = os.path.join(self.scratch, "out", f"r{index}")
        os.makedirs(own, exist_ok=True)
        tmp = os.path.join(own, capture.name + ".wav")
        cmd = [
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
        ]
        for attempt in (1, 2):
            self.wait_turn()
            t0 = time.time()
            log = os.path.join(self.raw, f"{capture.name}.log")
            with open(log, "a") as fh:
                fh.write(f"== attempt {attempt}: {' '.join(cmd)}\n")
                fh.flush()
                r = subprocess.run(
                    cmd, cwd=wt, stdout=fh, stderr=subprocess.STDOUT, check=False
                )
            with open(log) as fh:
                tail = (fh.read().strip().splitlines() or [""])[-1][-160:]
            if r.returncode == 0 and os.path.exists(tmp):
                final = os.path.join(self.raw, capture.name + ".wav")
                os.replace(tmp, final)
                for other in capture.serves[1:]:
                    link = os.path.join(self.raw, other + ".wav")
                    if os.path.exists(link):
                        os.replace(link, link + ".old")
                    os.link(final, link)
                write_info(
                    self.raw, capture, self.tempos[capture.name], seconds, "delugemu"
                )
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
                bad = os.path.join(self.raw, "bad")
                os.makedirs(bad, exist_ok=True)
                os.replace(
                    tmp, os.path.join(bad, f"{capture.name}-attempt{attempt}.wav")
                )
            if attempt == 1:
                time.sleep(RETRY_WAIT_S)
        open(os.path.join(self.raw, capture.name + ".failed"), "w").close()
        return False

    def runner(self, index, groups):
        """Claims variants in `groups` order until none is left."""
        wt = None
        try:
            for variant, captures in groups:
                todo = [c for c in captures if not self.done(c) and not self.failed(c)]
                if not todo or not self.claim(variant):
                    continue
                wt = wt or self.worktree(index)
                _variants.revert(wt)
                _variants.apply(wt, self.session, variant)
                self.say(f"runner {index}: {variant}, {len(todo)} captures")
                for c in todo:
                    self.capture(index, wt, c)
        finally:
            if wt:
                _variants.revert(wt)
            self.say(f"runner {index}: done")

    def run(self):
        """Runs the captures on self.runners threads. Returns the names of the failed captures."""
        os.makedirs(self.raw, exist_ok=True)
        secs = {
            c.name: _plan.seconds(c.bars, self.tempos[c.name]) for c in self.captures
        }
        order = _plan.groups(self.captures, secs)
        threads = [
            threading.Thread(target=self.runner, args=(i + 1, order))
            for i in range(self.runners)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        return [c.name for c in self.captures if not self.done(c)]


def write_info(raw, capture, tempo, seconds, kind):
    """raw/<name>.json beside each WAV the capture serves: what the cutter and the page read."""
    for name in capture.serves:
        info = {
            "tempo": tempo,
            "seconds": seconds,
            "variant": capture.variant,
            "card": capture.job.card,
            "args": " ".join(capture.job.capture_flags()),
            "kind": kind,
            "capture": capture.name,
        }
        with open(os.path.join(raw, name + ".json"), "w") as fh:
            json.dump(info, fh, indent=1)


def emulators():
    """The qemu-system processes on the host, any session's."""
    out = subprocess.run(
        ["ps", "-axo", "command"], capture_output=True, text=True, check=False
    ).stdout
    return sum(1 for line in out.splitlines() if "qemu-system" in line.split(" ", 1)[0])
