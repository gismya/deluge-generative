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
from itertools import pairwise

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
    probe(rig, "clockcfg", 1, 1, -1)
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
            probe(rig, "clockcfg", was["magmatch"], was["clockin"], -1)
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


def paced_dump(rig, command, count_pattern, record_pattern, timeout=20):
    """Asks for records 12 at a time, each chunk once the last has arrived: the firmware's event queue holds 16."""
    m = rig.mark()
    probe(rig, command, 0, 12)
    n = wait_event(rig, count_pattern, m)
    total = next(iter(n.values())) if n else 0
    end = time.time() + timeout
    while time.time() < end and len(rig.events(record_pattern, m)) < min(12, total):
        rig.pump(0.05)
    for first in range(12, total, 12):
        probe(rig, command, first, 12)
        want = min(first + 12, total)
        end = time.time() + timeout
        while time.time() < end and len(rig.events(record_pattern, m)) < want:
            rig.pump(0.05)
    lines = [line for _, line in rig.events(record_pattern, m)]
    if len(lines) < total:
        print(f"!! {command}: {len(lines)} of {total} records arrived", flush=True)
    return lines


def dump_records(rig, command, count_pattern, record_pattern):
    return [
        parse_fields(line)
        for line in paced_dump(rig, command, count_pattern, record_pattern)
    ]


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


# ---- Loading a user song through the LOAD browser ----


def browser_name(rig):
    m = rig.mark()
    probe(rig, "browser_name")
    end = time.time() + 5
    while time.time() < end:
        found = rig.events(r": browser ", m)
        if found:
            return found[-1][1].split(": browser ", 1)[1].rsplit("|", 1)[0]
        rig.pump(0.05)
    return None


def load_song(rig, name, max_steps=300):
    """LOAD in session view, then the select encoder until the browser shows `name`, then SELECT."""
    rig.play(False)
    if (
        rig.state()["ui"] == "load_song"
    ):  # a browser left open: leave it without loading anything
        rig.press("BACK")
        rig.wait_for("ui", "session", 10)
    rig.to_session()
    rig.press("LOAD")
    ok, _ = rig.wait_for("ui", "load_song", 10)
    if not ok:
        raise RuntimeError("the LOAD browser didn't open")
    rig.pump(1.0)
    seen = []
    direction = 1
    for _ in range(max_steps):
        current = browser_name(rig)
        if current is None:
            raise RuntimeError("no browser name")
        if current.lower() in (name.lower(), name.lower() + ".xml"):
            break
        if current in seen[:-1] and direction == 1:
            direction = -1  # wrapped round without finding it: try the other way
        seen.append(current)
        rig.turn(3, direction)
        rig.idle()
        rig.pump(0.15)
    else:
        raise RuntimeError(f"{name} not found in the browser")
    rig.screen(f"browser at {name}")
    t0 = time.time()
    rig.press("SELECT_ENC")
    rig.pump(1.0)

    def state_or_busy():
        try:
            return rig.port.state()
        except TimeoutError:  # a long load blocks the test port: keep asking
            return None

    rig.port.poll(
        state_or_busy,
        lambda s: (
            s is not None
            and s["ui"] != "load_song"
            and s["ui_mode"] == 0
            and not s["accessing_card"]
        ),
        120,
        0.2,
    )
    rig.pump(1.0)
    print(f"loaded {name} in about {time.time() - t0:.1f} s:", rig.state(), flush=True)
    rig.to_session()


# ---- H1, H3, H4: decision headroom, voice starts and tempo glides ----


def h1_window(rig, seconds):
    probe(rig, "h1_report")  # starts a fresh window
    rig.idle()
    rig.pump(seconds)
    m = rig.mark()
    probe(rig, "h1_report")
    return wait_event(rig, r": h1 busy", m)


def t_h1(rig, a):
    levels = [0, 100, 200, 300, 500, 750, 1000, 1500, 2000, 2500, 3000]
    results = {}
    for burn in a.h1_burn:
        probe(rig, "renderburn", burn)
        rig.idle()
        rows = []
        for us in levels:
            probe(rig, "busy", us)
            rig.idle()
            r = h1_window(rig, a.seconds)
            print(f"[H1 burn {burn}] busy {us} us: {r}", flush=True)
            rows.append(r)
            if r and r["late"] > 50:
                break  # well past the knee
        results[burn] = rows
    probe(rig, "busy", 0)
    probe(rig, "renderburn", 0)
    rig.idle()
    rig.note("h1 results " + repr(results))
    return results


