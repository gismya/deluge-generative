"""Headless delugemu runs of the sysex test port (#46): the probe scripts without a Deluge.

Boots an ENABLE_SYSEX_TEST build in delugemu on a per-run APFS clone of the factory card image, with the emulated
DIN MIDI port bridged to UDP, and drives it only through the test port: it checks the error replies, then runs one or
more test scripts on that one boot (by default test_port_smoke.txt, the same script that runs against a real Deluge),
printing "SCRIPT: N expects, M failed" for each.

DIN rather than USB because delugemu's USB-MIDI model passes the firmware's output to the host but never delivers
host input to the firmware.

The firmware: --build (re)builds the test-port build in build-testport/ (Release, -DENABLE_SYSEX_TEST=ON; its own
build directory, so build/ keeps the default options), incrementally. --elf runs another ELF.

Each run first takes a real-time emulator slot from the host-wide pool (emu_slots.py, DELUGE_EMU_SLOTS, default 6),
waiting while every checkout's runs together hold them all, and keeps it until its emulator has stopped.

Boots are checked and retried: a boot whose test port never answers (one boot in three can come up blank, spec
§21.3) or whose emulator exits (a held UDP port) is relaunched on fresh ports and a fresh card clone, and the first
inject after boot is retried (a "no reply to test port command 0x10" right after boot is a known flake).

A script that needs its own card (perform_sounds_override.txt) names an overlay folder in its "# needs: card DIR"
line: each boot's card clone then gets that folder (see apply_overlay()), written with fat_image.py, with no mount:
no sudo, and no macOS `._*` files on the card. Scripts that need different cards are refused: run them separately.
--card-overlay DIR picks the folder instead.

Stdlib only. Needs a delugemu source checkout (scripts/run.sh) and a QEMU binary, e.g. the nightly bundle's. Never
runs on the master card image.

    python3 scripts/generative/test_port_emu.py --build --out /path/to/scratch/run1
    python3 scripts/generative/test_port_emu.py --out DIR --script scripts/generative/perform_runner.txt [--script ...]
    python3 scripts/generative/test_port_emu.py --out DIR --script scripts/generative/perform_sounds.txt --events 'gen_sound_level|gen_mem'
    python3 scripts/generative/test_port_emu.py --out DIR --script scripts/generative/perform_sounds_override.txt
"""

import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe
import emu_slots
import fat_image
import hardware_session

HOME = os.path.expanduser("~")
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
QEMU = os.path.join(
    HOME,
    "source",
    "delugemu-nightly",
    "DelugEmu-macos-arm64",
    "DelugEmu.app",
    "Contents",
    "MacOS",
    "qemu-system-arm",
)
TESTPORT_BUILD = os.path.join(REPO, "build-testport")
TESTPORT_ELF = os.path.join(TESTPORT_BUILD, "Release", "deluge.elf")
SD_MASTER = os.path.join(HOME, "source", "delugemu-sd", "deluge_sd.img")
DELUGEMU = os.path.join(HOME, "source", "delugemu")

BOOT_TRIES = 3  # one boot in three can come up blank (spec §21.3)
BOOT_TIMEOUT = 90  # s, wall clock: the test port answers and the boot song has loaded
WARMUP_TRIES = 3


def log(msg):
    print("[test-port-emu] " + msg, flush=True)


def build_testport():
    """Builds build-testport/Release/deluge.elf (Release, -DENABLE_SYSEX_TEST=ON), incrementally. Returns its path."""
    log("building the test-port firmware in build-testport/")
    code = hardware_session.configure_and_build(
        TESTPORT_BUILD, ["-DENABLE_SYSEX_TEST=ON"]
    )
    if code:
        raise subprocess.CalledProcessError(code, "configure_and_build build-testport/")
    return TESTPORT_ELF


CARD_STEPS = "card.txt"  # an overlay's steps, not copied


