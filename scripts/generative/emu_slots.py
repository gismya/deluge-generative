"""The host-wide pool of real-time emulator slots (#417 slice 2): every delugemu that runs in real time takes one.

    python3 scripts/generative/emu_slots.py [--since SECONDS_AGO]

Probe scripts run delugemu in real time, so their timing and voice-count figures fail once the host is busy, and the
load comes from every checkout's runs together (several agents' check_all.py at once), not from one run's --jobs.
test_port_emu.EmuRun therefore takes a slot from this pool before it launches an emulator and holds it until the
emulator has stopped: the probe scripts (test_port_emu.py), and gen_emu.py's leak check and firmware check, which boot
the same way. The icount runs (gen_emu.py smoke, capture) don't take one: their figures don't depend on the host.

A slot is an flock on FOLDER/slot-N.lock (~/.cache/deluge-generative/emu-slots/), N below the pool size:
DELUGE_EMU_SLOTS, default 6 (half of 12 cores, as check_all.py's --jobs). The kernel drops a dead process's flock, so a
crashed run frees its slot. The emulator inherits the lock's file descriptor (EmuRun passes Slot.fd to it), so an
emulator that outlives its runner keeps its slot until it exits. A run that finds the pool full says so, polls, and
says how long it waited (WAITED, which check_all.py's table shows).

Each take and release appends "TIME take|release SLOT PID" to FOLDER/slots.log. The CLI prints the slots held now and
the most held at once, from the log (all of it, or the last --since seconds).
"""

import argparse
import contextlib
import fcntl
import os
import re
import sys
import time

FOLDER = os.path.join(
    os.path.expanduser("~"), ".cache", "deluge-generative", "emu-slots"
)
SIZE_ENV = "DELUGE_EMU_SLOTS"
DEFAULT_SIZE = 6
LOG = "slots.log"
POLL = 2.0  # s between tries while the pool is full
# What a run that waited prints, and check_all.py reads into its table.
WAITED = re.compile(r"waited (\d+) s for a real-time emulator slot")


def pool_size(env=None):
    """The pool's size: SIZE_ENV, else DEFAULT_SIZE. ValueError for a value that isn't a whole number above 0."""
    env = os.environ if env is None else env
    text = env.get(SIZE_ENV)
    if text is None:
        return DEFAULT_SIZE
    try:
        size = int(text)
    except ValueError:
        size = 0
    if size < 1:
        raise ValueError(
            f"{SIZE_ENV}={text!r}: give a whole number of slots, 1 or more"
        )
    return size


class Slot:
    """A held slot: index, the lock's fd (pass it to the emulator so its slot lives as long as it does), and how long
    the take waited."""

    def __init__(self, index, fd, folder, waited=0.0):
        self.index, self.fd, self.folder, self.waited = index, fd, folder, waited

    def release(self):
        if self.fd is None:
            return
        _log(self.folder, "release", self.index)
        os.close(self.fd)  # drops the flock, unless an emulator still holds the fd
        self.fd = None


def _log(folder, what, index):
    with contextlib.suppress(OSError), open(os.path.join(folder, LOG), "a") as fh:
        fh.write(f"{time.time():.1f} {what} {index} {os.getpid()}\n")


def try_take(size, folder=FOLDER):
    """A free slot, or None if all `size` are held."""
    os.makedirs(folder, exist_ok=True)
    for index in range(size):
        fd = os.open(
            os.path.join(folder, f"slot-{index}.lock"), os.O_RDWR | os.O_CREAT, 0o644
        )
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            continue
        _log(folder, "take", index)
        return Slot(index, fd, folder)
    return None


@contextlib.contextmanager
def take(size=None, folder=FOLDER, out=print, poll=POLL):
    """Holds a slot for the `with` block, waiting while the pool is full (saying so once, then how long it waited)."""
    size = pool_size() if size is None else size
    start = time.monotonic()
    slot = try_take(size, folder)
    if slot is None:
        out(
            f"waiting for a real-time emulator slot: all {size} are in use ({SIZE_ENV} sets the pool; "
            f"python3 scripts/generative/emu_slots.py shows them)"
        )
        while slot is None:
            time.sleep(poll)
            slot = try_take(size, folder)
        slot.waited = time.monotonic() - start
        out(
            f"waited {slot.waited:.0f} s for a real-time emulator slot (slot {slot.index} of {size})"
        )
    try:
        yield slot
    finally:
        slot.release()


def parse_log(lines):
    """(time, take|release, slot, pid) for each well-formed log line."""
    out = []
    for line in lines:
        parts = line.split()
        try:
            stamp, what, index, pid = (
                float(parts[0]),
                parts[1],
                int(parts[2]),
                int(parts[3]),
            )
        except (IndexError, ValueError):
            continue
        if what in ("take", "release"):
            out.append((stamp, what, index, pid))
    return out


def read_log(folder=FOLDER):
    try:
        with open(os.path.join(folder, LOG)) as fh:
            return parse_log(fh)
    except OSError:
        return []


def peak(log, since=None):
    """The most slots held at once, by the log: from `since` (a time) on, counting those held then. A slot taken again
    without a release (its holder was killed) counts once."""
    held, most = set(), 0
    for stamp, what, index, _ in sorted(log):
        if since is not None and stamp >= since:
            most = max(most, len(held))  # those held when the window opened
        if what == "take":
            held.add(index)
        else:
            held.discard(index)
        if since is None or stamp >= since:
            most = max(most, len(held))
    if since is not None:
        most = max(
            most, len(held)
        )  # held still, the log's last line before the window or in it
    return most


def held_now(size, folder=FOLDER):
    """The slots held right now, of `size`."""
    held = []
    for index in range(size):
        slot_path = os.path.join(folder, f"slot-{index}.lock")
        if not os.path.exists(slot_path):
            continue
        fd = os.open(slot_path, os.O_RDWR)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            held.append(index)
        finally:
            os.close(fd)
    return held


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--since",
        type=float,
        metavar="SECONDS_AGO",
        help="the peak over the last SECONDS_AGO only",
    )
    args = p.parse_args(argv)
    size = pool_size()
    held = held_now(size)
    since = time.time() - args.since if args.since is not None else None
    print(f"{len(held)} of {size} real-time emulator slots held now ({FOLDER})")
    print(
        f"most held at once{' in that time' if since else ''}: {peak(read_log(), since)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
