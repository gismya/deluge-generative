"""The spec review's hardware tests (H1-H14, docs/research/spec-review-technical.md on research/spec-review), driven
over the sysex test port.

Needs a build with ENABLE_SYSEX_TEST, which compiles in the POC probe (generative/debug/poc_probe.cpp) and the stand-in
probes (generative/debug/hw_probe.cpp). The probes take "probe <name> [a] [b] [c]" lines and report test events. Run
with ./dbt exec:

    ./dbt exec "python3 scripts/generative/spec_hw_tests.py h7 --log docs/spec-hw-tests/logs/h7.log"

Each test prints a summary and writes every event and state line it saw, with host time, to the log.
"""

import argparse
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import deluge_probe as dp
import poc_hw_tests as poc
from poc_hw_tests import Rig, bars, parse_fields, summarize_bars


def probe(rig, *words):
    rig.inject("probe " + " ".join(str(w) for w in words))


def wait_event(rig, pattern, since, timeout=30.0):
    """Pumps until an event matching `pattern` arrives after mark `since`. Returns its fields, or None."""
    end = time.time() + timeout
    while time.time() < end:
        found = rig.events(pattern, since)
        if found:
            return parse_fields(found[-1][1])
        rig.pump(0.1)
    print(f"!! no event {pattern!r} within {timeout:g} s", flush=True)
    return None


def perform_playing(rig, bpm_x10, settle=3):
    """A new song, Perform entered, the tempo set and the transport running.

    The MEAS build shows its Perform entry summary as a popup for 30 s. The popup covers the OLED lines that change,
    so tests that time OLED frames wait for it to go (settle=35).
    """
    poc.t_newsong(rig, None)
    rig.fresh_perform()
    probe(rig, "bpm", bpm_x10)
    rig.idle()
    rig.play(True)
    rig.pump(settle)


# ---- H7: pad rows and OLED each 16th ----


def t_h7(rig, a):
    perform_playing(rig, 1750, settle=35)
    probe(rig, "oled_state")
    configs = a.configs or [
        (0, 0, 0),  # baseline: the POC's playhead and an OLED frame each beat
        (1, 0, 0),
        (4, 0, 0),
        (8, 0, 0),
        (1, 0, 1),
        (8, 0, 1),
        (8, 1, 1),
    ]
    results = []
    rng = random.Random(7)
    for rows, side, oled in configs:
        probe(rig, "redraw", rows, side, oled)
        rig.idle()
        m = rig.mark()
        end = time.time() + a.seconds
        while time.time() < end:
            # One injected pad press and release at a random point, a few times a second.
            # No "wait" between them: the probe times each edge from its arrival, so a wait would count.
            rig.inject("pad 5 3 down")
            rig.pump(0.04)
            rig.inject("pad 5 3 up")
            rig.pump(rng.uniform(0.2, 0.6))
        m2 = rig.mark()
        probe(rig, "redraw_report")
        h7 = wait_event(rig, r": h7 rows", m2)
        row = wait_event(rig, r": h7row ", m2)
        oledf = wait_event(rig, r": h7oled ", m2)
        press = wait_event(rig, r": h7press ", m2)
        rowreq = wait_event(rig, r": h7rowreq ", m2) or {}
        rig.screen(f"h7 rows {rows} side {side} oled {oled}")
        b = bars(rig, m)
        late_renders = sum(v.get("v_lr", 0) for v in b.values())
        gap = max((v.get("v_gap", 0) for v in b.values()), default=0)
        if not (h7 and row and oledf and press):
            print("!! missing report for", rows, side, oled)
            continue
        busy_pct = 100.0 * h7["busy"] / max(1, h7["elapsed"])
        bytes_pct = (
            100.0 * h7["bytes"] * 50 / max(1, h7["elapsed"])
        )  # 200 kbaud, 10 bits a byte = 50 us
        r = {
            "rows": rows,
            "side": side,
            "oled_step": oled,
            "steps": h7["steps"],
            "step_us": h7["stepus"],
            "busy_pct": busy_pct,
            "bytes_pct": bytes_pct,
            "row_n": row["n"],
            "row_avg": row["avg"],
            "row_hi": row["hi"],
            "row_late": row["late"],
            "missed": row["missed"],
            "deferred": row["deferred"],
            "q_hi": row["q_hi"],
            "oled_n": oledf["n"],
            "oled_avg": oledf["avg"],
            "oled_hi": oledf["hi"],
            "oled_req_avg": oledf.get("ravg", 0),
            "oled_req_hi": oledf.get("rhi", 0),
            "row_req_avg": rowreq.get("avg", 0),
            "row_req_hi": rowreq.get("hi", 0),
            "skipped": rowreq.get("skipped", 0),
            "frames": oledf["frames"],
            "press_n": press["n"],
            "press_avg": press["avg"],
            "press_hi": press["hi"],
            "late_renders": late_renders,
            "gap": gap,
        }
        results.append(r)
        print(
            f"rows {rows} side {side} oled/step {oled}: {r['steps']} steps of {r['step_us'] / 1000:.1f} ms; "
            f"UART busy {busy_pct:.1f}% ({bytes_pct:.1f}% by bytes); rows n {r['row_n']} avg {r['row_avg'] / 1000:.1f} "
            f"max {r['row_hi'] / 1000:.1f} ms (from request avg {r['row_req_avg'] / 1000:.1f} max {r['row_req_hi'] / 1000:.1f}), "
            f"late {r['row_late']}, missed {r['missed']}, skipped {r['skipped']}, deferred {r['deferred']}; "
            f"OLED n {r['oled_n']} avg {r['oled_avg'] / 1000:.1f} max {r['oled_hi'] / 1000:.1f} ms "
            f"(from request avg {r['oled_req_avg'] / 1000:.1f} max {r['oled_req_hi'] / 1000:.1f}; {r['frames']} frames); "
            f"press n {r['press_n']} avg {r['press_avg'] / 1000:.2f} max {r['press_hi'] / 1000:.2f} ms; "
            f"late renders {late_renders}, max gap {gap} us",
            flush=True,
        )
    probe(rig, "redraw", -1, 0, 0)
    rig.play(False)
    rig.pump(2)
    rig.note("results " + repr(results))


