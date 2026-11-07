"""`deluge_probe.py session`: one hardware run on the shared Deluge, with the hardware rules in code.

    ./dbt exec "python3 scripts/generative/deluge_probe.py session --build hardware SCRIPT..."
    ./dbt exec "python3 scripts/generative/deluge_probe.py session --no-load SCRIPT..."

In order:
1. Refuses to start while the hung marker exists: prints it, adds this run to pending.md, exits 3. Run the scripts in
   delugemu instead (test_port_emu.py). Only the user can power cycle; `session --clear-hung` moves the marker away
   once they have. With --build hardware, then builds build-hardware/ (Release with both options, see HARDWARE)
   before taking the lock. Another --build (release, or a deluge.bin path) is loaded as it is.
2. Takes the lock (a directory, made with mkdir): waits up to --wait seconds for it, or fails at once with --no-wait.
   The lock is always released by moving it into released/, never deleted, also on errors and Ctrl-C.
3. Unless --no-load: checks the build has both ENABLE_SYSEX_TEST and ENABLE_SYSEX_LOAD, prints a one-line note when the
   running build was RAM-loaded (loads over one have hung; loading anyway only risks a power cycle), then
   `./dbt loadfw BUILD`.
4. Waits for the test port (`ready`), after a load on a fresh boot: younger than the build that ran before (or, if
   none answered before the load, young). No answer: writes the hung marker, exits 2. The old build answering near
   the end of the wait (STILL_ANSWERS_SECONDS), with no fresh one, means the load didn't take: that exits 1, with no
   marker. Silence for the rest of the wait is a hang (exit 2), whatever answered before it.
5. Runs every script on that one load, printing "SCRIPT: N expects, M failed" and a summary. With --events PATTERN,
   it also prints the script's test events that match the regex after each script (`--events 'gen_mem|gen_left'`,
   `--events .` for all), counted from after its first start_state (a script should have only one, counting its
   include files). A script that gets no reply from the test port writes the hung marker and stops the run (exit 2).
   Any failure exits 1.

State lives in ~/.cache/deluge-generative/hardware/ (or $DELUGE_HARDWARE_DIR): lock/ (owner inside), hung, pending.md,
loaded.json (the last load this script saw) and released/.
"""

import datetime
import json
import os
import re
import shlex
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
# A lock with no owner file, or a lock.take, this old is stale: see ownerless_stale().
OWNERLESS_STALE_SECONDS = 60
SAMPLE_RATE = 44100  # audioSampleTimer ticks a second
# With no uptime from before the load to compare with, a build that answers with less uptime than this booted after
# the load.
FRESH_BOOT_SECONDS = 30
TIMER_WRAP_SECONDS = (
    2**32 / SAMPLE_RATE
)  # audioSampleTimer is 32 bits: it wraps after about 27 h
UPTIME_SLACK_SECONDS = (
    120  # clock and query slack when comparing uptime with the time since the last load
)
READY_POLL_SECONDS = 0.5  # between test port queries while waiting for a fresh boot
# The late-answer window, defined here only: an answer from the old build this close to the end of the wait for a
# fresh boot means it still runs (the load didn't take). One further back may have come just before it restarted into
# a build that then hung. STILL_ANSWERS_SECONDS, or STILL_ANSWERS_FRACTION of a shorter --timeout, but never under
# two polls, so the old build's last answer before the deadline always falls inside it.
STILL_ANSWERS_SECONDS = 5
STILL_ANSWERS_FRACTION = 1 / 4
MIN_LOAD_TIMEOUT = (
    4 * READY_POLL_SECONDS
)  # the shortest --timeout a load takes: see main()
# How far below the old build's uptime the new one's must be, so a query's latency can't make the old build look new.
# A load takes longer than this, so a new build is always further below.
FRESH_SLACK_SECONDS = 2
# The dev-sysex key on the desk's Deluge (SETTINGS > COMMUNITY FEATURES > ALLOW INSECURE DEVELOP SYSEX MESSAGES);
# loadfw reads it from .deluge_hex_key, which git ignores, and stops at a prompt without it.
DEV_SYSEX_KEY = "4D750EA4"
NO_REPLY = "no reply to test port command"

EXIT_FAILED, EXIT_HUNG, EXIT_REFUSED = 1, 2, 3


def still_answers_window(timeout):
    """The late-answer window for a wait of `timeout` seconds: see STILL_ANSWERS_SECONDS."""
    return min(
        STILL_ANSWERS_SECONDS,
        max(timeout * STILL_ANSWERS_FRACTION, 2 * READY_POLL_SECONDS),
    )


