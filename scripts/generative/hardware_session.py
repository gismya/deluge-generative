"""`deluge_probe.py session`: one hardware run on the shared Deluge, with the hardware rules in code.

    ./dbt exec "python3 scripts/generative/deluge_probe.py session --build relwithdebinfo SCRIPT..."
    ./dbt exec "python3 scripts/generative/deluge_probe.py session --no-load SCRIPT..."

In order:
1. Refuses to start while the hung marker exists: prints it, adds this run to pending.md, exits 3. Run the scripts in
   delugemu instead (test_port_emu.py). Only the user can power cycle; `session --clear-hung` moves the marker away
   once they have.
2. Takes the lock (a directory, made with mkdir): waits up to --wait seconds for it, or fails at once with --no-wait.
   The lock is always released by moving it into released/, never deleted, also on errors and Ctrl-C.
3. Unless --no-load: checks the build has both ENABLE_SYSEX_TEST and ENABLE_SYSEX_LOAD, prints a one-line note when the
   running build was RAM-loaded (loads over one have hung; loading anyway only risks a power cycle), then
   `./dbt loadfw BUILD`.
4. Waits for the test port (`ready`), on a fresh boot after a load. No answer: writes the hung marker, exits 2.
5. Runs every script on that one load, printing "SCRIPT: N expects, M failed" and a summary. A script that gets no
   reply from the test port writes the hung marker and stops the run (exit 2). Any failure exits 1.

State lives in ~/.cache/deluge-generative/hardware/ (or $DELUGE_HARDWARE_DIR): lock/ (owner inside), hung, pending.md,
loaded.json (the last load this script saw) and released/.
"""

import datetime
import json
import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DIR_ENV = "DELUGE_HARDWARE_DIR"
DEFAULT_DIR = os.path.join(
    os.path.expanduser("~"), ".cache", "deluge-generative", "hardware"
)

LOCK_POLL = 10.0  # s between looks at a held lock
# A lock with no owner file this old is stale: acquire() writes the file right after the mkdir.
OWNERLESS_STALE_SECONDS = 60
SAMPLE_RATE = 44100  # audioSampleTimer ticks a second
FRESH_BOOT_SECONDS = (
    30  # a build that answers with less uptime than this booted after the load
)
TIMER_WRAP_SECONDS = (
    2**32 / SAMPLE_RATE
)  # audioSampleTimer is 32 bits: it wraps after about 27 h
UPTIME_SLACK_SECONDS = (
    120  # clock and query slack when comparing uptime with the time since the last load
)
# The dev-sysex key on the desk's Deluge (SETTINGS > COMMUNITY FEATURES > ALLOW INSECURE DEVELOP SYSEX MESSAGES);
# loadfw reads it from .deluge_hex_key, which git ignores, and stops at a prompt without it.
DEV_SYSEX_KEY = "4D750EA4"
NO_REPLY = "no reply to test port command"

EXIT_FAILED, EXIT_HUNG, EXIT_REFUSED = 1, 2, 3


def now_text():
    return datetime.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
    ).stdout.strip()


def build_identity():
    """The checkout's commit (with -dirty for local changes) and branch."""
    sha = git("rev-parse", "--short=9", "HEAD") or "unknown"
    if git("status", "--porcelain", "--untracked-files=no"):
        sha += "-dirty"
    return sha, git("rev-parse", "--abbrev-ref", "HEAD") or "unknown"