def t_oledstall(rig, a):
    """The OLED after Perform entry and play start: samples the OLED driver's state every 2 s."""
    perform_playing(rig, 1750)
    probe(rig, "redraw", 0, 0, 1)
    m = rig.mark()
    end = time.time() + a.seconds
    while time.time() < end:
        probe(rig, "oled_state")
        rig.pump(2)
    for t, line in rig.events(r"oled_state", m):
        print(line)
    probe(rig, "redraw", -1, 0, 0)
    rig.play(False)


# ---- H14: the driver's clock under an external MIDI clock ----

START, CONTINUE, STOP, CLOCK, SPP = 0xFA, 0xFB, 0xFC, 0xF8, 0xF2


class ClockSender(threading.Thread):
    """Sends a plan of MIDI real-time messages to the Deluge on its first USB port, timed on the host.

    plan: list of ("start",) ("continue",) ("stop",) ("spp", sixteenths) ("clocks", n) ("pause", seconds).
    """

    def __init__(self, bpm, plan):
        super().__init__(daemon=True)
        import rtmidi

        self.out = rtmidi.MidiOut()
        ports = self.out.get_ports()
        first = [i for i, p in enumerate(ports) if "deluge" in p.lower()]
        if not first:
            raise LookupError("No Deluge MIDI port")
        self.port_name = ports[first[0]]
        self.out.open_port(first[0])
        self.interval = 60.0 / bpm / 24
        self.plan = plan
        self.log = []  # (host time, what)
        self.late = []  # how late each clock went out, s

    def _at(self, t):
        while True:
            left = t - time.perf_counter()
            if left <= 0:
                return -left
            time.sleep(min(left, 0.0005) if left < 0.003 else left - 0.002)

    def run(self):
        t = time.perf_counter() + 0.05
        for step in self.plan:
            kind = step[0]
            if kind == "clocks":
                for _ in range(step[1]):
                    self.late.append(self._at(t))
                    self.out.send_message([CLOCK])
                    t += self.interval
            elif kind == "pause":
                t += step[1]
            else:
                self._at(t)
                if kind == "start":
                    self.out.send_message([START])
                elif kind == "continue":
                    self.out.send_message([CONTINUE])
                elif kind == "stop":
                    self.out.send_message([STOP])
                elif kind == "spp":
                    v = step[1]
                    self.out.send_message([SPP, v & 0x7F, (v >> 7) & 0x7F])
                self.log.append((time.time(), step))
        self.out.close_port()