def h3_bursts(rig, size, seconds, label):
    probe(rig, "burst", size)
    rig.idle()
    rig.pump(seconds)
    probe(rig, "burst", 0)
    rig.idle()
    rig.pump(0.5)
    lines = paced_dump(rig, "h3_dump", r": h3n ", r": h3b ")
    out = []
    for line in lines:
        body = line.split(": ", 1)[1]
        f = body.split()
        rec = {
            "i": int(f[1]),
            "dir": int(f[3]),
            "vb": int(f[5]),
            "w": [int(x) for x in f[7].strip(",").split(",") if x],
            "o": [int(x) for x in f[9].strip(",").split(",") if x],
        }
        out.append(rec)
        print(
            f"[H3 {label}] burst {rec['i']}: direness {rec['dir']} voices before {rec['vb']} renders waited {rec['w']} start order {rec['o']}"
        )
    rig.note(f"h3 {label} bursts {out!r}")
    return out


def t_h3(rig, a):
    results = {}
    results["8"] = h3_bursts(rig, 8, a.seconds, "8")
    results["12"] = h3_bursts(rig, 12, a.seconds, "12")
    for burn in a.burn:
        probe(rig, "renderburn", burn)
        rig.idle()
        rig.pump(2)
        s = rig.state()
        print(
            f"[H3] render burn {burn} us: direness {s['cpu_direness']} voices {s['voices']}",
            flush=True,
        )
        results[f"8 burn {burn}"] = h3_bursts(rig, 8, a.seconds, f"8 burn {burn}")
        results[f"12 burn {burn}"] = h3_bursts(rig, 12, a.seconds, f"12 burn {burn}")
    probe(rig, "renderburn", 0)
    rig.idle()
    return results


class ClockListener:
    """Collects MIDI clock from the Deluge's first USB port with CoreMIDI timestamps."""

    def __init__(self):
        import rtmidi

        self.mi = rtmidi.MidiIn()
        ports = self.mi.get_ports()
        first = [i for i, p in enumerate(ports) if "deluge" in p.lower()]
        self.mi.ignore_types(sysex=True, timing=False, active_sense=True)
        self.mi.open_port(first[0])
        self.t = 0.0
        self.clocks = []
        self.mi.set_callback(self._cb)

    def _cb(self, event, _data=None):
        msg, delta = event
        self.t += delta
        if msg and msg[0] == CLOCK:
            self.clocks.append(self.t)

    def close(self):
        self.mi.cancel_callback()
        self.mi.close_port()


def glide(rig, frm, to, bars, label, listener):
    probe(rig, "glide", int(frm * 10), int(to * 10), bars)
    rig.idle()
    c0 = len(listener.clocks)
    # It starts at the next bar line and logs one bar after it.
    rig.pump((bars + 3) * 4 * 60 / min(frm, to) + 1)
    clocks = listener.clocks[c0:]
    steps = [
        parse_fields(line)
        for line in paced_dump(rig, "glide_dump", r": h4n ", r": h4s ")
    ]
    rig.note(f"h4 {label} steps {steps!r} clocks {clocks!r}")
    return analyse_glide(steps, clocks, frm, to, bars, label)