class LoadDidNotTake(Exception):
    """After a load, the build that was running still answered near the end of the wait for a fresh boot."""

    def __init__(self, uptime):
        super().__init__(
            f"the build that was running still answers (uptime {uptime:.0f} s)"
        )
        self.uptime = uptime


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
        self.take_lock = os.path.join(self.root, "lock.take")
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
        """Whether the lock's owner was a process on this host that has exited, or the lock is ownerless_stale() (its
        taker died between the mkdir and writing the owner file)."""
        if not os.path.exists(os.path.join(self.lock, "owner")):
            try:
                return ownerless_stale(os.stat(self.lock), now)
            except OSError:
                return False  # gone already: the next mkdir takes it
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

    def _take_stale(self, out):
        """Moves the lock into released/ if it's stale. Returns whether the lock may have gone, so it's worth trying
        the mkdir again.

        Sessions that find a stale lock take turns: each holds `take_lock` (a second mkdir lock) while it judges the
        lock again and moves it, so one can't move the live lock another has just taken in its place. A take_lock
        older than OWNERLESS_STALE_SECONDS was left by a session that died holding it, and is moved away. Each
        directory's inode tells it from one made since in its place: the moved one still exists in released/, so a
        new one never reuses its inode."""
        if not self._stale():
            return False
        try:
            os.mkdir(self.take_lock)
            ours = os.stat(self.take_lock).st_ino
        except FileExistsError:
            return self._move_dead_take(out)
        except FileNotFoundError:
            return True  # moved as dead between the mkdir and the stat: try again
        try:
            return self._move_stale_lock(out)
        finally:
            self._release_take(ours)

    def _move_stale_lock(self, out):
        """_take_stale()'s work under take_lock: judges the lock again and moves it if it's still stale. If the lock
        was replaced anyway (its taker gave up after OWNERLESS_STALE_SECONDS, and a new one took it), the move
        caught a live lock, so it goes back."""
        try:
            judged = os.stat(self.lock).st_ino
            if not self._stale():
                # Another session took it while this one waited for take_lock.
                return False
            moved = self._move_unless_replaced(self.lock, judged, "stale-lock", out)
        except FileNotFoundError:
            return True  # another session moved it first
        if moved:
            out(f"session: the lock's owner has exited; moved its lock to {moved}")
        return True

    def _release_take(self, inode):
        """Moves take_lock into released/ if it's still the one with `inode`: if it was moved as dead meanwhile, a
        take_lock there now is another session's."""
        try:
            if os.stat(self.take_lock).st_ino == inode:
                self._move_to_released(self.take_lock, "take")
        except FileNotFoundError:
            pass

    def _move_dead_take(self, out):
        """Moves take_lock away if its session died holding it. Returns whether it's gone. If another session
        replaced it just before the move, the move caught that live one (the inode tells), so it goes back."""
        try:
            judged = os.stat(self.take_lock)
            if not ownerless_stale(judged):
                return False
            moved = self._move_unless_replaced(
                self.take_lock, judged.st_ino, "stale-take", out
            )
        except FileNotFoundError:
            return True
        if not moved:
            return False
        out(
            f"session: a session died while taking a stale lock; moved its {self.take_lock} to {moved}"
        )
        return True

    def _move_unless_replaced(self, path, inode, label, out):
        """Moves the directory `path` into released/ if it's still the one with `inode`, and returns where it went.
        If another session replaced it since it was judged, the move caught that live one: it goes back, and this
        returns None. Raises FileNotFoundError if `path` is gone."""
        moved = self._move_to_released(path, label)
        if os.stat(moved).st_ino == inode:
            return moved
        try:
            os.rename(moved, path)
        except OSError as exc:
            out(
                f"session: moved a live {path} to {moved} by mistake and couldn't put it back ({exc}); "
                "its owner still holds it"
            )
        return None

    def acquire(self, owner, wait_seconds, out=print, poll=LOCK_POLL):
        """Takes the lock, waiting up to wait_seconds (0: fail at once). Returns whether it's ours."""
        os.makedirs(self.root, exist_ok=True)
        deadline = time.monotonic() + wait_seconds
        told = False
        while True:
            try:
                os.mkdir(self.lock)
            except FileExistsError:
                if self._take_stale(out):
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
            try:
                # "x": a lock put back over ours (see _take_stale) already has its owner's file.
                with open(os.path.join(self.lock, "owner"), "x") as fh:
                    fh.write(
                        f"pid={os.getpid()}\nhost={socket.gethostname()}\nuser={os.environ.get('USER', '?')}\n"
                        f"since={now_text()}\ncwd={os.getcwd()}\n{owner}\n"
                    )
            except (FileNotFoundError, FileExistsError):
                continue  # moved or replaced since the mkdir: not ours
            self.owned = True
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