def apply_overlay(card, overlay, repo=REPO):
    """Puts the overlay folder `overlay` onto the card image `card` (#277). First the steps in its card.txt, in order,
    one a line ("#" starts a comment; paths may hold spaces):
      remove PATH        removes the file or folder at PATH from the card; one the card doesn't have is an error
                         (the factory card changed under the overlay)
      copy FROM -> TO    copies FROM onto the card as TO. FROM is a file or folder in the repo, relative to its root
                         (a folder's contents go under TO; "/" is the card's root; a FROM outside the repo is an
                         error), or card:PATH, a file on the card
    Then every other file in the folder goes to the same path on the card. Names starting with a dot (.DS_Store,
    ._*) are never copied."""
    if not os.path.isdir(overlay):
        raise FileNotFoundError(f"no card overlay folder {overlay}")
    steps = os.path.join(overlay, CARD_STEPS)
    with fat_image.FatImage(card) as image:
        if os.path.exists(steps):
            with open(steps) as fh:
                for number, raw in enumerate(fh, 1):
                    _overlay_step(
                        image,
                        raw.split("#", 1)[0].strip(),
                        repo,
                        f"{steps} line {number}",
                    )
        fat_image.copy_tree(image, overlay, skip={CARD_STEPS})


def _overlay_step(image, line, repo, where):
    if not line:
        return
    verb, _, rest = line.partition(" ")
    if verb == "remove" and rest.strip():
        if not image.exists(rest.strip()):
            raise FileNotFoundError(f"{where}: the card has no {rest.strip()}")
        image.remove(rest.strip())
        return
    source, arrow, target = rest.partition(" -> ")
    source, target = source.strip(), target.strip().strip("/")
    if verb != "copy" or not arrow or not source:
        raise ValueError(f"{where}: not `remove PATH` or `copy FROM -> TO`: {line!r}")
    if source.startswith("card:"):
        image.write_file(target, image.read_file(source[len("card:") :]))
        return
    path = os.path.realpath(os.path.join(repo, *source.split("/")))
    root = os.path.realpath(repo)
    if os.path.commonpath([path, root]) != root:
        raise ValueError(f"{where}: {source} is outside the repo")
    if os.path.isdir(path):
        fat_image.copy_tree(image, path, target)
    elif os.path.isfile(path):
        with open(path, "rb") as fh:
            image.write_file(target, fh.read())
    else:
        raise FileNotFoundError(f"{where}: no {source} in the repo")