def analyse_glide(steps, clocks, frm, to, bars, label):
    """Compares each 16th's sample time with the analytic curve: tempo set per Generator tick (96 a quarter)."""
    if len(steps) < 3:
        print(f"[H4 {label}] too few steps: {len(steps)}")
        return None
    nums = [s["step"] for s in steps]
    skipped = sum(1 for a_, b_ in pairwise(nums) if b_ - a_ > 1)
    repeated = sum(1 for a_, b_ in pairwise(nums) if b_ - a_ < 1)
    glide_ticks = bars * 384
    start_step = nums[0] + 1  # the first logged step is one before the glide's bar line

    def bpm_at(tick):  # tick from the glide start
        if tick < 0:
            return frm
        if tick > glide_ticks:
            return to
        return frm + (to - frm) * tick / glide_ticks

    def samples_per_tick(bpm):
        return 44100 * 60 / (bpm * 96)

    # Predicted sample of each step, from the first glide step's measured sample.
    key = (
        "tick" if "tick" in steps[0] else "sample"
    )  # the timer tick's own time when the build logs it
    base = next(s for s in steps if s["step"] == start_step)
    worst = 0.0
    worst_ticks = 0.0
    pred = base[key]
    tick = 0
    devs = []
    for s in steps:
        if s["step"] < start_step:
            continue
        while tick < (s["step"] - start_step) * 24:
            pred += samples_per_tick(bpm_at(tick))
            tick += 1
        dev = ((s[key] - pred + 2**31) % 2**32) - 2**31
        dev_ticks = dev / samples_per_tick(bpm_at(tick))
        devs.append(dev_ticks)
        if abs(dev_ticks) > abs(worst_ticks):
            worst, worst_ticks = dev, dev_ticks
    # Clock out: 24 per quarter. During a rising glide intervals should shrink, falling: grow.
    iv = [b_ - a_ for a_, b_ in pairwise(clocks)]
    reversals = 0
    if iv:
        sign = -1 if to > frm else 1
        smooth = [sum(iv[i : i + 6]) / 6 for i in range(0, len(iv) - 6, 6)]
        reversals = sum(1 for a_, b_ in pairwise(smooth) if (b_ - a_) * sign < -0.0005)
    print(
        f"[H4 {label}] {len(steps)} steps logged, skipped {skipped}, repeated {repeated}; worst deviation from the "
        f"analytic curve {worst:.0f} samples = {worst_ticks:.2f} ticks; clock out {len(clocks)} clocks, "
        f"intervals {min(iv) * 1000 if iv else 0:.2f}-{max(iv) * 1000 if iv else 0:.2f} ms, "
        f"reversals of the 6-clock mean {reversals}",
        flush=True,
    )
    return {
        "label": label,
        "steps": len(steps),
        "skipped": skipped,
        "repeated": repeated,
        "worst_samples": worst,
        "worst_ticks": worst_ticks,
        "clocks": len(clocks),
        "reversals": reversals,
    }


def t_h4(rig, a):
    m = rig.mark()
    probe(rig, "clockcfg", -1, -1, 1)
    was = wait_event(rig, r": clockcfg was", m)
    listener = ClockListener()
    results = []
    try:
        probe(rig, "bpm", 1300)
        rig.idle()
        rig.pump(3)
        for frm, to, bars, label in (
            (130, 170, 1, "130-170 over 1 bar"),
            (170, 100, 1, "170-100 over 1 bar"),
            (100, 130, 1, "100-130 over 1 bar"),
            (130, 170, 8, "130-170 over 1 phrase"),
            (170, 100, 8, "170-100 over 1 phrase"),
        ):
            results.append(glide(rig, frm, to, bars, label, listener))
    finally:
        listener.close()
        if was:
            probe(rig, "clockcfg", -1, -1, was["clockout"])
        rig.idle()
    rig.note("h4 results " + repr(results))
    return results


def t_h1fine(rig, a):
    """A finer H1 sweep around the knee, and H4 again with the timer tick's own time."""
    if a.song:
        load_song(rig, a.song)
        rig.to_session()
        for y in range(8):
            rig.inject(f"pad 16 {y} down", "wait 60", f"pad 16 {y} up", "wait 60")
        rig.idle()
    rig.fresh_perform()
    rig.set_macros(50, 50)
    rig.play(True)
    rig.pump(4)
    results = {}
    for burn, levels in (
        (0, (1000, 1100, 1200, 1300, 1400)),
        (500, (600, 700, 800, 900)),
    ):
        probe(rig, "renderburn", burn)
        rig.idle()
        for us in levels:
            probe(rig, "busy", us)
            rig.idle()
            r = h1_window(rig, a.seconds)
            print(f"[H1 fine burn {burn}] busy {us} us: {r}", flush=True)
            results[f"{burn}/{us}"] = r
    probe(rig, "busy", 0)
    probe(rig, "renderburn", 0)
    rig.idle()
    results["h4"] = t_h4(rig, a)
    rig.play(False)
    rig.note("results " + repr(results))


