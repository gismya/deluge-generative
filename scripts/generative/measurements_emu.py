"""Headless delugemu smoke test for the #37 hardware-measurement build.

Boots a Debug (or RelWithDebInfo) build in delugemu on a per-run APFS clone of the factory card image, with an
emulated MIDI port bridged to UDP. It subscribes to the firmware's sysex debug log, then presses every
measurement trigger (LEARN+CV, +SYNTH, +KIT, +MIDI, +KEYBOARD, +SCALE), enters and leaves Perform with SHIFT+SONG,
saves the song and loads it back. It checks that each produces its "MEAS" log lines and an OLED popup, and that the
firmware didn't abort.

The log goes over the emulated DIN MIDI port by default: the firmware takes the log subscription on any MIDI port,
and delugemu's USB-MIDI model (as of 2026-11) passes the firmware's output to the host but never delivers host input
to the firmware's receive pipe. On hardware the log goes over USB.

This only checks that the instrumentation works. Emulator numbers are not hardware numbers.

Stdlib only. Needs a delugemu source checkout (pylib and scripts/run.sh) and a QEMU binary, e.g. the nightly
bundle's. Never runs on the master card image.

    python3 scripts/generative/measurements_emu.py --out /path/to/scratch/meas1
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

HOME = os.path.expanduser("~")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULTS = {
    "elf": os.path.join(REPO, "build", "Debug", "deluge.elf"),
    "sd_master": os.path.join(HOME, "source", "delugemu-sd", "deluge_sd.img"),
    "delugemu": os.path.join(HOME, "source", "delugemu"),
    "qemu": os.path.join(
        HOME,
        "source",
        "delugemu-nightly",
        "DelugEmu-macos-arm64",
        "DelugEmu.app",
        "Contents",
        "MacOS",
        "qemu-system-arm",
    ),
}

SYSEX_LOG_ENABLE = bytes([0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x01, 0xF7])
SYSEX_LOG_HEADER = bytes([0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x40, 0x00])


def log(msg):
    print("[meas-emu] " + msg, flush=True)


class SysexLog:
    """Collects the firmware's debug log lines from the USB-MIDI byte stream on a UDP socket."""

    def __init__(self, listen_port, send_port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", listen_port))
        self.sock.settimeout(0.2)
        self.send_addr = ("127.0.0.1", send_port)
        self.lines = []
        self.buf = bytearray()
        self.stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def send(self, data):
        self.sock.sendto(data, self.send_addr)

    def _run(self):
        while not self.stop:
            try:
                data, _ = self.sock.recvfrom(65536)
            except TimeoutError:
                continue
            except OSError:
                return
            self.buf += data
            while True:
                start = self.buf.find(0xF0)
                if start < 0:
                    self.buf.clear()
                    break
                end = self.buf.find(0xF7, start)
                if end < 0:
                    del self.buf[:start]
                    break
                msg = bytes(self.buf[start : end + 1])
                del self.buf[: end + 1]
                if msg.startswith(SYSEX_LOG_HEADER):
                    text = msg[len(SYSEX_LOG_HEADER) : -1].decode("ascii", "replace")
                    for line in text.splitlines():
                        if line.strip():
                            self.lines.append(line)

    def meas(self, name):
        return [line for line in self.lines if f"MEAS {name} " in line]

    def close(self):
        self.stop = True
        self.sock.close()


def button_coords(name):
    from delugemu._controls import CONTROLS

    c = CONTROLS[name]
    return c["col"], c["row"]


def chord(s, held, tapped, hold_s=0.15):
    hx, hy = button_coords(held)
    tx, ty = button_coords(tapped)
    s.button(hx, hy, True)
    s.wait(hold_s)
    s.button(tx, ty, True)
    s.wait(hold_s)
    s.button(tx, ty, False)
    s.wait(hold_s)
    s.button(hx, hy, False)


def tap(s, name, hold_s=0.15):
    x, y = button_coords(name)
    s.button(x, y, True)
    s.wait(hold_s)
    s.button(x, y, False)


def oled_text(s):
    text = s.oled_text()
    return text if isinstance(text, str) else "\n".join(text)


def launch(args, card, sock, udp_spec, run_log, int_log):
    run_sh = os.path.join(args.delugemu, "scripts", "run.sh")
    cmd = [
        run_sh,
        args.elf,
        "--sd",
        card,
        "--display",
        "none",
        "--audio",
        "none",
        "--midi" if args.midi_port == "din" else "--usb-midi",
        udp_spec,
        "--",
        "-qmp",
        f"unix:{sock},server,nowait",
        "-d",
        "int",
        "-D",
        int_log,
    ]
    env = dict(os.environ, DELUGEMU_QEMU_BIN=args.qemu)
    fh = open(run_log, "w")  # noqa: SIM115 - the emulator writes to it for its whole run
    proc = subprocess.Popen(
        cmd,
        cwd=args.delugemu,
        stdout=fh,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
        env=env,
    )
    deadline = time.monotonic() + 90
    while not os.path.exists(sock):
        if proc.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("emulator did not start; see " + run_log)
        time.sleep(0.1)
    return proc


def run(args):
    os.makedirs(args.out, exist_ok=True)
    sys.path.insert(0, os.path.join(args.delugemu, "pylib"))
    from delugemu import DelugeSession

    card = os.path.join(args.out, "run.img")
    run_log = os.path.join(args.out, "run.log")
    int_log = os.path.join(args.out, "int.log")
    for path in (card, int_log):
        if os.path.exists(path):
            os.unlink(path)
    if os.path.exists(card):
        os.unlink(card)
    log("cloning card image -> " + card)
    subprocess.run(["cp", "-c", args.sd_master, card], check=True)

    listen_port, qemu_port = args.udp_port, args.udp_port + 1
    sysex = SysexLog(listen_port, qemu_port)
    sock = f"/tmp/dz_meas_{os.getpid()}.sock"
    udp_spec = f"udp:127.0.0.1:{listen_port}@127.0.0.1:{qemu_port}"
    log("launching " + args.elf)
    proc = launch(args, card, sock, udp_spec, run_log, int_log)
    s = DelugeSession.attach(
        sock=sock,
        repo_root=args.delugemu,
        artifacts_dir=args.out,
        run_log=run_log,
        int_log=int_log,
    )

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok)))
        log("{} {} {}".format("PASS" if ok else "FAIL", name, detail))

    def shot(name):
        s.screenshot(os.path.join(args.out, name + ".png"))
        with open(os.path.join(args.out, name + ".oled.txt"), "w") as fh:
            fh.write(oled_text(s))

    def trigger(button, name, settle=1.0, timeout=20):
        before = len(sysex.meas(name))
        frame_before = s.read_oled()
        log(f"LEARN+{button}")
        chord(s, "LEARN", button)
        s.wait(settle)
        s.wait_sd_idle(quiet=1.0, timeout=timeout)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and len(sysex.meas(name)) == before:
            s.wait(0.2)
        s.wait(0.5)
        shot(f"{name}_{button.lower()}")
        new = sysex.meas(name)[before:]
        check(f"LEARN+{button} logs 'MEAS {name}' lines", new, f"{len(new)} lines")
        check(
            f"LEARN+{button} shows a popup (OLED changed)",
            s.read_oled() != frame_before,
        )
        return new

    try:
        s.wait_until(lambda ss: ss.oled_on(), timeout=60)
        s.wait_sd_idle(quiet=1.5, timeout=90)
        s.wait(1.0)
        sysex.send(SYSEX_LOG_ENABLE)
        s.wait(0.5)
        if not s.read_leds().get("SESSION_VIEW"):
            tap(s, "SESSION_VIEW")
            s.wait(1.0)
        check("in session view", s.read_leds().get("SESSION_VIEW"))

        trigger("CV", "mem")
        trigger("SYNTH", "synth")
        trigger("KIT", "kit")
        lines = trigger("MIDI", "kit12")
        check(
            "12 lanes reported",
            sum(" lane " in line and " avg " not in line for line in lines) == 12,
        )
        trigger("KEYBOARD", "pads")
        pads_popup = s.read_oled()
        tap(s, "AFFECT_ENTIRE")  # dismisses the popup
        s.wait(0.5)
        dismissed = s.read_oled()
        chord(s, "LEARN", "SCALE_MODE")
        s.wait(0.5)
        check(
            "AFFECT ENTIRE dismisses the popup, LEARN+SCALE shows it again",
            dismissed != pads_popup and s.read_oled() == pads_popup,
        )
        shot("scale_again")

        before = len(sysex.meas("perform"))
        log("SHIFT+SONG (enter Perform)")
        chord(s, "SHIFT", "SESSION_VIEW")
        s.wait(1.0)
        s.wait_sd_idle(quiet=1.0, timeout=60)
        s.wait(1.0)
        shot("perform")
        new = sysex.meas("perform")[before:]
        check("Perform entry logs 'MEAS perform' lines", new, f"{len(new)} lines")
        check(
            "Perform roster build: 4 lanes",
            sum(" lane " in line and " avg " not in line for line in new) == 4,
        )
        check("Perform view entered", s.read_leds().get("BACK"))
        tap(s, "BACK")
        s.wait(1.0)

        # Save the song, then load it back: exercises the song-load hook.
        log("SAVE song")
        tap(s, "SAVE")
        s.wait(1.0)
        tap(s, "SAVE")
        s.wait(1.0)
        s.wait_sd_idle(quiet=1.0, timeout=30)
        shot("saved")
        before = len(sysex.meas("song"))
        try:
            log("LOAD SONG001")
            s.load_song("/SONGS/SONG001.XML")
            s.wait(1.0)
        except AssertionError as exc:
            log("load_song: " + str(exc))
        shot("song_loaded")
        new = sysex.meas("song")[before:]
        check("song load logs 'MEAS song' lines", new, f"{len(new)} lines")

        try:
            s.assert_no_aborts()
            check("no data/prefetch aborts", True)
        except AssertionError as exc:
            check("no data/prefetch aborts", False, str(exc))
    finally:
        try:
            s.close()
        finally:
            sysex.close()
            if proc.poll() is None:
                proc.terminate()

    with open(os.path.join(args.out, "sysex.log"), "w") as fh:
        fh.write("\n".join(sysex.lines) + "\n")
    with open(os.path.join(args.out, "meas.log"), "w") as fh:
        fh.write("\n".join(line for line in sysex.lines if "MEAS " in line) + "\n")
    if not args.keep_card:
        for path in (card, int_log):
            if os.path.exists(path):
                os.unlink(path)
    failed = [c for c in checks if not c[1]]
    log(f"{len(checks)} checks, {len(failed)} failed; log in {args.out}/sysex.log")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--out",
        required=True,
        help="scratch directory for the card clone, logs and screenshots",
    )
    p.add_argument("--elf", default=DEFAULTS["elf"])
    p.add_argument("--sd-master", default=DEFAULTS["sd_master"])
    p.add_argument("--delugemu", default=DEFAULTS["delugemu"])
    p.add_argument("--qemu", default=DEFAULTS["qemu"])
    p.add_argument(
        "--udp-port",
        type=int,
        default=21998,
        help="host port for the MIDI bridge (and +1)",
    )
    p.add_argument(
        "--midi-port",
        choices=("din", "usb"),
        default="din",
        help="emulated port that carries the log",
    )
    p.add_argument("--keep-card", action="store_true")
    args = p.parse_args()
    if os.path.realpath(args.sd_master) == os.path.realpath(
        os.path.join(args.out, "run.img")
    ):
        sys.exit("refusing to run on the master card image")
    sys.exit(run(args))


if __name__ == "__main__":
    main()
