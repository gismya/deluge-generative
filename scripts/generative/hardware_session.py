"""`deluge_probe.py session`: one hardware run on the shared Deluge, with the hardware rules in code.

    ./dbt exec "python3 scripts/generative/deluge_probe.py session --build release SCRIPT..."
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
4. Waits for the test port (`ready`), after a load on a fresh boot: young, and younger than the build that ran
   before. No answer: writes the hung marker, exits 2. The old build still answering to the end means the load
   didn't take: that exits 1, with no marker.
5. Runs every script on that one load, printing "SCRIPT: N expects, M failed" and a summary. With --events PATTERN,
   it also prints the script's test events that match the regex after each script (`--events 'gen_mem|gen_left'`,
   `--events .` for all), counted from after its start_state. A script that gets no reply from the test port writes
   the hung marker and stops the run (exit 2). Any failure exits 1.

State lives in ~/.cache/deluge-generative/hardware/ (or $DELUGE_HARDWARE_DIR): lock/ (owner inside), hung, pending.md,
loaded.json (the last load this script saw) and released/.
"""

import datetime
import json
import os
import re
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
# How far below the old build's uptime the new one's must be, so a query's latency can't make the old build look new.
# A load takes longer than this, so a new build is always further below.
FRESH_SLACK_SECONDS = 2
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
        self.take = os.path.join(self.root, "lock.take")
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

    def _take_stale(self, out):
        """Moves the lock into released/ if it's stale. Returns whether the lock may have gone, so it's worth trying
        the mkdir again.

        Sessions that find a stale lock take turns: each holds `take` (a second mkdir lock) while it judges the lock
        again and moves it, so one can't move the live lock another has just taken in its place. A `take` older than
        OWNERLESS_STALE_SECONDS was left by a session that died holding it, and is moved away. If the lock was
        replaced anyway (its taker gave up after OWNERLESS_STALE_SECONDS, and a new one took it), the move caught a
        live lock, so it goes back: the directory's inode tells the two apart, since the stale one still exists in
        released/ and a new lock never reuses its inode."""
        if not self._stale():
            return False
        try:
            os.mkdir(self.take)
        except FileExistsError:
            return self._move_dead_take(out)
        try:
            try:
                judged = os.stat(self.lock).st_ino
            except FileNotFoundError:
                return True
            if not self._stale():
                return False  # another session took it while this one waited for `take`
            try:
                moved = self._move_to_released(self.lock, "stale-lock")
            except FileNotFoundError:
                return True
            if os.stat(moved).st_ino == judged:
                out(f"session: the lock's owner has exited; moved its lock to {moved}")
                return True
            try:
                os.rename(moved, self.lock)
            except OSError as exc:
                out(
                    f"session: moved a live lock to {moved} by mistake and couldn't put it back ({exc}); "
                    "its owner still holds the Deluge"
                )
            return True
        finally:
            self._move_to_released(self.take, "take")

    def _move_dead_take(self, out):
        """Moves `take` away if its session died holding it. Returns whether it's gone."""
        try:
            made = os.stat(self.take).st_mtime
        except FileNotFoundError:
            return True
        if time.time() - made <= OWNERLESS_STALE_SECONDS:
            return False
        try:
            moved = self._move_to_released(self.take, "stale-take")
        except FileNotFoundError:
            return True
        out(
            f"session: a session died while taking a stale lock; moved its {self.take} to {moved}"
        )
        return True

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


def fresh_boot(sample_timer, before=None, now=None):
    """Whether a build answering with `sample_timer` booted after the load. `before` is (uptime in seconds,
    time.monotonic() when read) from the build running before the load, or None if it didn't answer. A young build
    isn't enough: a load that fails its checksum leaves the old build running, and it may have booted only seconds
    before. So the uptime must also be below what the old build's would be by now, as its 32-bit timer reads it:
    an old build up for 27 h has wrapped round to a young uptime."""
    seconds = sample_timer / SAMPLE_RATE
    if seconds >= FRESH_BOOT_SECONDS:
        return False
    if before is None:
        return True
    uptime, at = before
    old_now = (
        uptime + ((time.monotonic() if now is None else now) - at)
    ) % TIMER_WRAP_SECONDS
    return seconds + FRESH_SLACK_SECONDS < old_now


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


def build_dir(build):
    """Where ./dbt build puts a build type's outputs."""
    configs = {
        "release": "Release",
        "debug": "Debug",
        "relwithdebinfo": "RelWithDebInfo",
    }
    return os.path.join(REPO, "build", configs.get(build.lower(), build))


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
    binary = os.path.join(build_dir(build), "deluge.bin")
    if not os.path.exists(binary):
        return f"no {binary}: ./dbt build {build}"
    return None


