"""Boot the firmware in delugemu, load SONGS/CAPTURE.XML, play it, and capture the audio.

Two modes:

  realtime  The emulator runs against the host clock, as it does for a person
            using it. Input is sent on wall-clock timers through pylib's
            load_song and press. Shows how close to real time the host gets
            and whether audio drops out.

  icount    The CPU runs on an instruction-counted virtual clock
            (-icount shift=N,sleep=off) and starts paused (-S). The script
            steps the VM in virtual time with run_until and sends every
            button press at a fixed virtual time. Two runs should see the
            same guest, so the audio can be compared run to run.

Audio comes from delugemu's raw SSIF capture (DELUGEMU_SSIF_DUMP): the
S32LE stereo frames the firmware renders, before the emulator's output
resampler. v1.3.0 has no USB audio class, so --usb-audio can't be used.

Usage:
  capture.py --firmware deluge.elf --card card.img --delugemu ~/source/delugemu \\
             --mode icount --seconds 60 --runs 2 --out OUTDIR

Set DELUGEMU_QEMU_BIN to the bundle's qemu-system-arm, as run.sh expects.
Writes OUTDIR/run<N>.wav per run and OUTDIR/report.json.
"""

import argparse
import hashlib
import json
import os
import platform
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave

RATE = 44100
FRAME = 8  # S32LE stereo
MS = 1_000_000  # ns per ms
SEC = 1_000_000_000  # ns per s


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--firmware", required=True)
    p.add_argument(
        "--card",
        required=True,
        help="FAT image with SONGS/CAPTURE.XML (attached read-only)",
    )
    p.add_argument(
        "--delugemu",
        required=True,
        help="delugemu source checkout (for pylib and scripts/run.sh)",
    )
    p.add_argument("--mode", choices=("realtime", "icount"), default="icount")
    p.add_argument(
        "--shift", type=int, default=2, help="icount shift: 2^shift ns per instruction"
    )
    p.add_argument("--seconds", type=float, default=60.0, help="song time to capture")
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--out", required=True)
    return p.parse_args()


# -- host CPU of the QEMU process -------------------------------------------


