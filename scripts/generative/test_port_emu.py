"""Headless delugemu run of the sysex test port (#46).

Boots an ENABLE_SYSEX_TEST build in delugemu on a per-run APFS clone of the factory card image, with the emulated
DIN MIDI port bridged to UDP, and drives it only through the test port: it checks the error replies, then runs a
test script (by default test_port_smoke.txt, the same script that runs against a real Deluge).

DIN rather than USB because delugemu's USB-MIDI model passes the firmware's output to the host but never delivers
host input to the firmware.

Stdlib only. Needs a delugemu source checkout (scripts/run.sh) and a QEMU binary, e.g. the nightly bundle's. Never
runs on the master card image.

    ./dbt configure -DENABLE_SYSEX_TEST=ON && ./dbt build debug
    python3 scripts/generative/test_port_emu.py --out /path/to/scratch/run1
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe

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


def log(msg):
    print("[test-port-emu] " + msg, flush=True)


def launch(args, card, udp_spec, run_log):
    cmd = [
        os.path.join(args.delugemu, "scripts", "run.sh"),
        args.elf,
        "--sd",
        card,
        "--display",
        "none",
        "--audio",
        "none",
        "--midi",
        udp_spec,
    ]
    env = dict(os.environ, DELUGEMU_QEMU_BIN=args.qemu)
    fh = open(run_log, "w")  # noqa: SIM115 - the emulator writes to it for its whole run
    return subprocess.Popen(
        cmd,
        cwd=args.delugemu,
        stdout=fh,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
        env=env,
    )


def wait_for_boot(port, proc, timeout=90):
    """Waits until the test port answers and the boot song has loaded."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("emulator exited during boot")
        try:
            state = port.state()
            if deluge_probe.booted(state):
                return state
        except TimeoutError:
            pass
        time.sleep(0.5)
    raise RuntimeError("the test port did not answer")


def protocol_checks(port, check):
    """Error replies: a line that doesn't parse, and a key that doesn't match (the emulator has no key set)."""
    for line in (
        "button NOPE down",
        "pad 18 0 down",
        "encoder 6 1",
        "wait",
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


def run(args):
    os.makedirs(args.out, exist_ok=True)
    card = os.path.join(args.out, "run.img")
    run_log = os.path.join(args.out, "run.log")
    if os.path.exists(card):
        os.unlink(card)
    log("cloning card image -> " + card)
    subprocess.run(["cp", "-c", args.sd_master, card], check=True)

    listen_port, qemu_port = args.udp_port, args.udp_port + 1
    port = deluge_probe.TestPort(deluge_probe.UdpLink(f"{listen_port}:{qemu_port}"))
    log("launching " + args.elf)
    proc = launch(
        args, card, f"udp:127.0.0.1:{listen_port}@127.0.0.1:{qemu_port}", run_log
    )

    failed = total = 0

    def check(name, ok):
        nonlocal failed, total
        failed += 0 if ok else 1
        total += 1
        log("{} {}".format("PASS" if ok else "FAIL", name))

    try:
        state = wait_for_boot(port, proc)
        log("booted: " + str(state))
        time.sleep(1.0)
        port.subscribe()
        protocol_checks(port, check)
        script_failed, script_total = deluge_probe.run_script(
            port, deluge_probe.read_script(args.script), out=log
        )
        failed += script_failed
        total += script_total
    finally:
        port.subscribe(False)
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)

    with open(os.path.join(args.out, "sysex.log"), "w") as fh:
        fh.write("\n".join(port.log + ["--- events"] + port.events) + "\n")
    if not args.keep_card and os.path.exists(card):
        os.unlink(card)
    log(f"{total} checks, {failed} failed; log in {args.out}")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--out", required=True, help="scratch directory for the card clone and logs"
    )
    p.add_argument("--elf", default=os.path.join(REPO, "build", "Debug", "deluge.elf"))
    p.add_argument("--script", default=os.path.join(HERE, "test_port_smoke.txt"))
    p.add_argument(
        "--sd-master",
        default=os.path.join(HOME, "source", "delugemu-sd", "deluge_sd.img"),
    )
    p.add_argument("--delugemu", default=os.path.join(HOME, "source", "delugemu"))
    p.add_argument("--qemu", default=QEMU)
    p.add_argument(
        "--udp-port",
        type=int,
        default=22998,
        help="host port for the MIDI bridge (and +1)",
    )
    p.add_argument("--keep-card", action="store_true")
    args = p.parse_args()
    args.elf = os.path.abspath(args.elf)  # run.sh runs in the delugemu checkout
    if os.path.realpath(args.sd_master) == os.path.realpath(
        os.path.join(args.out, "run.img")
    ):
        sys.exit("refusing to run on the master card image")
    sys.exit(run(args))


if __name__ == "__main__":
    main()