def h14_scenario(rig, bpm, internal_bpm_x10, label):
    rig.play(False)
    probe(rig, "bpm", internal_bpm_x10)
    probe(rig, "h14", 1)
    rig.idle()
    rig.pump(0.5)
    m = rig.mark()
    bar = 96
    plan = [
        ("start",),
        ("clocks", 8 * bar),
        ("spp", 2 * 16),  # mid-play SPP back to bar 3
        ("clocks", 4 * bar),
        ("stop",),
        ("pause", 1.0),
        ("spp", 4 * 16),  # Continue from bar 5
        ("continue",),
        ("clocks", 4 * bar),
        ("stop",),
    ]
    sender = ClockSender(bpm, plan)
    sender.start()
    while sender.is_alive():
        rig.pump(0.2)
    rig.pump(1.5)
    m2 = rig.mark()
    probe(rig, "h14_report")
    rep = wait_event(rig, r": h14rep ", m2)
    ev = wait_event(rig, r": h14ev ", m2)
    records = []
    n = rep["records"] if rep else 0
    for first in range(0, n, 12):
        m3 = rig.mark()
        probe(rig, "h14_dump", first, 12)
        want = min(12, n - first)
        end = time.time() + 10
        while time.time() < end and len(rig.events(r": h14r ", m3)) < want:
            rig.pump(0.1)
        records += [parse_fields(line) for _, line in rig.events(r": h14r ", m3)]
    probe(rig, "h14", 0)
    rig.idle()
    clocks = [line for _, line in rig.events(r": h14 clock ", m)]
    false_jumps = [line for _, line in rig.events(r": h14 false jump", m)]
    b = bars(rig, m)
    late_clock = max(sender.late) * 1000 if sender.late else 0
    print(
        f"[{label}] external {bpm} BPM, internal {internal_bpm_x10 / 10:g} BPM, host clock jitter max {late_clock:.2f} ms"
    )
    print(f"[{label}] report: {rep}  events: {ev}")
    for line in clocks:
        print(f"[{label}]   {line}")
    for line in false_jumps:
        print(f"[{label}]   {line}")
    # The first hook calls after each clock event: delta and magnitude.
    for r in records:
        if r.get("fl", 0) & 1 or r.get("fl", 0) & 2:
            print(f"[{label}]   record {r}")
    mags = sorted({r["mag"] for r in records})
    print(f"[{label}] magnitudes seen {mags}; {len(records)} records")
    summarize_bars(b, label)
    rig.note(f"h14 {label} records {records!r}")
    return {
        "label": label,
        "report": rep,
        "events": ev,
        "false": len(false_jumps),
        "mags": mags,
    }


def t_h14(rig, a):
    poc.t_newsong(rig, None)
    rig.fresh_perform()
    m = rig.mark()
    probe(rig, "clockcfg", 1, 1)
    was = wait_event(rig, r": clockcfg was", m)
    results = []
    try:
        for bpm, internal, label in (
            (70, 1300, "70 vs 130"),
            (120, 1300, "120 vs 130"),
            (180, 1300, "180 vs 130"),
            (180, 900, "180 vs 90"),
        ):
            results.append(h14_scenario(rig, bpm, internal, label))
    finally:
        if was:
            probe(rig, "clockcfg", was["magmatch"], was["clockin"])
        rig.idle()
    rig.note("results " + repr(results))
    for r in results:
        print(r)


# ---- H8: input during card loads ----

H8_KINDS = {
    1: "hid",
    2: "deferred",
    3: "encoder",
    4: "knob",
    5: "action",
    6: "build_start",
    7: "build_end",
}
WAIT_PAD = (15, 7)
LANES = 16


def send_now(rig, line):
    """Queues an event line without waiting for the status reply, which a card load can hold up."""
    port = rig.port
    seq = port._next_seq()
    port.link.send(
        dp.HDR + [0x02, dp.INJECT, seq] + port.key + list(line.encode("ascii")) + [0xF7]
    )


def dump_records(rig, command, count_pattern, record_pattern):
    m = rig.mark()
    probe(rig, command, 0, 12)
    n = wait_event(rig, count_pattern, m)
    total = next(iter(n.values())) if n else 0
    for first in range(12, total, 12):
        probe(rig, command, first, 12)
    end = time.time() + 15
    while time.time() < end and len(rig.events(record_pattern, m)) < total:
        rig.pump(0.1)
    return [parse_fields(line) for _, line in rig.events(record_pattern, m)]