class Hardware:
    """The files that coordinate agents sharing the one Deluge."""

    def __init__(self, root=None):
        self.root = root or os.environ.get(DIR_ENV) or DEFAULT_DIR
        self.lock = os.path.join(self.root, "lock")
        self.hung = os.path.join(self.root, "hung")
        self.pending = os.path.join(self.root, "pending.md")
        self.loaded = os.path.join(self.root, "loaded.json")
        self.released = os.path.join(self.root, "released")
        self.owned = False

    def _ensure(self):
        os.makedirs(self.released, exist_ok=True)

    def _move_to_released(self, path, label):
        self._ensure()
        stamp = datetime.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
        target = os.path.join(self.released, f"{stamp}-{label}-{os.getpid()}")
        n = 1
        while os.path.exists(target):
            n += 1
            target = os.path.join(self.released, f"{stamp}-{label}-{os.getpid()}-{n}")
        os.rename(path, target)  # mv, never rm: rm is denied by policy
        return target

    # -- hung marker --

    def hung_text(self):
        if not os.path.exists(self.hung):
            return None
        with open(self.hung) as fh:
            return fh.read().strip() or "(empty marker)"

    def write_hung(self, what, build, scripts):
        self._ensure()
        sha, branch = build_identity()
        with open(self.hung, "w") as fh:
            fh.write(
                f"{what}\nwhen: {now_text()}\nbuild: {build or 'not loaded by this run'} from {sha} ({branch})\n"
                f"scripts: {' '.join(scripts)}\nwritten by: deluge_probe.py session, pid {os.getpid()} on "
                f"{socket.gethostname()}\nThe user power cycles the Deluge, then runs `deluge_probe.py session "
                "--clear-hung`.\n"
            )

    def clear_hung(self):
        if not os.path.exists(self.hung):
            return None
        return self._move_to_released(self.hung, "hung")

    # -- pending.md --

    def add_pending(self, reason, build, scripts, note=None):
        """Appends the run to pending.md, once: an entry for the same commit, build and scripts isn't repeated."""
        self._ensure()
        sha, branch = build_identity()
        key = f"<!-- {sha} {build or '-'} {' '.join(scripts)} -->"
        if os.path.exists(self.pending):
            with open(self.pending) as fh:
                if key in fh.read():
                    return False
        command = './dbt exec "python3 scripts/generative/deluge_probe.py session {}{}"'.format(
            f"--build {build} " if build else "--no-load ", " ".join(scripts)
        )
        lines = [
            f"## {now_text()}: {branch} at {sha}",
            key,
            f"- Why skipped: {reason}",
            f"- Build: `{build or 'whatever is running'}`, configured with -DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON",
            f"- Run: `{command}`",
            "- Passing looks like: every script prints `N expects, 0 failed` and the session exits 0",
        ]
        if note:
            lines.append(f"- Also: {note}")
        with open(self.pending, "a") as fh:
            fh.write("\n".join(lines) + "\n\n")
        return True

    # -- lock --

    def owner_text(self):
        try:
            with open(os.path.join(self.lock, "owner")) as fh:
                return fh.read().strip()
        except OSError:
            return "(no owner file)"

    def _stale(self, now=None):
        """Whether the lock's owner was a process on this host that has exited, or the lock has had no owner file for
        over OWNERLESS_STALE_SECONDS (its taker died between the mkdir and writing the file)."""
        if not os.path.exists(os.path.join(self.lock, "owner")):
            try:
                made = os.stat(self.lock).st_mtime
            except OSError:
                return False  # gone already: the next mkdir takes it
            return (
                time.time() if now is None else now
            ) - made > OWNERLESS_STALE_SECONDS
        fields = dict(
            line.split("=", 1) for line in self.owner_text().splitlines() if "=" in line
        )
        if (
            fields.get("host") != socket.gethostname()
            or not fields.get("pid", "").isdigit()
        ):
            return False
        try:
            os.kill(int(fields["pid"]), 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            return False
        return False

    def acquire(self, owner, wait_seconds, out=print, poll=LOCK_POLL):
        """Takes the lock, waiting up to wait_seconds (0: fail at once). Returns whether it's ours."""
        os.makedirs(self.root, exist_ok=True)
        deadline = time.monotonic() + wait_seconds
        told = False
        while True:
            try:
                os.mkdir(self.lock)
            except FileExistsError:
                if self._stale():
                    moved = self._move_to_released(self.lock, "stale-lock")
                    out(
                        f"session: the lock's owner has exited; moved its lock to {moved}"
                    )
                    continue
                if time.monotonic() >= deadline:
                    out(
                        f"session: the Deluge is in use (lock {self.lock}):\n{self.owner_text()}"
                    )
                    return False
                if not told:
                    out(
                        f"session: waiting up to {wait_seconds:g} s for the lock, held by:\n{self.owner_text()}"
                    )
                    told = True
                time.sleep(min(poll, max(0.0, deadline - time.monotonic())))
                continue
            self.owned = True
            with open(os.path.join(self.lock, "owner"), "w") as fh:
                fh.write(
                    f"pid={os.getpid()}\nhost={socket.gethostname()}\nuser={os.environ.get('USER', '?')}\n"
                    f"since={now_text()}\ncwd={os.getcwd()}\n{owner}\n"
                )
            return True

    def release(self):
        if self.owned and os.path.isdir(self.lock):
            self._move_to_released(self.lock, "lock")
        self.owned = False

    # -- the last load this script saw --

    def read_loaded(self):
        try:
            with open(self.loaded) as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return None

    def write_loaded(self, record):
        self._ensure()
        with open(self.loaded, "w") as fh:
            json.dump(record, fh, indent=2)


def ram_loaded_note(record, uptime_seconds, now=None):
    """One line about the running build, from the last load this script saw and the device's uptime (None if it
    didn't answer). "" when there's nothing to say."""
    now = time.time() if now is None else now
    if record is None:
        return ""
    since = now - record["loaded_at"]
    what = (
        f"{record.get('build')} from {record.get('sha')}, loaded {record.get('when')}"
    )
    if uptime_seconds is None:
        return f"note: the running build didn't answer; the last load this script saw was {what}"
    if since >= TIMER_WRAP_SECONDS:
        return f"note: can't tell whether the running build is RAM-loaded (last load {what}, over 27 h ago)"
    if uptime_seconds + UPTIME_SLACK_SECONDS >= since:
        return (
            f"note: loading over a RAM-loaded build ({what}); loads like this hung 3 times on 2026-11-06, "
            "a hang costs a power cycle"
        )
    return ""  # it has rebooted since: a power cycle back to the SD-flashed build


def check_build_config(build):
    """Why `build` can't be loaded, or None. Both options are needed: without ENABLE_SYSEX_LOAD the device ignores the
    next load until a power cycle, and without ENABLE_SYSEX_TEST nothing answers the scripts."""
    cache = os.path.join(REPO, "build", "CMakeCache.txt")
    if os.path.isfile(build):
        return None  # a binary path: trust the caller
    try:
        with open(cache) as fh:
            text = fh.read()
    except OSError:
        return f"no {cache}: configure and build first"
    missing = [
        opt
        for opt in ("ENABLE_SYSEX_TEST", "ENABLE_SYSEX_LOAD")
        if f"{opt}:BOOL=ON" not in text
    ]
    if missing:
        return (
            "build/ isn't configured with "
            + " and ".join(f"-D{m}=ON" for m in missing)
            + ": ./dbt configure -DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON && ./dbt build "
            + build
        )
    configs = {
        "release": "Release",
        "debug": "Debug",
        "relwithdebinfo": "RelWithDebInfo",
    }
    binary = os.path.join(
        REPO, "build", configs.get(build.lower(), build), "deluge.bin"
    )
    if not os.path.exists(binary):
        return f"no {binary}: ./dbt build {build}"
    return None


def loadfw(build, out=print):
    """./dbt loadfw BUILD, with its carriage-return progress bar trimmed. Returns the exit code."""
    key = os.path.join(REPO, ".deluge_hex_key")
    if not os.path.exists(key):
        with open(key, "w") as fh:
            fh.write(DEV_SYSEX_KEY + "\n")
        out(f"session: wrote {key} (the desk Deluge's dev-sysex key)")
    out(f"session: ./dbt loadfw {build}")
    proc = subprocess.run(
        [os.path.join(REPO, "dbt"), "loadfw", build],
        cwd=REPO,
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    lines = [
        line
        for line in (proc.stdout + proc.stderr).replace("\r", "\n").splitlines()
        if line.strip()
    ]
    for line in lines[-3:]:
        out("  " + line)
    return proc.returncode


class Session:
    """One run. The device-facing steps are methods, so tests can replace them."""

    def __init__(self, probe, args, hw=None, out=print):
        self.dp, self.args, self.out = probe, args, out
        self.hw = hw or Hardware()
        self.build = None if args.no_load else args.build
        self.scripts = list(args.args)

    # Device steps -------------------------------------------------------

    def open_link(self):
        return (
            self.dp.UdpLink(self.args.udp)
            if self.args.udp
            else self.dp.RtMidiLink(quiet=True)
        )

    def uptime(self):
        """The running build's uptime in seconds, or None if it doesn't answer the test port."""
        link = self.open_link()  # LookupError: no Deluge port at all
        try:
            return (
                self.dp.TestPort(link, self.dp.read_key(self.args)).state()[
                    "sample_timer"
                ]
                / SAMPLE_RATE
            )
        except (TimeoutError, RuntimeError):
            return None
        finally:
            link.close()

    def load(self):
        return loadfw(self.build, self.out)

    def wait_ready(self, fresh):
        """The TestPort once the firmware answers and has booted (after a load: on a fresh boot), or None."""
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            try:
                port = self.dp.wait_ready(
                    self.open_link,
                    self.dp.read_key(self.args),
                    deadline - time.monotonic(),
                )
            except TimeoutError:
                return None
            try:
                state = port.state()
            except Exception as exc:
                # It answered once, then stopped: keep trying until the deadline.
                if not self.dp.link_failed(exc):
                    raise
                port.link.close()
                time.sleep(0.5)
                continue
            if not fresh or state["sample_timer"] < FRESH_BOOT_SECONDS * SAMPLE_RATE:
                self.out(f"session: ready, sample_timer {state['sample_timer']}")
                return port
            port.link.close()  # still the old build, about to restart
            time.sleep(0.5)
        return None

    def run_one(self, port, script):
        lines = self.dp.read_script(script)
        port.subscribe()
        try:
            return self.dp.run_script(
                port, lines, out=lambda line: self.out(f"  {line}")
            )
        finally:
            port.subscribe(False)

    # The rules ----------------------------------------------------------

    def hung(self, what):
        self.hw.write_hung(what, self.build, self.scripts)
        self.hw.add_pending(
            f"the Deluge hung: {what}", self.build, self.scripts, self.args.note
        )
        self.out(f"session: HUNG: {what}")
        self.out(
            f"session: wrote {self.hw.hung}; stop using the Deluge. Run the same scripts in delugemu:"
        )
        self.out("  " + emu_command(self.scripts))
        self.out(
            f"session: the run is in {self.hw.pending} for when the user has power cycled"
        )
        return EXIT_HUNG

    def refuse_if_hung(self):
        text = self.hw.hung_text()
        if text is None:
            return None
        self.out(f"session: the Deluge is marked hung ({self.hw.hung}):")
        self.out("\n".join("  " + line for line in text.splitlines()))
        self.out(
            "session: don't touch the device. Run the same scripts in delugemu instead:"
        )
        self.out("  " + emu_command(self.scripts))
        if self.hw.add_pending(
            "the hung marker was set", self.build, self.scripts, self.args.note
        ):
            self.out(f"session: added this run to {self.hw.pending}")
        else:
            self.out(f"session: this run is already in {self.hw.pending}")
        return EXIT_REFUSED

    def run(self):
        refused = self.refuse_if_hung()
        if refused:
            return refused
        sha, branch = build_identity()
        owner = f"build={self.build or '(no load)'}\nsha={sha}\nbranch={branch}\nscripts={' '.join(self.scripts)}"
        if self.args.owner:
            owner += f"\nnote={self.args.owner}"
        if not self.hw.acquire(
            owner, 0 if self.args.no_wait else self.args.wait, self.out
        ):
            return EXIT_FAILED
        try:
            return self._run_locked(sha)
        except KeyboardInterrupt:
            self.out("session: interrupted")
            return 130
        finally:
            self.hw.release()
            self.out("session: lock released")

    def _run_locked(self, sha):
        refused = self.refuse_if_hung()  # it may have appeared while we waited
        if refused:
            return refused
        if self.build:
            problem = check_build_config(self.build)
            if problem:
                self.out("session: " + problem)
                return EXIT_FAILED
            try:
                uptime = self.uptime()
            except LookupError as exc:
                self.out(f"session: {exc}")
                return EXIT_FAILED
            note = ram_loaded_note(self.hw.read_loaded(), uptime)
            if note:
                self.out(note)
            code = self.load()
            if code != 0:
                self.out(
                    f"session: loadfw exited {code}; waiting for the test port anyway"
                )
        port = self.wait_ready(fresh=bool(self.build))
        if port is None:
            what = f"no answer from the test port within {self.args.timeout:g} s" + (
                f" after loadfw {self.build}" if self.build else ""
            )
            return self.hung(what)
        if self.build:
            self.hw.write_loaded(
                {
                    "build": self.build,
                    "sha": sha,
                    "when": now_text(),
                    "loaded_at": time.time(),
                }
            )
        results = []
        try:
            for script in self.scripts:
                name = os.path.basename(script)
                self.out(f"session: {name}")
                try:
                    failed, total = self.run_one(port, script)
                    results.append((name, failed, total, None))
                except TimeoutError as exc:
                    if NO_REPLY in str(exc):
                        results.append((name, 1, 0, str(exc)))
                        self.summary(results)
                        return self.hung(f"{exc} during {name}")
                    raise
                except (RuntimeError, ValueError, OSError) as exc:
                    results.append((name, 1, 0, str(exc)))
                self.out("session: " + self.dp.result_line(*results[-1]))
        finally:
            port.link.close()
        return self.summary(results)

    def summary(self, results):
        self.out("session: summary")
        for result in results:
            self.out("  " + self.dp.result_line(*result))
        bad = sum(1 for r in results if r[1])
        self.out(f"session: {len(results)} scripts, {bad} with failures")
        return EXIT_FAILED if bad else 0


def emu_command(scripts):
    return (
        "python3 scripts/generative/test_port_emu.py --build --out <scratch dir> "
        + " ".join(f"--script {s}" for s in scripts)
    )


def _interrupt(signum, frame):
    raise KeyboardInterrupt


def main(probe, args, out=print):
    hw = Hardware()
    if args.clear_hung:
        moved = hw.clear_hung()
        out(
            f"session: moved the hung marker to {moved}"
            if moved
            else "session: no hung marker"
        )
        return 0
    if not args.args:
        out("session: name at least one script")
        return EXIT_FAILED
    if not args.no_load and not args.build:
        out(
            "session: say which build to load (--build relwithdebinfo), or --no-load to use the running one"
        )
        return EXIT_FAILED
    if args.udp and not args.no_load:
        out("session: --udp (delugemu) needs --no-load")
        return EXIT_FAILED
    for script in args.args:
        if not os.path.isfile(script):
            out(f"session: no script {script}")
            return EXIT_FAILED
    signal.signal(
        signal.SIGINT, _interrupt
    )  # also when started in the background, where SIGINT is ignored
    signal.signal(signal.SIGTERM, _interrupt)
    signal.signal(signal.SIGHUP, _interrupt)
    return Session(probe, args, hw, out).run()


if __name__ == "__main__":
    sys.exit("run it as: deluge_probe.py session ...")