# Builds whose internal-RAM segment (the code and data that run from internal RAM, by vaddr) ends past this
# address haven't booted after loadfw: the upload completes, then the device hangs with every LED lit (3 of 3 on
# 2026-11-07: relwithdebinfo test-port builds of 80f20883e and later, segment ending at 0x202005b4). The same code
# as a Release build, ending at 0x201f8274, boots and passes. This is the segment end, not the end of deluge.bin:
# the SDRAM sections' load copy follows it in the file and runs past this address in builds that boot, so keep
# measuring by vaddr (p_paddr would refuse every build). Why the boundary matters isn't known yet: issue #166.
CHAINLOAD_IMAGE_LIMIT = 0x20200000


def elf_for(build):
    """The deluge.elf beside the binary `build` names (a build type or a deluge.bin path)."""
    if os.path.isfile(build):
        return os.path.join(os.path.dirname(os.path.abspath(build)), "deluge.elf")
    return os.path.join(build_dir(build), "deluge.elf")


def image_end(elf_path):
    """End (by vaddr) of the file-backed load segments that run from internal RAM, or None if unreadable."""
    import struct

    try:
        with open(elf_path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if data[:4] != b"\x7fELF" or data[4] != 1:  # ELF32 only
        return None
    (phoff,) = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    end = None
    for i in range(phnum):
        p_type, _off, vaddr, _paddr, filesz, _memsz = struct.unpack_from(
            "<6I", data, phoff + i * phentsize
        )
        if (
            p_type == 1 and filesz and 0x20000000 <= vaddr < 0x20300000
        ):  # PT_LOAD in internal RAM
            end = max(end or 0, vaddr + filesz)
    return end


def check_image_end(build):
    """Why `build` won't boot after loadfw, or None."""
    end = image_end(elf_for(build))
    if end is None or end <= CHAINLOAD_IMAGE_LIMIT:
        return None
    return (
        f"the internal-RAM segment ends at {end:#x}, past {CHAINLOAD_IMAGE_LIMIT:#x}; builds like this have hung after every "
        "loadfw (all LEDs lit). Load a Release build instead (./dbt build release, --build release), or pass "
        "--force-load to try anyway"
    )


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
        self.old_uptime = None

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

    def wait_ready(self, fresh, before=None):
        """The TestPort once the firmware answers and has booted, or None. With `fresh` (after a load), only a build
        that booted since: see fresh_boot(), which takes `before`. On None, `old_uptime` is the uptime in seconds of
        the last answer if it came from a build that didn't boot since (the load didn't take), or None if the device
        had gone silent."""
        self.old_uptime = None
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            try:
                port = self.dp.wait_ready(
                    self.open_link,
                    self.dp.read_key(self.args),
                    deadline - time.monotonic(),
                )
            except TimeoutError:
                self.old_uptime = None
                return None
            try:
                state = port.state()
            except Exception as exc:
                # It answered once, then stopped: keep trying until the deadline.
                if not self.dp.link_failed(exc):
                    raise
                self.old_uptime = None
                port.link.close()
                time.sleep(0.5)
                continue
            if not fresh or fresh_boot(state["sample_timer"], before):
                self.out(f"session: ready, sample_timer {state['sample_timer']}")
                return port
            self.old_uptime = state["sample_timer"] / SAMPLE_RATE
            port.link.close()  # the old build: about to restart, or the load didn't take
            time.sleep(0.5)
        return None

    def run_one(self, port, script):
        lines = self.dp.read_script(script)
        seen = [
            len(port.events)
        ]  # from after start_state, once it has run: what it caused isn't the script's

        def started(index):
            seen[0] = index

        port.subscribe()
        try:
            return self.dp.run_script(
                port, lines, out=lambda line: self.out(f"  {line}"), started=started
            )
        finally:
            self.print_events(
                port.events[seen[0] :]
            )  # first: unsubscribing fails on a hung device
            port.subscribe(False)

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
            problem = (
                None
                if getattr(self.args, "force_load", False)
                else check_image_end(self.build)
            )
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
        port = self.wait_ready(fresh=bool(self.build), before=before)
        if port is None and self.old_uptime is not None:
            # It answered to the end, so it didn't hang: no marker.
            self.out(
                f"session: the build that was running still answers (uptime {self.old_uptime:.0f} s); "
                "the load didn't take"
            )
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