def t_h134(rig, a):
    """H1 knee, H3 and H4 with the user's heavy song Ttt playing under the Perform POC, then an H1 soak."""
    if a.song:
        load_song(rig, a.song)
        rig.to_session()
        # Saved for arrangement playback, the song's clips aren't active in session view: switch the first 8 rows on
        # with their launch pads (stopped, in RAM only).
        for y in range(8):
            rig.inject(f"pad 16 {y} down", "wait 60", f"pad 16 {y} up", "wait 60")
        rig.idle()
    rig.fresh_perform()
    print("macros:", rig.set_macros(50, 50))
    rig.play(True)
    rig.pump(4)
    print("playing:", rig.state(), flush=True)
    rig.screen("playing")
    results = {}
    if "h1" in a.parts:
        results["h1"] = t_h1(rig, a)
    if "h3" in a.parts:
        results["h3"] = t_h3(rig, a)
    if "h4" in a.parts:
        results["h4"] = t_h4(rig, a)
    if a.soak:
        probe(rig, "busy", a.soak_busy)
        rig.idle()
        start = time.time()
        windows = []
        while time.time() - start < a.soak:
            r = h1_window(rig, min(60, a.soak - (time.time() - start)))
            windows.append(r)
            print(f"[soak {time.time() - start:6.0f} s] {r}", flush=True)
        probe(rig, "busy", 0)
        rig.idle()
        results["soak"] = windows
        late = sum(w["late"] for w in windows if w)
        print(
            f"[soak] busy {a.soak_busy} us for {a.soak:.0f} s: late renders {late}, "
            f"max gap {max((w['maxgap'] for w in windows if w), default=0)} us, "
            f"max direness {max((w['direness'] for w in windows if w), default=0)}, "
            f"culled {sum(w['culled'] for w in windows if w)}",
            flush=True,
        )
    rig.play(False)
    rig.pump(2)
    rig.note("results " + repr(results))


# ---- H5, H6: a full roster preload and lane rolls ----


def run_probe(rig, words, pattern, timeout=60):
    m = rig.mark()
    probe(rig, *words)
    return wait_event(rig, pattern, m, timeout)


def h6_rolls(rig, label):
    """Perc lane, Tom lane and the whole roster, without and with a tag."""
    out = []
    for which, name in ((11, "Perc"), (12, "Tom"), (99, "all 11 lanes")):
        for tag in (0, 1):
            r = run_probe(rig, ("laneroll", which, tag, 7 + which), r": h6 which")
            if r:
                r["label"] = f"{label} {name}{' tag 808' if tag else ''}"
                print(
                    f"[H6 {r['label']}] {r['us'] / 1000:.1f} ms, longest run without a yield {r['maxrun'] / 1000:.2f} ms, "
                    f"slowest f_readdir {r['maxentry'] / 1000:.2f} ms, slowest f_opendir {r['maxopen'] / 1000:.2f} ms, "
                    f"{r['entries']} entries, {r['cand']} candidates, {r['picked']} picked, late renders {r['late']}",
                    flush=True,
                )
                out.append(r)
    rig.note(f"h6 {label} {out!r}")
    return out


def t_h6cold(rig, a):
    """Right after boot, stopped: cold, then warm."""
    rig.play(False)
    results = {
        "cold stopped": h6_rolls(rig, "cold stopped"),
        "warm stopped": h6_rolls(rig, "warm stopped"),
    }
    rig.note("results " + repr(results))


def h5_preload(rig, seed, label):
    m = rig.mark()
    probe(rig, "roster", seed)
    h5 = wait_event(rig, r": h5 seed", m, 60)
    t = wait_event(rig, r": h5t ", m, 10)
    mem = wait_event(rig, r": h5m ", m, 10)
    raw = [line.split(": ", 1)[1] for _, line in rig.events(r": h5 seed", m)]
    if h5 and t and mem:
        print(
            f"[H5 {label}] {raw[-1] if raw else ''}; "
            f"scan {t['scan'] / 1000:.0f} drums {t['drums'] / 1000:.0f} fx {t['fx'] / 1000:.0f} synths {t['synths'] / 1000:.0f} "
            f"clusters {t['clusters'] / 1000:.0f} total {t['total'] / 1000:.0f} ms, teardown {t['teardown'] / 1000:.0f} ms; "
            f"fast free {mem['before'] // 1024} -> {mem['peak'] // 1024} K at the peak (largest {mem['largest'] // 1024} K), "
            f"after {mem['after'] // 1024} K; late renders {mem['late']}, max gap {mem['gap']} us",
            flush=True,
        )
    rig.note(f"h5 {label} {h5!r} {t!r} {mem!r}")
    return {"h5": h5, "t": t, "mem": mem}


