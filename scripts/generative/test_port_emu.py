"""Headless delugemu runs of the sysex test port (#46): the probe scripts without a Deluge.

Boots an ENABLE_SYSEX_TEST build in delugemu on a per-run APFS clone of the factory card image, with the emulated
DIN MIDI port bridged to UDP, and drives it only through the test port: it checks the error replies, then runs one or
more test scripts on that one boot (by default test_port_smoke.txt, the same script that runs against a real Deluge),
printing "SCRIPT: N expects, M failed" for each.

DIN rather than USB because delugemu's USB-MIDI model passes the firmware's output to the host but never delivers
host input to the firmware.

The firmware: --build (re)builds the test-port build in build-testport/ (Release, -DENABLE_SYSEX_TEST=ON; its own
build directory, so build/ keeps the default options), incrementally. --elf runs another ELF.

Boots are checked and retried: a boot whose test port never answers (one boot in three can come up blank, spec
§21.3) or whose emulator exits (a held UDP port) is relaunched on fresh ports and a fresh card clone, and the first
inject after boot is retried (a "no reply to test port command 0x10" right after boot is a known flake).

Stdlib only. Needs a delugemu source checkout (scripts/run.sh) and a QEMU binary, e.g. the nightly bundle's. Never
runs on the master card image.

    python3 scripts/generative/test_port_emu.py --build --out /path/to/scratch/run1
    python3 scripts/generative/test_port_emu.py --out DIR --script scripts/generative/perform_runner.txt [--script ...]
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
    ):
        self.elf, self.out, self.delugemu, self.qemu = (
            os.path.abspath(elf),
            out,
            delugemu,
            qemu,
        )
        self.sd_master, self.keep_card, self.tag = sd_master, keep_card, tag
        self.gdb_port, self.gdb_wait = gdb_port, gdb_wait
        self.card = os.path.join(out, tag + ".img")
        if os.path.realpath(sd_master) == os.path.realpath(self.card):
            raise ValueError("refusing to run on the master card image")
        self.proc = None
        self.port = None
        self.log_file = None

    def __enter__(self):
        os.makedirs(self.out, exist_ok=True)
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
        if not self.keep_card and os.path.exists(self.card):
            os.unlink(self.card)

    def _launch(self, attempt):
        if os.path.exists(self.card):
            os.unlink(self.card)
        subprocess.run(["cp", "-c", self.sd_master, self.card], check=True)
        # Our end binds first, on a port the OS picks; QEMU's end is probed free just before launch. A port someone
        # grabs in between makes QEMU exit, which the boot check catches and relaunches on new ports.
        link = deluge_probe.UdpLink("0:0")
        listen = link.sock.getsockname()[1]
        send = free_udp_port()
        link.addr = ("127.0.0.1", send)
        self.port = deluge_probe.TestPort(link)
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


def run_scripts(emu, scripts, out=log):
    """Runs each script on the booted emulator, in order. Returns [(script, failed, total, error)].

    A script error (a bad line, or no reply from the test port) counts as a failed run. After no reply, the rest are
    skipped: the emulator is hung or gone.
    """
    results = []
    for i, script in enumerate(scripts):
        name = os.path.basename(script)
        try:
            failed, total = deluge_probe.run_script(
                emu.port,
                deluge_probe.read_script(script),
                out=lambda line, n=name: out(f"{n}: {line}"),
            )
            results.append((script, failed, total, None))
        except (TimeoutError, RuntimeError, ValueError) as exc:
            results.append((script, 1, 0, str(exc)))
            out(f"{name}: ERROR {exc}")
            if isinstance(exc, TimeoutError) or not emu.alive():
                results += [
                    (s, 1, 0, "skipped: the emulator stopped answering")
                    for s in scripts[i + 1 :]
                ]
                break
        out(deluge_probe.result_line(name, *results[-1][1:]))
    return results


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
    p.add_argument("--sd-master", default=SD_MASTER)
    p.add_argument("--delugemu", default=DELUGEMU)
    p.add_argument("--qemu", default=QEMU)
    p.add_argument("--keep-card", action="store_true")
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
    out = os.path.abspath(args.out)  # run.sh runs in the delugemu checkout
    elf = args.elf or TESTPORT_ELF
    if args.build:
        elf = build_testport() if not args.elf else elf
    if not os.path.exists(elf):
        sys.exit(f"{elf} doesn't exist: run with --build, or pass --elf")
    scripts = args.script or [os.path.join(HERE, "test_port_smoke.txt")]

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
        ) as emu:
            if not args.no_protocol_checks:
                protocol_checks(emu.port, check)
                log(f"protocol checks: {total} checks, {failed} failed")
            results = run_scripts(emu, scripts)
    except RuntimeError as exc:
        log(f"ERROR {exc}")
        return 1
    log("summary:")
    for script, f, t, e in results:
        log(
            f"  {os.path.basename(script)}: {t} expects, {f} failed"
            + (f" ({e})" if e else "")
        )
        failed += f
        total += t
    log(f"{total} checks, {failed} failed; logs in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