def descendants(pid):
    """pid and every process below it (run.sh starts QEMU as a child)."""
    out = subprocess.run(
        ["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True, check=False
    ).stdout
    kids = {}
    pairs = [
        tuple(int(x) for x in l.split()) for l in out.strip().splitlines() if l.strip()
    ]
    for c, parent in pairs:
        kids.setdefault(parent, []).append(c)
    todo, seen = [pid], []
    while todo:
        p = todo.pop()
        seen.append(p)
        todo += kids.get(p, [])
    return seen


def cpu_seconds(pids):
    """User + system CPU seconds of the live processes in pids."""
    total = 0.0
    if os.path.isdir("/proc"):
        tick = os.sysconf("SC_CLK_TCK")
        for p in pids:
            try:
                with open(f"/proc/{p}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
                total += (int(fields[11]) + int(fields[12])) / tick
            except OSError:
                pass
        return total
    out = subprocess.run(
        ["ps", "-o", "time=", "-p", ",".join(str(p) for p in pids)],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    for t in out.split():
        # macOS: [[dd-]hh:]mm:ss.ss
        days = 0
        if "-" in t:
            d, t = t.split("-", 1)
            days = int(d)
        parts = [float(x) for x in t.split(":")]
        secs = 0.0
        for x in parts:
            secs = secs * 60 + x
        total += days * 86400 + secs
    return total


class Clock:
    """Wall-clock and QEMU-CPU stamps at phase boundaries."""

    def __init__(self, proc_pid):
        self.pid = proc_pid
        self.t0 = time.monotonic()
        self.marks = []

    def mark(self, name):
        self.marks.append(
            (name, time.monotonic() - self.t0, cpu_seconds(descendants(self.pid)))
        )

    def phases(self):
        out, prev = {}, ("start", 0.0, 0.0)
        for m in self.marks:
            out[m[0]] = {
                "wall_s": round(m[1] - prev[1], 3),
                "cpu_s": round(m[2] - prev[2], 3),
            }
            prev = m
        return out


# -- the two drivers ----------------------------------------------------------


def boot_and_play_realtime(s, clock, seconds, dump):
    s.wait_until(lambda ss: ss.oled_on(), timeout=60)
    s.wait(
        5.0
    )  # the card mounts and the default song is set up after the OLED comes on
    s.wait_sd_idle(quiet=1.0, timeout=60)
    clock.mark("boot")
    s.load_song("CAPTURE")
    clock.mark("load")
    start = os.path.getsize(dump)
    s.press("PLAY")
    want = start + int(seconds * RATE) * FRAME
    deadline = time.monotonic() + seconds * 20 + 60
    while os.path.getsize(dump) < want:
        if time.monotonic() > deadline:
            raise TimeoutError(f"render did not reach {seconds:.0f} s of audio")
        time.sleep(0.05)
    clock.mark("render")
    return start, None


def boot_and_play_icount(s, clock, seconds, dump):
    """Every input at a fixed virtual time, and every wait a fixed virtual span,
    so two runs give the guest the same history."""
    t = 0

    def run_to(ns):
        nonlocal t
        while t < ns:
            t = min(ns, t + 100 * MS)
            s.run_until(t, timeout=600)

    def tap(col, row):
        s.button(col, row, True)
        run_to(t + 150 * MS)
        s.button(col, row, False)

    # Boot: step until the OLED is on, then 5 s more for the card and default song.
    while not s.oled_on():
        run_to(t + 100 * MS)
        if t > 60 * SEC:
            raise TimeoutError("OLED never came on")
    oled_at = t
    run_to(t + 5 * SEC)
    clock.mark("boot")

    # Load: LOAD opens the browser on the only song on the card, SELECT loads it.
    tap(6, 1)  # LOAD
    run_to(t + 1500 * MS)
    tap(4, 3)  # SELECT encoder push
    quiet_since, last = t, s.sd_command_count()
    while t - quiet_since < 1 * SEC:
        run_to(t + 100 * MS)
        c = s.sd_command_count()
        if c != last:
            last, quiet_since = c, t
        if t > 120 * SEC:
            raise TimeoutError("song load never went quiet")
    clock.mark("load")

    s.button(8, 3, True)  # PLAY
    start = os.path.getsize(dump)
    play_at = t
    run_to(t + 150 * MS)
    s.button(8, 3, False)
    run_to(play_at + int(seconds * SEC) + 200 * MS)
    clock.mark("render")
    return start, {
        "oled_on_ns": oled_at,
        "play_ns": play_at,
        "end_ns": t,
        "vclock_ns": s.vclock_ns(),
    }


# -- audio checks -----------------------------------------------------------


def analyse(pcm):
    n = len(pcm) // 4
    v = struct.unpack(f"<{n}i", pcm[: n * 4])
    left, right = v[0::2], v[1::2]
    # Longest run of exact digital silence on both channels, in frames.
    longest = run = 0
    for a, b in zip(left, right):
        if a == 0 and b == 0:
            run += 1
            longest = max(longest, run)
        else:
            run = 0
    # Loudness per second, to spot a stall or dropout.
    per_sec = []
    for i in range(0, len(left), RATE):
        seg = left[i : i + RATE : 4]
        per_sec.append(
            round((sum(x * x for x in seg) / max(1, len(seg))) ** 0.5 / 2**31, 4)
        )
    peak = max(max(abs(x) for x in left), max(abs(x) for x in right)) / 2**31
    return {
        "frames": len(left),
        "longest_silence_ms": round(longest * 1000 / RATE, 2),
        "peak": round(peak, 4),
        "rms_per_second": per_sec,
    }


def compare(a, b):
    if a == b:
        return {"identical": True}
    n = min(len(a), len(b)) // FRAME
    first = next(
        (
            i
            for i in range(n)
            if a[i * FRAME : (i + 1) * FRAME] != b[i * FRAME : (i + 1) * FRAME]
        ),
        None,
    )
    va = struct.unpack(f"<{n * 2}i", a[: n * FRAME])
    vb = struct.unpack(f"<{n * 2}i", b[: n * FRAME])
    diffs = [abs(x - y) for x, y in zip(va, vb)]
    differing = sum(1 for d in diffs if d)
    # Is run 2 run 1 shifted by a few frames? Look for an exact copy of 64 frames
    # of run 1 (taken 1 s in) anywhere in run 2.
    lag = None
    off = min(RATE, n // 2) * FRAME
    sig = a[off : off + 64 * FRAME]
    if len(sig) == 64 * FRAME:
        pos = b.find(sig)
        while pos != -1 and pos % FRAME:
            pos = b.find(sig, pos + 1)
        if pos != -1:
            lag = (pos - off) // FRAME
    return {
        "identical": False,
        "length_frames": [len(a) // FRAME, len(b) // FRAME],
        "first_diff_frame": first,
        "first_diff_s": None if first is None else round(first / RATE, 4),
        "samples_differing": differing,
        "samples_compared": len(diffs),
        "max_abs_diff": max(diffs) / 2**31 if diffs else 0,
        "lag_frames_if_shifted_copy": lag,
    }


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(4)
        w.setframerate(RATE)
        w.writeframes(pcm)


# -- one run ----------------------------------------------------------------


def one_run(args, idx, work):
    sys.path.insert(0, os.path.join(args.delugemu, "pylib"))
    from delugemu import DelugeSession

    dump = os.path.join(work, f"run{idx}.s32")
    stats = os.path.join(work, f"run{idx}.ssif-stats")
    env = {"DELUGEMU_SSIF_DUMP": dump, "DELUGEMU_SSIF_STATS": stats}
    extra = ["-snapshot"]
    if args.mode == "icount":
        extra = ["-icount", f"shift={args.shift},sleep=off", "-S"] + extra
    sock = os.path.join(tempfile.gettempdir(), f"dzcap{os.getpid()}_{idx}.sock")

    t_launch = time.monotonic()
    s = DelugeSession.launch(
        firmware=os.path.abspath(args.firmware),
        sd=os.path.abspath(args.card),
        repo_root=args.delugemu,
        env=env,
        sock=sock,
        run_log=os.path.join(work, f"run{idx}.log"),
        extra_qemu=tuple(extra),
    )
    clock = Clock(s.proc.pid)
    clock.t0 = t_launch
    try:
        clock.mark("launch")
        drive = (
            boot_and_play_icount if args.mode == "icount" else boot_and_play_realtime
        )
        start, vtimes = drive(s, clock, args.seconds, dump)
        # The panel at the end of the render: should show the CAPTURE song playing.
        s.screenshot(os.path.join(work, f"run{idx}.png"))
        cpu_total = cpu_seconds(descendants(s.proc.pid))
    finally:
        s.close()

    with open(dump, "rb") as f:
        f.seek(start)
        pcm = f.read(int(args.seconds * RATE) * FRAME)
    wav = os.path.join(args.out, f"run{idx}.wav")
    write_wav(wav, pcm)

    phases = clock.phases()
    render = phases["render"]["wall_s"]
    report = {
        "wav": os.path.basename(wav),
        "sha256": hashlib.sha256(pcm).hexdigest(),
        "phases": phases,
        "qemu_cpu_s": round(cpu_total, 3),
        "realtime_factor": round(args.seconds / render, 3) if render else None,
        "render_cpu_per_wall": round(phases["render"]["cpu_s"] / render, 3)
        if render
        else None,
        "audio": analyse(pcm),
        "dump_bytes_total": os.path.getsize(dump),
    }
    if vtimes:
        report["virtual"] = vtimes
        # Frames the firmware rendered between Play and the end, against the virtual time that passed.
        span = (vtimes["end_ns"] - vtimes["play_ns"]) / SEC
        report["virtual"]["frames_expected_after_play"] = int(span * RATE)
        report["virtual"]["frames_rendered_after_play"] = (
            os.path.getsize(dump) - start
        ) // FRAME
    try:
        with open(stats) as f:
            lines = [l.strip() for l in f if l.strip()]
        report["ssif_stats_tail"] = lines[-5:]
        report["ssif_stats_lines"] = len(lines)
        # One line per virtual second: "production N B/s (P% of 352800), underruns U/s, ..."
        under = [
            int(m.group(1))
            for m in (re.search(r"underruns (\d+)/s", l) for l in lines)
            if m
        ]
        prod = [
            float(m.group(1))
            for m in (re.search(r"\(([\d.]+)% of", l) for l in lines)
            if m
        ]
        report["ssif_underruns_total"] = sum(under)
        report["ssif_seconds_with_underruns"] = sum(1 for u in under if u)
        report["ssif_production_min_pct"] = min(prod) if prod else None
        report["ssif_stats_file"] = os.path.basename(stats)
    except OSError:
        pass
    os.remove(dump)
    return report, pcm


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="dzcap")
    report = {
        "mode": args.mode,
        "seconds": args.seconds,
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "cpus": os.cpu_count(),
            "python": platform.python_version(),
        },
        "runs": [],
    }
    if args.mode == "icount":
        report["icount_shift"] = args.shift
    pcms = []
    for i in range(1, args.runs + 1):
        r, pcm = one_run(args, i, work)
        report["runs"].append(r)
        pcms.append(pcm)
        print(
            f"run {i}: "
            + json.dumps({k: r[k] for k in ("phases", "realtime_factor", "sha256")}),
            flush=True,
        )
        for name in os.listdir(work):
            if name.startswith(f"run{i}."):
                os.replace(os.path.join(work, name), os.path.join(args.out, name))
    if len(pcms) > 1:
        report["compare_run1_run2"] = compare(pcms[0], pcms[1])
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report.get("compare_run1_run2", {}), indent=2))


if __name__ == "__main__":
    main()