def t_h5(rig, a):
    """Ttt plays under Perform at 175 BPM: lane rolls cold and warm while playing, then full roster preloads.

    --preload-only skips the song load, Perform entry and rolls, for more preloads in the same session.
    """
    results = {}
    if not a.preload_only:
        if a.song:
            load_song(rig, a.song)
            rig.to_session()
            for y in range(8):
                rig.inject(f"pad 16 {y} down", "wait 60", f"pad 16 {y} up", "wait 60")
            rig.idle()
        rig.fresh_perform()
        rig.set_macros(50, 50)
        probe(rig, "bpm", 1750)
        rig.idle()
    rig.play(True)
    rig.pump(4)
    print("playing:", rig.state(), flush=True)
    if not a.preload_only:
        results["cold playing"] = h6_rolls(rig, "cold playing")
        results["warm playing"] = h6_rolls(rig, "warm playing")
    results["h5"] = [h5_preload(rig, seed, f"preload seed {seed}") for seed in a.seeds]
    rig.play(False)
    rig.pump(1)
    rig.note("results " + repr(results))


# ---- H10, H11, H12: Compose- and Capture-sized songs ----


def compose(rig, occurrences, bars, budget_us, label, cancel_after=None):
    m = rig.mark()
    if cancel_after is None:
        probe(rig, "compose", occurrences, bars, budget_us)
    else:
        send_now(rig, f"probe compose {occurrences} {bars} {budget_us}")
        time.sleep(cancel_after)
        send_now(rig, "probe composecancel")
    h10 = wait_event(rig, r": h10 occ", m, 180)
    t = wait_event(rig, r": h10t ", m, 10)
    mem = wait_event(rig, r": h10m ", m, 10)
    c = wait_event(rig, r": h10c ", m, 10)
    wait_event(rig, r": h10d ", m, 10)
    raw = [
        line.split(": ", 1)[1]
        for _, line in rig.events(
            r": h10 occ|: h10t |: h10m |: h10c |: h10b |: h10d ", m
        )
    ]
    print(f"[{label}] " + " | ".join(raw), flush=True)
    rig.note(f"compose {label} {h10!r} {t!r} {mem!r} {c!r}")
    return {"label": label, "h10": h10, "t": t, "mem_raw": raw}


def t_h10(rig, a):
    """Ttt plays under Perform: Compose-shaped songs into a private Song, then with yields and a cancel."""
    if a.song:
        load_song(rig, a.song)
        rig.to_session()
        for y in range(8):
            rig.inject(f"pad 16 {y} down", "wait 60", f"pad 16 {y} up", "wait 60")
        rig.idle()
    rig.fresh_perform()
    rig.set_macros(50, 50)
    rig.play(True)
    rig.pump(4)
    print("playing:", rig.state(), flush=True)
    results = []
    # 4:00 and 20:00 at 128 BPM: 128 and 640 bars.
    results.append(compose(rig, 9, 128, 0, "H10 4:00, 9 occurrences"))
    results.append(compose(rig, 23, 640, 0, "H10 20:00, 23 occurrences"))
    results.append(compose(rig, 9, 128, 1000, "H11 4:00, 1 ms budget"))
    results.append(compose(rig, 9, 128, 2000, "H11 4:00, 2 ms budget"))
    results.append(compose(rig, 23, 640, 1000, "H11 20:00, 1 ms budget"))
    results.append(compose(rig, 9, 128, 1000, "H11 4:00 cancelled", cancel_after=1.0))
    rig.play(False)
    rig.pump(1)
    rig.note("results " + repr(results))


def t_h12(rig, a):
    """A 60-minute Capture shape built into a new song, saved as SONGS/GENTEST_H12.XML and loaded again."""
    poc.t_newsong(rig, None)
    rig.play(False)
    m = rig.mark()
    # 60 minutes at 128 BPM is 1920 bars. Sections of 24 bars: 80 occurrences, past the 23rd arrangement-only.
    probe(rig, "songsave", a.h12_occurrences, a.h12_bars)
    build = wait_event(rig, r": h10 occ", m, 600)
    bt = wait_event(rig, r": h10t ", m, 10)
    mem = [line for _, line in rig.events(r": h10m ", m)]
    save = wait_event(rig, r": h12save ", m, 600)
    print(f"[H12 build] {build} {bt} {mem}", flush=True)
    print(f"[H12 save] {save}", flush=True)
    m2 = rig.mark()
    probe(rig, "songcount")
    before = wait_event(rig, r": songcount ", m2, 30)
    print(f"[H12 before reload] {before}", flush=True)
    t0 = time.time()
    ok = True
    try:
        load_song(rig, "GENTEST_H12")
    except RuntimeError as exc:
        ok = False
        print("[H12 reload] failed:", exc, flush=True)
    reload_s = time.time() - t0
    m3 = rig.mark()
    probe(rig, "songcount")
    after = wait_event(rig, r": songcount ", m3, 30)
    print(
        f"[H12 reload] ok {ok}, about {reload_s:.1f} s including browsing; after reload {after}",
        flush=True,
    )
    rig.screen("after reload")
    rig.note(
        "results "
        + repr(
            {
                "build": build,
                "bt": bt,
                "mem": mem,
                "save": save,
                "before": before,
                "after": after,
            }
        )
    )