def ownerless_stale(stat, now=None):
    """Whether a lock directory (lock/ with no owner file, or lock.take) with this os.stat() was left by a session
    that died holding it: one that lives writes its owner file, or finishes with lock.take, well within
    OWNERLESS_STALE_SECONDS of the mkdir."""
    return (
        time.time() if now is None else now
    ) - stat.st_mtime > OWNERLESS_STALE_SECONDS


def fresh_boot(sample_timer, before=None, now=None):
    """Whether a build answering with `sample_timer` booted after the load. `before` is (uptime in seconds,
    time.monotonic() when read) from the build running before the load, or None if it didn't answer.

    With `before`, the uptime must be below what the old build's would be by now, as its 32-bit timer reads it (an
    old build up for 27 h has wrapped round to a young uptime). Being young isn't enough there: a load that fails its
    checksum leaves the old build running, and it may have booted only seconds before. Nor is it needed: a new build
    that loads a long boot song first answers well past FRESH_BOOT_SECONDS. It must also be no older than the time
    since `before` was read, as a build that booted after it is: an old build whose timer fell behind during the
    transfer can read below what its uptime should be by now. With no `before`, being young is all there is to go
    on."""
    seconds = sample_timer / SAMPLE_RATE
    if before is None:
        return seconds < FRESH_BOOT_SECONDS
    uptime, at = before
    since = (time.monotonic() if now is None else now) - at
    old_now = (uptime + since) % TIMER_WRAP_SECONDS
    return (
        seconds + FRESH_SLACK_SECONDS < old_now
        and seconds < since + FRESH_SLACK_SECONDS
    )


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


# `--build hardware`: the session builds this itself (Release, with both options) before it takes the lock, so build/
# keeps the default configuration that check_all.py and ./dbt sizediff need.
HARDWARE = "hardware"


def build_root(build):
    """The CMake build directory a build type lives in: build-hardware/ for HARDWARE, else build/."""
    return os.path.join(REPO, "build-hardware" if build == HARDWARE else "build")


def build_dir(build):
    """Where ./dbt build puts a build type's outputs."""
    configs = {
        "release": "Release",
        "debug": "Debug",
        "relwithdebinfo": "RelWithDebInfo",
        HARDWARE: "Release",
    }
    return os.path.join(build_root(build), configs.get(build.lower(), build))


def binary_for(build):
    """The deluge.bin `build` names: a build type, or a path to the binary itself."""
    if os.path.isfile(build):
        return build
    return os.path.join(build_dir(build), "deluge.bin")


def configure_and_build(root, options, stdout=None):
    """Configures the CMake build directory `root` with `options` (e.g. ["-DENABLE_SYSEX_TEST=ON"]) if it has no
    CMakeCache.txt yet, then builds its Release firmware incrementally, both through ./dbt exec. Output goes to
    `stdout` (a file), or to this process's. Returns the exit code."""
    steps = []
    if not os.path.exists(os.path.join(root, "CMakeCache.txt")):
        steps.append(
            f"cmake -B {shlex.quote(root)} -S {shlex.quote(REPO)} -G 'Ninja Multi-Config' "
            "-DCMAKE_CROSS_CONFIGS:STRING=all '-DCMAKE_DEFAULT_CONFIGS=Debug;Release' "
            + " ".join(options)
        )
    steps.append(f"cmake --build {shlex.quote(root)} --config Release --target deluge")
    for step in steps:
        code = subprocess.run(
            [os.path.join(REPO, "dbt"), "exec", step],
            cwd=REPO,
            stdout=stdout,
            stderr=subprocess.STDOUT if stdout is not None else None,
            stdin=subprocess.DEVNULL,
            check=False,
        ).returncode
        if code:
            return code
    return 0


def build_hardware(out=print):
    """Builds build-hardware/ (see HARDWARE), logging to build-hardware/build.log. Returns the exit code."""
    root = build_root(HARDWARE)
    log = os.path.join(root, "build.log")
    out(f"session: building {build_dir(HARDWARE)} (log: {log})")
    os.makedirs(root, exist_ok=True)
    with open(log, "w") as fh:
        code = configure_and_build(
            root, ["-DENABLE_SYSEX_TEST=ON", "-DENABLE_SYSEX_LOAD=ON"], fh
        )
    if code:
        with open(log, errors="replace") as fh:
            tail = fh.read().replace("\r", "\n").splitlines()[-15:]
        out("\n".join("  " + line for line in tail))
        out(f"session: the build failed (exit {code})")
    return code


