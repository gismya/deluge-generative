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
import time

sys.path.insert(0, os.path.dirname(__file__))
import poc_hw_tests as poc
from poc_hw_tests import Rig, bars, parse_fields


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


TESTS = {
    "h7": t_h7,
    "oledstall": t_oledstall,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("test", choices=TESTS)
    ap.add_argument("--log")
    ap.add_argument("--seconds", type=float, default=30)
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