def h8_run(rig, offset, with_wait, label):
    probe(rig, "h8", 1)
    rig.idle()
    m = rig.mark()
    # The host times the inputs from when it sends the build command.
    # The test port holds back everything after an encoder turn until it has fed the turn's edges (0.16 s each), so
    # the knob goes last.
    plan = []
    if with_wait:
        plan += [
            (0.010, f"pad {WAIT_PAD[0]} {WAIT_PAD[1]} down"),
            (0.015, f"pad {WAIT_PAD[0]} {WAIT_PAD[1]} up"),
        ]
    plan += [
        (0.020, "pad 3 2 down"),
        (0.025, "pad 3 2 up"),
        (0.030, "button BACK down"),
        (0.035, "button BACK up"),
        (0.040, "pad 4 2 down"),
        (0.045, "pad 4 2 up"),
        (0.050, "pad 5 2 down"),
        (0.055, "pad 5 2 up"),
        (0.060, "encoder 4 1"),
    ]
    probe(rig, "kitprep", offset, LANES)
    prep = wait_event(rig, r": kitprep ", m)
    rig.pump(0.3)
    t0 = time.perf_counter()
    send_now(rig, f"probe kitbuild {offset} {LANES}")
    for at, line in plan:
        while time.perf_counter() - t0 < at:
            time.sleep(0.0005)
        send_now(rig, line)
    rig.pump(3)
    built = wait_event(rig, r": kitbuild ", m, 20)
    rig.idle()
    recs = dump_records(rig, "h8_dump", r": h8n ", r": h8r ")
    probe(rig, "h8", 0)
    rig.idle()
    rig.note(f"h8 {label} kitprep {prep} kitbuild {built} records {recs!r}")
    out = analyse_h8(recs, built, label, with_wait)
    out["next"] = built["next"] if built else offset + LANES
    out["err"] = built["err"] if built else None
    return out


def analyse_h8(recs, built, label, with_wait):
    start = next((r["us"] for r in recs if r["k"] == 6), None)
    end = next((r["us"] for r in recs if r["k"] == 7), None)
    out = {
        "label": label,
        "build_ms": (end - start) / 1000
        if start is not None and end is not None
        else None,
    }
    act, knob, wait_ran, deferred = [], [], [], []
    for r in recs:
        kind = r["k"]
        lat = (r["us"] - r["arr"]) / 1000 if r["arr"] else None
        if kind == 1 and r["vel"] != 0:  # a press's handler returned
            during = start is not None and end is not None and start <= r["arr"] <= end
            act.append((r["v"], lat, during, r["fl"]))
        elif kind == 4:
            during = start is not None and end is not None and start <= r["arr"] <= end
            knob.append((r["v"], lat, during))
        elif kind == 5 and r["v"] == 2:
            wait_ran.append(r["us"])
        elif kind == 2:
            deferred.append(r["v"])
    out.update(
        act=act, knob=knob, wait_ran=wait_ran, deferred=deferred, start=start, end=end
    )
    print(
        f"[{label}] {built}  build {out['build_ms']} ms (from {start} to {end} us)",
        flush=True,
    )
    for v, lat, during, fl in act:
        print(
            f"[{label}]   press value {v}: {lat:.1f} ms after arrival, arrived during build {during}, flags {fl}"
        )
    for v, lat, during in knob:
        print(
            f"[{label}]   knob {v}: first action {lat:.1f} ms after the turn arrived, during build {during}"
        )
    for us in wait_ran:
        print(
            f"[{label}]   Wait-class pad ran at {us} us ({'after' if end and us > end else 'DURING'} the build)"
        )
    print(f"[{label}]   deferred edges: {deferred}")
    return out


def t_h8(rig, a):
    perform_playing(rig, 1300)
    results = []
    offset = a.offset
    for i in range(a.repeat):
        for with_wait in (False, True):
            for _attempt in range(
                4
            ):  # a failed build (a bad sample) is retried with fresh samples
                label = f"{'wait first' if with_wait else 'no wait'} #{i + 1} (from sample {offset})"
                r = h8_run(rig, offset, with_wait, label)
                offset = r["next"]
                rig.pump(1)
                if r["err"] == 0:
                    results.append(r)
                    break
    rig.play(False)
    rig.pump(1)
    rig.note("results " + repr(results))


TESTS = {
    "h7": t_h7,
    "h8": t_h8,
    "h14": t_h14,
    "oledstall": t_oledstall,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("test", choices=TESTS)
    ap.add_argument("--log")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument(
        "--offset", type=int, default=0, help="H8: the first drum sample to build from"
    )
    ap.add_argument(
        "--configs",
        type=lambda v: [tuple(int(x) for x in c.split(",")) for c in v.split(";")],
        help='H7 configurations "rows,sidebar,oled_each_step;...", e.g. "0,0,0;8,0,1"',
    )
    a = ap.parse_args()
    rig = Rig(a.log)
    try:
        TESTS[a.test](rig, a)
    finally:
        print("final state", rig.state())
        rig.port.subscribe(False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