def free_udp_port():
    """A UDP port nothing on 127.0.0.1 holds right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class EmuRun:
    """One booted delugemu with the test port on a UDP bridge. Use as a context manager; .port is the TestPort."""

    def __init__(
        self,
        elf,
        out,
        delugemu=DELUGEMU,
        qemu=QEMU,
        sd_master=SD_MASTER,
        keep_card=False,
        tag="run",
        gdb_port=None,
        gdb_wait=False,
        card_overlay=None,
        take_slot=None,
    ):
        self.elf, self.out, self.delugemu, self.qemu = (
            os.path.abspath(elf),
            out,
            delugemu,
            qemu,
        )
        self.sd_master, self.keep_card, self.tag = sd_master, keep_card, tag
        self.gdb_port, self.gdb_wait = gdb_port, gdb_wait
        # a folder apply_overlay() puts on each boot's card clone
        self.card_overlay = card_overlay
        self.card = os.path.join(out, tag + ".img")
        if os.path.realpath(sd_master) == os.path.realpath(self.card):
            raise ValueError("refusing to run on the master card image")
        self.proc = None
        self.port = None
        self.log_file = None
        # a real-time emulator slot from the host-wide pool (emu_slots.take, #417): held from before the first launch
        # until the emulator has stopped, across boot retries; the emulator inherits its lock (Slot.fd)
        self.take_slot = take_slot or (lambda out: emu_slots.take(out=out))
        self._slot_hold = None
        self.slot = None

    def __enter__(self):
        os.makedirs(self.out, exist_ok=True)
        self._slot_hold = self.take_slot(log)
        self.slot = self._slot_hold.__enter__()
        try:
            return self._boot()
        except BaseException:
            self._release_slot()
            raise

    def _boot(self):
        for attempt in range(1, BOOT_TRIES + 1):
            try:
                self._launch(attempt)
                state = self._wait_for_boot()
                self._warm_up()
                log(f"booted (attempt {attempt}): {state}")
                self.port.subscribe()
                return self
            except RuntimeError as exc:
                log(f"boot {attempt} failed: {exc}; see {self.run_log}")
                self._stop()
        raise RuntimeError(f"no boot in {BOOT_TRIES} tries")

    def __exit__(self, *exc):
        if self.port is not None:
            try:
                self.port.subscribe(False)
            except OSError:
                pass
            self.save_log()
        self._stop()
        self._release_slot()
        if not self.keep_card and os.path.exists(self.card):
            os.unlink(self.card)

    def _release_slot(self):
        if self._slot_hold is not None:
            hold, self._slot_hold, self.slot = self._slot_hold, None, None
            hold.__exit__(None, None, None)

    def _launch(self, attempt):
        if os.path.exists(self.card):
            os.unlink(self.card)
        subprocess.run(["cp", "-c", self.sd_master, self.card], check=True)
        if self.card_overlay:
            try:
                apply_overlay(self.card, self.card_overlay)
            except Exception:
                os.unlink(self.card)
                raise
        # Our end binds first, on a port the OS picks; QEMU's end is probed free just before launch. A port someone
        # grabs in between makes QEMU exit, which the boot check catches and relaunches on new ports.
        link = deluge_probe.UdpLink("0:0")
        listen = link.sock.getsockname()[1]
        send = free_udp_port()
        link.addr = ("127.0.0.1", send)
        self.port = deluge_probe.TestPort(
            link, on_hardware=False
        )  # skips hardware_only expects
        self.run_log = os.path.join(self.out, f"{self.tag}-boot{attempt}.log")
        cmd = [
            os.path.join(self.delugemu, "scripts", "run.sh"),
            self.elf,
            "--sd",
            self.card,
            "--display",
            "none",
            "--audio",
            "none",
            "--midi",
            f"udp:127.0.0.1:{listen}@127.0.0.1:{send}",
        ]
        if self.gdb_port:
            cmd += ["--", "-gdb", f"tcp::{self.gdb_port}"] + (
                ["-S"] if self.gdb_wait else []
            )
        self.log_file = open(self.run_log, "w")  # noqa: SIM115 - the emulator writes to it for its whole run
        self.proc = subprocess.Popen(
            cmd,
            cwd=self.delugemu,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            # the slot's lock: an emulator that outlives this run (a killed runner) keeps its slot until it exits
            pass_fds=[self.slot.fd] if self.slot is not None else [],
            env=dict(os.environ, DELUGEMU_QEMU_BIN=self.qemu),
        )

    def _wait_for_boot(self):
        # Under --gdb-wait the CPU waits at reset, or at a breakpoint, for as long as gdb holds it: no deadline, or the
        # retry would restart the emulator under gdb.
        deadline = None if self.gdb_wait else time.monotonic() + BOOT_TIMEOUT
        while deadline is None or time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("emulator exited during boot")
            try:
                state = self.port.state()
                if deluge_probe.booted(state):
                    return state
            except TimeoutError:
                pass
            time.sleep(0.5)
        raise RuntimeError(
            f"the test port did not answer within {BOOT_TIMEOUT} s (a blank boot?)"
        )

    def _warm_up(self):
        """The first inject after boot sometimes gets no reply: retry a harmless one until the port takes injects."""
        time.sleep(1.0)
        for _ in range(WARMUP_TRIES):
            try:
                self.port.inject("wait 0")
                self.port.wait_idle()
                return
            except TimeoutError:
                time.sleep(1.0)
        raise RuntimeError("the test port answers state requests but not injects")

    def _stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
        self.proc = None
        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None
        if self.port is not None:
            self.port.link.close()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def save_log(self):
        with open(os.path.join(self.out, self.tag + "-sysex.log"), "w") as fh:
            fh.write(
                "\n".join(self.port.log + ["--- events"] + self.port.events) + "\n"
            )


def protocol_checks(port, check):
    """Error replies: a line that doesn't parse, and a key that doesn't match (the emulator has no key set)."""
    for line in (
        "button NOPE down",
        "pad 18 0 down",
        "encoder 6 1",
        "wait",
        "hold clip",
        "frobnicate 1 2",
        "next recipe tempo 5",  # below kMinTempo
        "next recipe genre nosuch",
        "next recipe part 6 SYNTHS/A.XML",  # past the last part
        "next recipe lane 0 " + "A" * 128,  # longer than a sound pick holds
    ):
        try:
            port.inject(line)
            check(f"{line!r} is a parse error", False)
        except RuntimeError as exc:
            check(f"{line!r} is a parse error", "parse error" in str(exc))
    # The first wait leaves the queue once the buttons task starts it and holds back the rest, so the queue fills: 63
    # entries fit (one slot stays free), plus the first wait if it has started by then. The rest are refused.
    statuses = []
    for line in ["wait 2000"] + ["wait 0"] * 64:
        msg = port._request(
            deluge_probe.INJECT, line.encode("ascii"), (deluge_probe.STATUS_REPLY,)
        )
        statuses.append(deluge_probe.STATUS[msg[8]])
    full = statuses.index("queue full") if "queue full" in statuses else len(statuses)
    check(
        "a full queue refuses entries",
        full in (63, 64) and statuses[full:] == ["queue full"] * (len(statuses) - full),
    )
    port.wait_idle()
    keyed = deluge_probe.TestPort(port.link, key=0x12345678)
    try:
        keyed.state()
        check("a wrong key is refused", False)
    except RuntimeError as exc:
        check("a wrong key is refused", "bad key" in str(exc))
    check("the device still answers", port.state()["inject_queue"] == 0)


def run_scripts(emu, scripts, out=log, events=None):
    """Runs each script on the booted emulator, in order (deluge_probe.run_one_script(), as hardware_session.py
    does). Returns [(script, failed, total, error, skipped)]: `skipped` counts its hardware_only expects, which the
    emulator skips.

    A script error (a bad line, or no reply from the test port) counts as one more failed expect, after those the
    script ran (deluge_probe.ScriptStopped), and prints the script's last events of any kind. After no reply, the
    rest are skipped: the emulator is hung or gone. With `events` (a regex), after each script it prints the
    script's test events that match, from after its first start_state, as hardware_session.py's --events does.
    """
    results = []
    pattern = deluge_probe.events_pattern(events)
    for i, script in enumerate(scripts):
        name = os.path.basename(script)
        skipped_before = emu.port.hardware_only_skipped
        try:
            failed, total = deluge_probe.run_one_script(
                emu.port,
                script,
                out=lambda line, n=name: out(f"{n}: {line}"),
                events=pattern,
            )
            result, gone = (script, failed, total, None), False
        except deluge_probe.ScriptStopped as stop:
            result = (script, *stop.result())
            out(f"{name}: ERROR {stop.error}")
            gone = deluge_probe.link_failed(stop.error) or not emu.alive()
        results.append((*result, emu.port.hardware_only_skipped - skipped_before))
        if gone:
            results += [
                (s, 1, 0, "skipped: the emulator stopped answering", 0)
                for s in scripts[i + 1 :]
            ]
            break
        out(deluge_probe.result_line(name, *results[-1][1:]))
    return results


def scripts_card(scripts):
    """The card overlay folder the scripts' "# needs: card DIR" lines name (deluge_probe.script_needs()), or None
    for the factory card. Raises ValueError when they need different cards: one boot has one card."""
    cards = {script: deluge_probe.script_needs(script).card for script in scripts}
    if len(set(cards.values())) > 1:
        raise ValueError(
            "these scripts need different cards, so run them separately: "
            + ", ".join(
                f"{os.path.basename(s)} ({card or 'the factory card'})"
                for s, card in cards.items()
            )
        )
    return next(iter(cards.values()), None)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--out", required=True, help="scratch directory for the card clone and logs"
    )
    p.add_argument(
        "--elf", help="firmware to boot (default: build-testport/Release/deluge.elf)"
    )
    p.add_argument(
        "--build",
        action="store_true",
        help="(re)build build-testport/ first, incrementally",
    )
    p.add_argument(
        "--script",
        action="append",
        help="a test script; repeat to run several on one boot (default: test_port_smoke.txt)",
    )
    p.add_argument(
        "--no-protocol-checks", action="store_true", help="skip the error-reply checks"
    )
    p.add_argument(
        "--events",
        metavar="PATTERN",
        help="after each script, print its test events that match this regex (--events . for all), from after "
        "its first start_state",
    )
    p.add_argument("--sd-master", default=SD_MASTER)
    p.add_argument("--delugemu", default=DELUGEMU)
    p.add_argument("--qemu", default=QEMU)
    p.add_argument("--keep-card", action="store_true")
    p.add_argument(
        "--card-overlay",
        metavar="DIR",
        help="put this overlay folder on the card clone (apply_overlay(): its card.txt steps, then its files); "
        'default: the one the scripts\' "# needs: card DIR" line names',
    )
    p.add_argument(
        "--gdb",
        type=int,
        metavar="PORT",
        help="let gdb attach on this TCP port; with a hardware breakpoint on handle_cpu_fault, r0 and r2 at the "
        "break are a fault's PC and LR",
    )
    p.add_argument(
        "--gdb-wait",
        action="store_true",
        help="with --gdb, hold the CPU at reset until gdb continues it, to catch a fault during boot (attach within "
        "the boot timeout)",
    )
    args = p.parse_args()
    if args.gdb_wait and not args.gdb:
        p.error("--gdb-wait needs --gdb")
    try:
        deluge_probe.events_pattern(args.events)
    except ValueError as exc:
        p.error(str(exc))
    out = os.path.abspath(args.out)  # run.sh runs in the delugemu checkout
    elf = args.elf or TESTPORT_ELF
    if args.build:
        elf = build_testport() if not args.elf else elf
    if not os.path.exists(elf):
        sys.exit(f"{elf} doesn't exist: run with --build, or pass --elf")
    scripts = args.script or [os.path.join(HERE, "test_port_smoke.txt")]
    card_overlay = args.card_overlay
    if card_overlay is None:
        try:
            card_overlay = scripts_card(scripts)
        except (ValueError, OSError) as exc:
            sys.exit(str(exc))
    if card_overlay and not os.path.isdir(card_overlay):
        sys.exit(f"no card overlay folder {card_overlay}")

    failed = total = 0

    def check(name, ok):
        nonlocal failed, total
        failed += 0 if ok else 1
        total += 1
        log("{} {}".format("PASS" if ok else "FAIL", name))

    log("launching " + elf)
    try:
        with EmuRun(
            elf,
            out,
            args.delugemu,
            args.qemu,
            args.sd_master,
            args.keep_card,
            gdb_port=args.gdb,
            gdb_wait=args.gdb_wait,
            card_overlay=card_overlay,
        ) as emu:
            if not args.no_protocol_checks:
                protocol_checks(emu.port, check)
                log(f"protocol checks: {total} checks, {failed} failed")
            results = run_scripts(emu, scripts, events=args.events)
    except (RuntimeError, ValueError, OSError) as exc:
        log(f"ERROR {exc}")
        return 1
    log("summary:")
    skipped = 0
    for script, f, t, e, k in results:
        log("  " + deluge_probe.result_line(os.path.basename(script), f, t, e, k))
        failed += f
        total += t
        skipped += k
    # check_all.py's table note is this line's "N checks, M failed[, K hardware-only skipped]"
    log(
        f"{total} checks, {failed} failed"
        + (f", {skipped} hardware-only skipped" if skipped else "")
        + f"; logs in {out}"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