def t_h10split(rig, a):
    """No song loaded, stopped: where a Compose-shaped song's memory goes, by Outputs, clip setup, notes, drift
    nodes and arranger instances. Run straight after a load, before anything else takes memory."""
    rig.play(False)
    print("before:", rig.state(), flush=True)
    results = [
        compose(rig, 9, 128, 0, "H10 split 4:00, 9 occurrences"),
        compose(rig, 23, 640, 0, "H10 split 20:00, 23 occurrences"),
    ]
    rig.note("results " + repr(results))


def t_h12build(rig, a):
    """H12's build and save on their own: a 60-minute Capture shape into a new song, saved as SONGS/GENTEST_H12.XML.
    The device is left with no general memory free, so touch nothing afterwards: power cycle, then h12reload."""
    poc.t_newsong(rig, None)
    rig.play(False)
    m = rig.mark()
    probe(rig, "songsave", a.h12_occurrences, a.h12_bars)
    # The build's first event ("h10 occ") sometimes never arrives, so wait on the ones after it.
    bt = wait_event(rig, r": h10t ", m, 600)
    save = wait_event(rig, r": h12save ", m, 600)
    build = wait_event(rig, r": h10 occ", m, 1)
    mem = [line for _, line in rig.events(r": h10m |: h10b |: h10d ", m)]
    print(f"[H12 build] {build} {bt} {mem}", flush=True)
    print(f"[H12 save] {save}", flush=True)
    rig.note("results " + repr({"build": build, "bt": bt, "mem": mem, "save": save}))


def t_h12reload(rig, a):
    """After a power cycle and a fresh load: SONGS/GENTEST_H12.XML through the LOAD browser, then counted and
    played for --seconds."""
    m0 = rig.mark()
    probe(rig, "songcount")
    empty = wait_event(rig, r": songcount ", m0, 30)
    print(f"[H12 before reload] {empty}", flush=True)
    t0 = time.time()
    load_song(rig, "GENTEST_H12")
    reload_s = time.time() - t0
    m = rig.mark()
    probe(rig, "songcount")
    after = wait_event(rig, r": songcount ", m, 30)
    print(
        f"[H12 reload] about {reload_s:.1f} s including browsing; {after}", flush=True
    )
    rig.screen("after reload")
    rig.play(True)
    rig.pump(a.seconds)
    playing = rig.state()
    print("[H12 playing]", playing, flush=True)
    rig.play(False)
    rig.pump(1)
    m2 = rig.mark()
    probe(rig, "songcount")
    stopped = wait_event(rig, r": songcount ", m2, 30)
    print(f"[H12 after play] {stopped}", flush=True)
    rig.note(
        "results "
        + repr({"empty": empty, "after": after, "playing": playing, "stopped": stopped})
    )


TESTS = {
    "h10split": t_h10split,
    "h12build": t_h12build,
    "h12reload": t_h12reload,
    "h7": t_h7,
    "h6cold": t_h6cold,
    "h5": t_h5,
    "h10": t_h10,
    "h12": t_h12,
    "h134": t_h134,
    "h1fine": t_h1fine,
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
        "--song", help="H1/H3/H4: a user song to load and play under Perform, e.g. Ttt"
    )
    ap.add_argument("--parts", default="h1,h3,h4")
    ap.add_argument(
        "--burn", type=lambda v: [int(x) for x in v.split(",") if x], default=[]
    )
    ap.add_argument(
        "--soak",
        type=float,
        default=0,
        help="H1: seconds of play with --soak-busy on decision ticks",
    )
    ap.add_argument("--soak-busy", type=int, default=100)
    ap.add_argument("--h12-occurrences", type=int, default=80)
    ap.add_argument(
        "--preload-only", action="store_true", help="H5: skip to the preloads"
    )
    ap.add_argument(
        "--seeds", type=lambda v: [int(x) for x in v.split(",")], default=[11, 23, 37]
    )
    ap.add_argument("--h12-bars", type=int, default=1920)
    ap.add_argument(
        "--h1-burn",
        type=lambda v: [int(x) for x in v.split(",") if x],
        default=[0],
        help="H1: render burn levels (us) to sweep at",
    )
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