def check_build_config(build):
    """Why `build` can't be loaded, or None. Both options are needed: without ENABLE_SYSEX_LOAD the device ignores the
    next load until a power cycle, and without ENABLE_SYSEX_TEST nothing answers the scripts."""
    cache = os.path.join(build_root(build), "CMakeCache.txt")
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
            f"{os.path.relpath(build_root(build), REPO)}/ isn't configured with "
            + " and ".join(f"-D{m}=ON" for m in missing)
            + f": use --build {HARDWARE}, which builds build-hardware/ with both"
        )
    binary = binary_for(build)
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
    target = binary_for(build) if build == HARDWARE else build
    out(f"session: ./dbt loadfw {target}")
    proc = subprocess.run(
        [os.path.join(REPO, "dbt"), "loadfw", target],
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

    def build_firmware(self):
        """Builds `--build hardware`. Returns the exit code."""
        return build_hardware(self.out)

    def wait_ready(self, fresh, before=None):
        """The TestPort once the firmware answers and has booted, or None if it's silent. With `fresh` (after a
        load), only a build that booted since: see fresh_boot(), which takes `before`.

        Raises LoadDidNotTake if no fresh build answered, but the old build's last answer came near the end of the
        wait (still_answers_window()): it still runs. Silence for the rest of the wait is a hang, whatever came
        before: the old build was about to restart into a build that never answered."""
        # The old build's last answer, and when.
        old_uptime = old_at = None
        window = still_answers_window(self.args.timeout)
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            try:
                port = self.dp.wait_ready(
                    self.open_link,
                    self.dp.read_key(self.args),
                    deadline - time.monotonic(),
                )
            except TimeoutError:
                break  # nothing answered for the rest of the wait
            try:
                state = port.state()
            except Exception as exc:
                # It answered once, then stopped: keep trying until the deadline.
                if not self.dp.link_failed(exc):
                    raise
                port.link.close()
                time.sleep(READY_POLL_SECONDS)
                continue
            if not fresh or fresh_boot(state["sample_timer"], before):
                self.out(f"session: ready, sample_timer {state['sample_timer']}")
                return port
            # The old build: about to restart, or the load didn't take.
            old_uptime, old_at = state["sample_timer"] / SAMPLE_RATE, time.monotonic()
            port.link.close()
            time.sleep(READY_POLL_SECONDS)
        if old_at is not None and deadline - old_at <= window:
            raise LoadDidNotTake(old_uptime)
        return None

    def run_one(self, port, script):
        lines = self.dp.read_script(script)
        seen = len(port.events)
        starts = []  # where port.events stood after each start_state
        port.subscribe()
        try:
            return self.dp.run_script(
                port,
                lines,
                out=lambda line: self.out(f"  {line}"),
                on_start_state=starts.append,
            )
        finally:
            # From after the first start_state (a script should have only one): what it caused isn't the script's.
            self.print_events(port.events[starts[0] if starts else seen :])
            try:
                port.subscribe(False)
            except Exception as exc:
                # A device that went away: the script's own error (a no-reply hang) is the one to report.
                if not self.dp.link_failed(exc):
                    raise

    def print_events(self, events):
        """With --events PATTERN: the script's test events that match the regex."""
        if self.args.events is None:
            return
        pattern = re.compile(self.args.events)
        for line in events:
            if pattern.search(line):
                self.out(f"  event {line}")

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
        # Built before the lock, so other agents don't wait on a compile.
        if self.build == HARDWARE and self.build_firmware():
            return EXIT_FAILED
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
        before = None
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
            before = None if uptime is None else (uptime, time.monotonic())
            note = ram_loaded_note(self.hw.read_loaded(), uptime)
            if note:
                self.out(note)
            code = self.load()
            if code != 0:
                # The old build may still be up: if it answers, the device didn't hang, so no marker.
                port = self.wait_ready(fresh=False)
                if port is not None:
                    port.link.close()
                    self.out(
                        f"session: loadfw exited {code} and the running build still answers; nothing was loaded"
                    )
                    return EXIT_FAILED
        try:
            port = self.wait_ready(fresh=bool(self.build), before=before)
        except LoadDidNotTake as exc:
            # It answered to the end, so it didn't hang: no marker.
            self.out(f"session: {exc}; the load didn't take")
            return EXIT_FAILED
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
            "session: say which build to load (--build release), or --no-load to use the running one"
        )
        return EXIT_FAILED
    if args.udp and not args.no_load:
        out("session: --udp (delugemu) needs --no-load")
        return EXIT_FAILED
    if not args.no_load and args.timeout < MIN_LOAD_TIMEOUT:
        # Shorter, the late-answer window would be the whole wait, and a hang would read as a load that didn't take.
        out(f"session: a load needs --timeout of at least {MIN_LOAD_TIMEOUT:g} s")
        return EXIT_FAILED
    if args.events is not None:
        try:
            re.compile(args.events)
        except re.error as exc:
            out(f"session: --events {args.events!r} isn't a regex: {exc}")
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
