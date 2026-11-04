"""Headless delugemu check for the Generative Perform POC.

Boots a firmware build in delugemu on a per-run APFS clone of the factory card image, enters Perform with
SHIFT+SONG, plays about 8 bars while capturing the audio to a WAV, turns the Energy knob, stops, and leaves with
BACK. Then it checks the run from the outside:

- the panel: Perform's LEDs after entry (BACK lit, SONG dark) and session view's after BACK (SONG lit);
- the firmware log: no data or prefetch aborts;
- the OLED keeps changing while playing (bar.beat advances), so the firmware didn't freeze;
- the audio: level over time, onsets, the tempo they imply, and a low-band beat profile folded at 130 BPM that
  shows the kick on the beat and the bass on the off-beat 8th.

Stdlib only. Needs a delugemu source checkout (for pylib and scripts/run.sh) and a QEMU binary, e.g. the nightly
bundle's. Never runs on the master card image: it clones it into the output directory first.

    python3 scripts/generative/perform_poc_emu.py --out /path/to/scratch/run1
    python3 scripts/generative/perform_poc_emu.py --analyse-only /path/to/scratch/run1/perform.wav
"""

import argparse
import array
import itertools
import math
import os
import shutil
import struct
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULTS = {
    "elf": os.path.join(REPO, "build", "Release", "deluge.elf"),
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

BPM = 130.0
BEAT_S = 60.0 / BPM
BAR_S = 4 * BEAT_S
PLAY_BARS_BEFORE_KNOB = 6
PLAY_BARS_AFTER_KNOB = 3
PLAY_BARS_DENSITY_ZERO = 3
ENERGY_DETENTS = 15
DENSITY_DETENTS = -60  # Past 0: Density clamps at 0, which leaves only the kick.
# pylib's firmware order: 4 upper gold (Energy), 5 lower gold (Density).
UPPER_GOLD_ENCODER = 4
LOWER_GOLD_ENCODER = 5


# ---------------------------------------------------------------------------------------------------------------
# Emulator run


def log(msg):
    print("[perform-poc] " + msg, flush=True)


def clone_card(master, dest):
    """APFS clone (copy-on-write) so the master image is never written."""
    if os.path.exists(dest):
        os.unlink(dest)
    subprocess.run(["cp", "-c", master, dest], check=True)


def button_coords(name):
    from delugemu._controls import CONTROLS

    c = CONTROLS[name]
    return c["col"], c["row"]


def chord(s, held, tapped, hold_s=0.15):
    """Hold `held`, tap `tapped`, release `held`, with raw matrix events (independent of the pointer)."""
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


def oled_frame(s):
    return tuple(tuple(row) for row in s.read_oled())


def run_emulator(args):
    os.makedirs(args.out, exist_ok=True)
    sys.path.insert(0, os.path.join(args.delugemu, "pylib"))
    from delugemu import DelugeSession

    card = os.path.join(args.out, "run.img")
    wav = os.path.join(args.out, "perform.wav")
    int_log = os.path.join(args.out, "int.log")
    run_log = os.path.join(args.out, "run.log")
    for path in (wav, int_log):
        if os.path.exists(path):
            os.unlink(path)
    log("cloning card image -> " + card)
    clone_card(args.sd_master, card)

    results = {"checks": []}

    def check(name, ok, detail=""):
        results["checks"].append((name, bool(ok), detail))
        log("{} {} {}".format("PASS" if ok else "FAIL", name, detail))

    env = {"DELUGEMU_QEMU_BIN": args.qemu}
    sock = f"/tmp/dz_perform_{os.getpid()}.sock"
    log("launching " + args.elf)
    s = DelugeSession.launch(
        firmware=args.elf,
        sd=card,
        headless=True,
        int_log=int_log,
        repo_root=args.delugemu,
        env=env,
        artifacts_dir=args.out,
        run_log=run_log,
        sock=sock,
        audio="wav,path=" + wav,
    )
    try:
        s.wait_until(lambda ss: ss.oled_on(), timeout=60)
        s.wait_sd_idle(quiet=1.5, timeout=90)
        s.wait(1.0)
        # A fresh boot opens the new song's clip view. SONG goes to session view, where SHIFT+SONG is free.
        if not s.read_leds().get("SESSION_VIEW"):
            tap(s, "SESSION_VIEW")
            s.wait(1.0)
        check("in session view before entering", s.read_leds().get("SESSION_VIEW"))
        s.screenshot(os.path.join(args.out, "01_session.png"))
        session_frame = oled_frame(s)

        log("SHIFT+SONG")
        chord(s, "SHIFT", "SESSION_VIEW")
        s.wait(0.5)
        s.wait_sd_idle(quiet=1.5, timeout=60)
        s.wait(0.5)
        s.screenshot(os.path.join(args.out, "02_perform_entered.png"))
        leds = s.read_leds()
        entered_frame = oled_frame(s)
        check(
            "perform view entered (BACK LED on, SONG LED off, OLED changed)",
            leds.get("BACK")
            and not leds.get("SESSION_VIEW")
            and entered_frame != session_frame,
            "BACK={} SESSION_VIEW={}".format(
                leds.get("BACK"), leds.get("SESSION_VIEW")
            ),
        )
        with open(os.path.join(args.out, "02_perform_entered.oled.txt"), "w") as fh:
            fh.write(
                s.oled_text()
                if isinstance(s.oled_text(), str)
                else "\n".join(s.oled_text())
            )

        log("PLAY")
        tap(s, "PLAY")
        frames = []
        t_end = time.monotonic() + PLAY_BARS_BEFORE_KNOB * BAR_S
        shot = 0
        while time.monotonic() < t_end:
            s.wait(BEAT_S)
            frames.append(oled_frame(s))
            if shot == 0 and len(frames) == 9:
                s.screenshot(os.path.join(args.out, "03_playing.png"))
                shot = 1
        distinct = len(set(frames))
        check(
            "OLED advances while playing (no freeze)",
            distinct >= 4,
            f"{distinct} distinct OLED frames in {len(frames)} samples",
        )
        check("PLAY LED on while playing", s.read_leds().get("PLAY"))

        log(f"turning upper gold knob (Energy) +{ENERGY_DETENTS}")
        s.encoder(UPPER_GOLD_ENCODER, ENERGY_DETENTS)
        s.wait(0.5)
        s.screenshot(os.path.join(args.out, "04_energy_turned.png"))
        knob_frame = oled_frame(s)
        s.wait(PLAY_BARS_AFTER_KNOB * BAR_S)

        log(f"turning lower gold knob (Density) {DENSITY_DETENTS}")
        s.encoder(LOWER_GOLD_ENCODER, DENSITY_DETENTS)
        s.wait(0.5)
        s.screenshot(os.path.join(args.out, "04b_density_zero.png"))
        s.wait(PLAY_BARS_DENSITY_ZERO * BAR_S)

        log("PLAY (stop)")
        tap(s, "PLAY")
        s.wait(1.5)
        s.screenshot(os.path.join(args.out, "05_stopped.png"))
        check("PLAY LED off after stop", not s.read_leds().get("PLAY"))

        log("BACK")
        tap(s, "BACK")
        s.wait(1.5)
        s.screenshot(os.path.join(args.out, "06_back_in_session.png"))
        leds = s.read_leds()
        check(
            "back in session view (SONG LED on, BACK LED off)",
            leds.get("SESSION_VIEW") and not leds.get("BACK"),
            "BACK={} SESSION_VIEW={}".format(
                leds.get("BACK"), leds.get("SESSION_VIEW")
            ),
        )

        # Still alive after leaving: the panel responds to another enter/leave round trip.
        chord(s, "SHIFT", "SESSION_VIEW")
        s.wait(0.5)
        s.wait_sd_idle(quiet=1.0, timeout=60)
        leds = s.read_leds()
        re_entered = leds.get("BACK") and not leds.get("SESSION_VIEW")
        tap(s, "BACK")
        s.wait(1.0)
        leds = s.read_leds()
        check(
            "second enter/leave round trip works",
            re_entered and leds.get("SESSION_VIEW"),
        )
        s.screenshot(os.path.join(args.out, "07_after_round_trip.png"))
        _ = knob_frame

        try:
            s.assert_no_aborts()
            check("no data/prefetch aborts in the int log", True)
        except AssertionError as exc:
            check("no data/prefetch aborts in the int log", False, str(exc))
    finally:
        s.close()

    # The int log is large; keep only the verdict.
    if os.path.exists(int_log) and not args.keep_int_log:
        os.unlink(int_log)
    if not args.keep_card and os.path.exists(card):
        os.unlink(card)

    log("analysing " + wav)
    ok = analyse(wav, results)
    failed = [c for c in results["checks"] if not c[1]]
    log(f"{len(results['checks'])} checks, {len(failed)} failed")
    return 0 if ok and not failed else 1


# ---------------------------------------------------------------------------------------------------------------
# Audio analysis (stdlib only)


def read_wav(path):
    """Read a WAV, tolerating a header whose sizes were never finalised (QEMU killed mid-run)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file: " + path)
    pos = 12
    fmt = None
    payload = None
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        size = struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", data[body : body + 16])
        elif cid == b"data":
            end = body + size if size and body + size <= len(data) else len(data)
            payload = data[body:end]
            break
        pos = body + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError("WAV has no fmt/data chunk: " + path)
    _, channels, rate, _, _, bits = fmt
    if bits != 16:
        raise ValueError(f"expected 16-bit PCM, got {bits}")
    samples = array.array("h")
    usable = len(payload) - (len(payload) % (2 * channels))
    samples.frombytes(payload[:usable])
    if sys.byteorder != "little":
        samples.byteswap()
    mono = array.array("f", [0.0]) * (len(samples) // channels)
    for i in range(len(mono)):
        acc = 0
        for c in range(channels):
            acc += samples[i * channels + c]
        mono[i] = acc / channels
    return mono, rate


def one_pole_lowpass(x, rate, cutoff):
    a = math.exp(-2.0 * math.pi * cutoff / rate)
    y = array.array("f", [0.0]) * len(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1.0 - a) * v + a * acc
        y[i] = acc
    return y


def frame_rms(x, hop):
    out = []
    for start in range(0, len(x) - hop + 1, hop):
        acc = 0.0
        for v in x[start : start + hop]:
            acc += v * v
        out.append(math.sqrt(acc / hop))
    return out


def to_db(v):
    return 20.0 * math.log10(max(v, 1e-9) / 32768.0)


def analyse(wav, results=None):
    results = results if results is not None else {"checks": []}

    def check(name, ok, detail=""):
        results["checks"].append((name, bool(ok), detail))
        log("{} {} {}".format("PASS" if ok else "FAIL", name, detail))

    mono, rate = read_wav(wav)
    duration = len(mono) / rate
    log(f"WAV: {duration:.1f} s at {rate} Hz")
    hop = rate // 100  # 10 ms frames
    frame_s = hop / rate

    full = frame_rms(mono, hop)
    # Two crude 2nd-order lowpasses: below ~150 Hz is kick and bass fundamentals.
    low_signal = one_pole_lowpass(one_pole_lowpass(mono, rate, 150.0), rate, 150.0)
    low = frame_rms(low_signal, hop)

    peak = max(abs(v) for v in mono) if len(mono) else 0
    log(f"peak {peak:.0f} ({to_db(peak):.1f} dBFS)")

    # Level over time, in 1 s blocks.
    per_second = []
    frames_per_s = round(1.0 / frame_s)
    for i in range(0, len(full), frames_per_s):
        block = full[i : i + frames_per_s]
        per_second.append(math.sqrt(sum(v * v for v in block) / max(1, len(block))))
    log("RMS per second (dBFS): " + " ".join(f"{to_db(v):.0f}" for v in per_second))

    # The playing stretch: frames clearly above the noise floor.
    floor = sorted(full)[len(full) // 10] if full else 0
    threshold = max(floor * 4.0, 30.0)
    loud = [i for i, v in enumerate(full) if v > threshold]
    if not loud:
        check("audio present", False, "no frame above the floor")
        return False
    start, end = loud[0], loud[-1]
    play_s = (end - start) * frame_s
    check(
        "audio present for most of the playing time",
        play_s
        > 0.8
        * (PLAY_BARS_BEFORE_KNOB + PLAY_BARS_AFTER_KNOB + PLAY_BARS_DENSITY_ZERO)
        * BAR_S,
        f"{play_s:.1f} s of sound from {start * frame_s:.2f} s to {end * frame_s:.2f} s",
    )

    # No dropouts inside the playing stretch: every 0.5 s window has some level.
    win = int(0.5 / frame_s)
    gaps = 0
    for i in range(start, end - win, win):
        if max(full[i : i + win]) < threshold:
            gaps += 1
    check(
        "no silent half-second inside the playing stretch",
        gaps == 0,
        f"{gaps} silent windows",
    )

    # Onsets: rises in the level envelope.
    onsets = []
    last = -1000
    for i in range(start + 1, end + 1):
        rise = full[i] - full[i - 1]
        if (
            rise > threshold
            and full[i] > 1.5 * full[i - 1]
            and i - last > int(0.08 / frame_s)
        ):
            onsets.append(i * frame_s)
            last = i
    intervals = [b - a for a, b in itertools.pairwise(onsets)]
    med = sorted(intervals)[len(intervals) // 2] if intervals else 0
    log(
        f"{len(onsets)} onsets, median spacing {med:.3f} s (an 8th at 130 BPM is {BEAT_S / 2:.3f} s, a beat {BEAT_S:.3f} s)"
    )

    # Tempo: put each onset on the nearest 16th of a 130 BPM grid from the first onset, then fit a line through
    # (grid index, time). The slope is the real 16th length; the residual says how well the onsets sit on a grid.
    # The fit grows over 2, 4, 8 s and then all onsets, so a slightly-off clock can't slip the rounding by a 16th.
    if len(onsets) >= 8:
        slope = BEAT_S / 4
        resid = 0.0
        for span in (2.0, 4.0, 8.0, float("inf")):
            ts = [t for t in onsets if t - onsets[0] <= span]
            ks = [round((t - onsets[0]) / slope) for t in ts]
            n = len(ks)
            mk, mt = sum(ks) / n, sum(ts) / n
            den = sum((k - mk) ** 2 for k in ks)
            if den == 0:
                continue
            slope = sum((k - mk) * (t - mt) for k, t in zip(ks, ts)) / den
            resid = math.sqrt(
                sum((t - (mt + slope * (k - mk))) ** 2 for k, t in zip(ks, ts)) / n
            )
        est_bpm = 60.0 / (4 * slope)
        # The emulator's audio clock isn't exact, so allow a few BPM. 10 ms frames limit the residual to ~3 ms.
        check(
            "tempo near 130 BPM, onsets on a 16th grid",
            abs(est_bpm - BPM) < 3.0 and resid < 0.015,
            f"fitted {est_bpm:.2f} BPM, onset residual {resid * 1000:.1f} ms",
        )
    else:
        check(
            "tempo near 130 BPM, onsets on a 16th grid",
            False,
            f"only {len(onsets)} onsets",
        )

    # The last PLAY_BARS_DENSITY_ZERO bars run at Density 0, where the core plays only the kick, on every beat.
    # Skip the first bar of that stretch: steering lands at the next undecided step, and the knob turn's wall-clock
    # time only roughly matches audio time.
    zero_end = end * frame_s
    zero_start = zero_end - (PLAY_BARS_DENSITY_ZERO - 1) * BAR_S + 0.2
    busy_end = zero_end - PLAY_BARS_DENSITY_ZERO * BAR_S - 0.5
    busy_start = busy_end - 2 * BAR_S
    zero_onsets = [t for t in onsets if zero_start <= t <= zero_end]
    busy_onsets = [t for t in onsets if busy_start <= t <= busy_end]
    zero_iv = [b - a for a, b in itertools.pairwise(zero_onsets)]
    zero_med = sorted(zero_iv)[len(zero_iv) // 2] if zero_iv else 0
    log(
        f"Density 0 stretch {zero_start:.2f}-{zero_end:.2f} s: {len(zero_onsets)} onsets, "
        f"median spacing {zero_med:.3f} s; 2 bars before it: {len(busy_onsets)} onsets"
    )
    check(
        "Density 0 leaves a kick on every beat",
        abs(zero_med - BEAT_S) < 0.04
        and all(abs(iv - BEAT_S) < 0.06 for iv in zero_iv)
        and len(zero_onsets) >= 4 * (PLAY_BARS_DENSITY_ZERO - 1) - 1,
        "intervals " + ", ".join(f"{iv:.3f}" for iv in zero_iv),
    )
    zero_rate = len(zero_onsets) / max(zero_end - zero_start, 1e-3)
    busy_rate = len(busy_onsets) / (2 * BAR_S)
    check(
        "Density knob thins the music (fewer onsets at Density 0)",
        busy_rate > 2 * zero_rate,
        f"{busy_rate:.1f} onsets/s before vs {zero_rate:.1f} onsets/s at Density 0",
    )
    _ = low

    failed = [c for c in results["checks"] if not c[1]]
    return not failed


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--out", help="scratch directory for the card clone, WAV, screenshots and logs"
    )
    p.add_argument("--elf", default=DEFAULTS["elf"])
    p.add_argument(
        "--sd-master",
        default=DEFAULTS["sd_master"],
        help="card image to clone (never written)",
    )
    p.add_argument(
        "--delugemu",
        default=DEFAULTS["delugemu"],
        help="delugemu source checkout (pylib, run.sh)",
    )
    p.add_argument(
        "--qemu",
        default=DEFAULTS["qemu"],
        help="qemu-system-arm with the deluge machine",
    )
    p.add_argument(
        "--keep-card",
        action="store_true",
        help="keep the cloned card image after the run",
    )
    p.add_argument(
        "--keep-int-log",
        action="store_true",
        help="keep QEMU's interrupt log after the run",
    )
    p.add_argument(
        "--analyse-only", metavar="WAV", help="only analyse an existing capture"
    )
    args = p.parse_args()

    if args.analyse_only:
        return 0 if analyse(args.analyse_only) else 1
    if not args.out:
        p.error("--out is required")
    for key in ("elf", "sd_master", "qemu"):
        if not os.path.exists(getattr(args, key)):
            p.error(f"not found: {getattr(args, key)}")
    if not shutil.which("cp"):
        p.error("cp not found")
    return run_emulator(args)


if __name__ == "__main__":
    sys.exit(main())
