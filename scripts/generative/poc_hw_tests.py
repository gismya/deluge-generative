"""Hardware tests of the Perform POC (#45), driven over the sysex test port.

Needs a build with ENABLE_SYSEX_TEST, which also compiles in the POC probe (src/deluge/generative/debug/poc_probe.cpp)
that reports per-bar timing, voice starts, audio gaps and memory as test events. Run with ./dbt exec:

    ./dbt exec "python3 scripts/generative/poc_hw_tests.py <test> --log docs/perform-poc-hardware-tests/logs/<test>.log"

Tests: newsong, enter, busy, swing, tempo, long, startstop, choke, save. Each prints a summary and writes every event
and state line it saw, with host time, to the log.
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
import deluge_probe as dp

UPPER_GOLD, LOWER_GOLD, TEMPO = 4, 5, 2  # test port encoder indexes
ENERGY, DENSITY = UPPER_GOLD, LOWER_GOLD  # the POC's gold knobs
SAMPLE_RATE = 44100


class Rig:
    def __init__(self, log_path):
        self.port = dp.wait_ready(
            lambda: dp.RtMidiLink(quiet=True),
            int(Path(".deluge_hex_key").read_text(), 16),
            30,
        )
        self.timed = []  # (host time, line)
        self.log = open(log_path, "a") if log_path else None  # noqa: SIM115 (open for the whole run)
        collect = self.port._collect

        def stamped(msg):
            before = len(self.port.events)
            handled = collect(msg)
            for line in self.port.events[before:]:
                self.timed.append((time.time(), line))
                self.note(line)
            return handled

        self.port._collect = stamped
        self.port.subscribe()
        self.entered_at = 0  # index into timed at the last Perform entry: the Recipe resets to 25/25 there
        self.note(f"# start {time.strftime('%Y-%m-%d %H:%M:%S')}")

    def note(self, text):
        if self.log:
            self.log.write(f"{time.time():.4f} {text}\n")
            self.log.flush()

    # ---- input ----

    def inject(self, *lines):
        for line in lines:
            self.port.inject(line)

    def press(self, name, hold_ms=60):
        self.inject(f"button {name} down", f"wait {hold_ms}", f"button {name} up")

    def shift_press(self, name):
        self.inject("button SHIFT down", "wait 80")
        self.press(name)
        self.inject("wait 80", "button SHIFT up")

    def turn(self, encoder, detents):
        self.inject(f"encoder {encoder} {detents}")

    def idle(self):
        return self.port.wait_idle(120)

    def state(self):
        s = self.port.state()
        self.note("state " + json.dumps(s))
        return s

    def pump(self, seconds):
        self.port.pump(seconds)

    def wait_for(self, key, value, timeout=10):
        s = self.port.poll(
            self.port.state, lambda s: dp._matches(s[key], str(value)), timeout
        )
        self.note("state " + json.dumps(s))
        return dp._matches(s[key], str(value)), s

    def screen(self, label):
        img = self.port.oled()
        text = dp.oled_ascii(img) if img else "(no OLED reply)"
        self.note(f"oled {label}\n{text}")
        return text

    def events(self, pattern, since=0):
        rx = re.compile(pattern)
        return [(t, line) for t, line in self.timed[since:] if rx.search(line)]

    def mark(self):
        return len(self.timed)

    # ---- the POC ----

    def macros(self):
        rec = self.events(r"recipe energy", self.entered_at)
        if not rec:
            return (25, 25) if self.entered_at else None
        m = re.search(r"energy (\d+) density (\d+)", rec[-1][1])
        return int(m[1]), int(m[2])

    def set_macros(self, energy, density, current=(25, 25)):
        """Turns the gold knobs to the targets and returns the macros the firmware reports.

        A test port detent is 4 encoder edges, and the Perform view moves a macro one step per edge, so a detent is
        4 steps. A target 4k steps away is reached directly; others by clamping at 0 or 50 first.
        """
        e, d = self.macros() or current
        for knob, now, target in ((ENERGY, e, energy), (DENSITY, d, density)):
            for path in self._paths(now, target):
                self.turn(knob, path)
        self.idle()
        self.pump(0.5)
        return self.macros()

    @staticmethod
    def _paths(now, target):
        if target == now:
            return []
        if (target - now) % 4 == 0:
            return [(target - now) // 4]
        if target % 4 == 0:
            return [-13, target // 4]  # to 0 first
        if (50 - target) % 4 == 0:
            return [13, -((50 - target) // 4)]  # to 50 first
        return [round((target - now) / 4)]  # nearest reachable

    def enter_perform(self):
        """SHIFT+SONG from session view. Returns host seconds from the press to the root UI changing."""
        start = time.time()
        self.entered_at = self.mark()
        self.shift_press("SESSION_VIEW")
        ok, _ = self.wait_for("root_ui", "generative_perform", 15)
        return ok, time.time() - start

    def to_session(self):
        s = self.state()
        if s["root_ui"] == "generative_perform":
            self.press("BACK")
            self.wait_for("root_ui", "session")
            s = self.state()
        if s["root_ui"] != "session":
            self.shift_press("SESSION_VIEW")
            self.wait_for("ui", "session")
            self.wait_for("ui_mode", 0)

    def fresh_perform(self):
        """Stopped, then a fresh Perform entry: the Recipe starts at 25/25."""
        self.play(False)
        self.to_session()
        return self.enter_perform()

    def play(self, on=True):
        s = self.state()
        if s["playing"] != on:
            self.press("PLAY")
            return self.wait_for("playing", on)[0]
        return True


# ---- Parsing the probe's events ----


def parse_fields(line):
    """'bar 3 long 5000 short 5000 ...' -> {'bar': 3, 'long': 5000, ...}"""
    body = line.split(": ", 1)[1] if ": " in line else line
    words = body.split()
    if len(words) > 1 and not re.fullmatch(r"-?\d+", words[1]):
        words = words[1:]  # an event name with no value, e.g. "gen_start pos 0 ..."
    return {
        k: int(v) for k, v in zip(words[0::2], words[1::2]) if re.fullmatch(r"-?\d+", v)
    }


def bars(rig, since=0):
    out = {}
    for _, line in rig.events(r": bar \d", since):
        f = parse_fields(line)
        out.setdefault(f["bar"], {}).update(f)
    for _, line in rig.events(r": barv \d", since):
        f = parse_fields(line)
        out.setdefault(f["barv"], {}).update({("v_" + k): v for k, v in f.items()})
    for t, line in rig.events(r": downbeat \d", since):
        f = parse_fields(line)
        out.setdefault(f["downbeat"], {}).update(
            {
                "t": t,
                "sample": f["sample"],
                "len": f.get("len"),
                "dev_t": float(line.split(":")[0]),
            }
        )
    return dict(sorted(out.items()))


def bpm_of(length):
    return 4 * 60 * SAMPLE_RATE / length if length else None


def summarize_bars(b, label):
    lens = [v["len"] for v in b.values() if v.get("len")]
    notes = sum(v.get("v_n", 0) for v in b.values())
    deferred = sum(v.get("v_def", 0) for v in b.values())
    kick = sum(v.get("v_dk", 0) for v in b.values())
    bass = sum(v.get("v_db", 0) for v in b.values())
    refused = sum(v.get("v_ref", 0) for v in b.values())
    late = sum(v.get("v_late", 0) for v in b.values())
    lr = sum(v.get("v_lr", 0) for v in b.values())
    gap = max((v.get("v_gap", 0) for v in b.values()), default=0)
    vmax = max((v.get("v_vmax", 0) for v in b.values()), default=0)
    grids = [v.get("grid") for v in b.values() if "grid" in v]
    print(
        f"[{label}] {len(b)} bars, {notes} note-ons, {deferred} deferred (kick {kick}, bass {bass}), "
        f"refused {refused} / late-started {late}, max voices {vmax}, max audio gap {gap} us, late renders {lr}"
    )
    if lens:
        print(
            f"[{label}] bar length samples min {min(lens)} max {max(lens)} mean {sum(lens) / len(lens):.1f} "
            f"-> {bpm_of(sum(lens) / len(lens)):.3f} BPM by the audio clock"
        )
    if grids:
        print(f"[{label}] grid lines per bar: min {min(grids)} max {max(grids)}")
    return {
        "notes": notes,
        "deferred": deferred,
        "kick": kick,
        "bass": bass,
        "refused": refused,
        "late": late,
        "lr": lr,
        "gap": gap,
    }


# ---- Tests ----


def t_newsong(rig, a):
    """SHIFT+LOAD, OK: a new, empty song."""
    rig.to_session() if rig.state()["root_ui"] == "generative_perform" else None
    rig.play(False)
    rig.shift_press("LOAD")
    ok, _ = rig.wait_for("ui", "context_menu", 5)
    rig.screen("clear song menu")
    rig.press("SELECT_ENC")
    ok2, s = rig.wait_for("ui_mode", 0, 15)
    rig.pump(1)
    s = rig.state()
    print("new song:", ok, ok2, s["ui"], s["bpm"], "swing", s["swing"])
    rig.to_session()
    print("now in", rig.state()["ui"])


def t_enter(rig, a):
    """Test 7: time entering Perform (stopped), several times."""
    rig.play(False)
    for i in range(a.repeat):
        rig.to_session()
        rig.pump(1)
        m = rig.mark()
        ok, host = rig.enter_perform()
        rig.pump(0.5)
        ev = rig.events(r"perform_enter", m)
        print(
            f"enter {i}: ok={ok} host {host * 1000:.0f} ms (press to root UI change), "
            f"firmware {ev[-1][1].split(': ', 1)[1] if ev else 'no event'}"
        )
        rig.screen(f"perform {i}")
    print(rig.state())


def t_busy(rig, a):
    """Test 1: Energy 50 and Density 50, count voice starts the per-render limit deferred."""
    rig.fresh_perform()
    print("macros:", rig.set_macros(50, 50))
    m = rig.mark()
    rig.play(True)
    rig.pump(a.seconds)
    rig.play(False)
    rig.pump(2)
    b = bars(rig, m)
    summarize_bars(b, "E50 D50")
    for k, v in list(b.items())[:4]:
        print(k, v)
    print(rig.events(r"gen_stop", m)[-1:])
    ok, s = rig.wait_for("voices", 0, 5)
    print("voices 0 after stop:", ok, s["voices"])


def t_swing(rig, a):
    """Test 2: change the song swing while playing; the 16th intervals should go long-short."""
    rig.fresh_perform()
    rig.set_macros(25, 50)
    rig.play(True)
    phases = []
    for target in (0, 10, 20, -10, 0):  # swing amount offset from 50
        m = rig.mark()
        cur = rig.state()["swing"]
        if target != cur:
            rig.inject("button SHIFT down", "wait 80")
            rig.turn(TEMPO, 1 if target > cur else -1)  # first detent shows the popup
            rig.inject("wait 200")
            for _ in range(
                abs(target - cur)
            ):  # the tempo encoder moves swing one step per action
                rig.turn(TEMPO, 1 if target > cur else -1)
                rig.inject("wait 60")
            rig.inject("wait 80", "button SHIFT up")
            rig.idle()
            if target == cur + (1 if target > cur else -1):
                pass
        rig.screen(f"swing {target}")
        got = rig.state()["swing"]
        rig.pump(a.seconds)
        b = bars(rig, m)
        full = [v for v in list(b.values())[1:] if v.get("long") and v.get("short")]
        longs = [v["long"] for v in full]
        shorts = [v["short"] for v in full]
        if longs:
            lo, sh = sum(longs) / len(longs), sum(shorts) / len(shorts)
            print(
                f"swing {got + 50}%: {len(full)} full bars, long {lo:.0f} short {sh:.0f} samples, "
                f"ratio {lo / (lo + sh) * 100:.1f}% (expect {got + 50}%)"
            )
            phases.append((got, lo, sh))
        else:
            print(f"swing {got + 50}%: no full bars")
        summarize_bars(b, f"swing {got + 50}")
    rig.play(False)
    rig.pump(2)


def t_tempo(rig, a):
    """Test 3: turn the tempo encoder while playing, slowly and fast."""
    rig.fresh_perform()
    rig.set_macros(25, 50)
    m0 = rig.mark()
    rig.play(True)
    rig.pump(8)
    print("start bpm", rig.state()["bpm"])

    def phase(label, steps, per, pause):
        m = rig.mark()
        rig.turn(TEMPO, 1 if per > 0 else -1)  # first detent shows the tempo popup
        rig.inject("wait 100")
        for _ in range(steps):
            rig.turn(TEMPO, per)
            if pause:
                rig.inject(f"wait {pause}")
        rig.idle()
        bpm = rig.state()["bpm"]
        rig.pump(6)
        b = bars(rig, m)
        summarize_bars(b, label)
        print(f"[{label}] now {bpm} BPM")

    phase("slow up", 10, 1, 700)
    phase("slow down", 10, -1, 700)
    phase("fast up", 30, 1, 0)
    phase("fast down", 30, -1, 0)
    phase("burst up", 1, 40, 0)
    phase("burst down", 1, -40, 0)
    rig.play(False)
    rig.pump(2)
    b = bars(rig, m0)
    summarize_bars(b, "tempo all")
    st = rig.events(r"gen_stop", m0)
    print(st[-1:] if st else "no gen_stop")
    for k, v in b.items():
        if v.get("grid", 16) != 16 or v.get("v_skip") or v.get("v_back"):
            print("odd bar", k, v)


def t_long(rig, a):
    """Test 4: play a long time, sweeping both knobs now and then."""
    rig.fresh_perform()
    rig.set_macros(25, 25)
    m0 = rig.mark()
    rig.play(True)
    host0 = time.time()
    sweeps = [
        (0, 0),
        (50, 50),
        (10, 40),
        (40, 10),
        (25, 25),
        (50, 30),
        (5, 50),
        (30, 15),
    ]
    i = 0
    while time.time() - host0 < a.seconds:
        rig.pump(min(a.sweep_every, a.seconds - (time.time() - host0)))
        if time.time() - host0 >= a.seconds:
            break
        target = sweeps[i % len(sweeps)]
        i += 1
        got = rig.set_macros(*target)
        s = rig.state()
        print(
            f"{time.time() - host0:7.0f} s sweep -> {got}  voices {s['voices']} direness {s['cpu_direness']} "
            f"dropped {s['events_dropped']}",
            flush=True,
        )
    rig.play(False)
    rig.pump(3)
    s = rig.state()
    b = bars(rig, m0)
    summarize_bars(b, "long")
    print("after stop", s)
    print(rig.events(r"gen_stop", m0)[-1:])
    mem = rig.events(r": mem ", m0)
    if mem:
        print("mem first", mem[0][1], "last", mem[-1][1])
    downs = [(v["t"], v["sample"], v["dev_t"]) for v in b.values() if "t" in v]
    if len(downs) > 2:
        (t0, s0, d0), (t1, s1, d1) = downs[0], downs[-1]
        n = len(downs) - 1
        print(
            f"downbeats {n} bars apart: host {t1 - t0:.3f} s, device timer {d1 - d0:.4f} s, "
            f"audio {(s1 - s0) / SAMPLE_RATE:.4f} s"
        )
        print(
            f"bpm by host clock {n * 4 * 60 / (t1 - t0):.4f}, by device timer {n * 4 * 60 / (d1 - d0):.4f}, "
            f"by audio samples {n * 4 * 60 * SAMPLE_RATE / ((s1 - s0) & 0xFFFFFFFF):.4f}"
        )


def t_startstop(rig, a):
    """Test 5: quick start/stop, then enter Perform while the transport runs."""
    rig.fresh_perform()
    rig.set_macros(25, 50)
    m0 = rig.mark()
    for hold in (0.3, 0.15, 1.0, 0.5, 2.2, 0.08, 0.6, 3.0):
        rig.press("PLAY", 30)
        rig.inject(f"wait {int(hold * 1000)}")
        rig.press("PLAY", 30)
        rig.inject("wait 150")
    rig.idle()
    rig.pump(3)
    stops = rig.events(r"gen_stop", m0)
    starts = rig.events(r"gen_start", m0)
    print(
        f"{len(starts)} gen_start, {len(stops)} gen_stop:",
        [l.split(": ", 1)[1] for _, l in stops],
    )
    ok, s = rig.wait_for("voices", 0, 5)
    print("voices 0 after:", ok, s)
    rig.screen("after start/stop")

    # Enter Perform while the transport runs.
    for delay in (0.0, 0.7, 1.3):
        rig.to_session()
        rig.play(True)
        rig.pump(1.0 + delay)
        m = rig.mark()
        rig.enter_perform()
        rig.pump(8)
        st = rig.events(r"gen_start", m)
        b = bars(rig, m)
        if st:
            f = parse_fields(st[0][1])
            pos, origin, barlen = f["pos"], f["origin"], f["barlen"]
            print(
                f"enter while playing (+{delay}s): {st[0][1].split(': ', 1)[1]} -> origin is "
                f"{'a bar line' if origin % barlen == 0 else 'NOT a bar line'}, {origin - pos} ticks after the hook"
            )
        else:
            print("enter while playing: no gen_start")
        summarize_bars(b, f"entered +{delay}")
        rig.play(False)
        rig.pump(2)
        print("stop:", rig.events(r"gen_stop", m)[-1:])
    ok, s = rig.wait_for("voices", 0, 5)
    print("voices 0 at end:", ok, s["voices"])


def t_choke(rig, a):
    """Test 6: hats at Density 30 and up. Counts closed-hat hits while the open hat rang, and both on one tick."""
    rig.fresh_perform()
    for d in (25, 30, 40, 50):
        rig.set_macros(25, d)
        m = rig.mark()
        rig.play(True)
        rig.pump(a.seconds)
        rig.play(False)
        rig.pump(2)
        b = bars(rig, m)
        ohr = sum(v.get("v_ohr", 0) for v in b.values())
        hat2 = sum(v.get("v_hat2", 0) for v in b.values())
        print(
            f"density {d}: {len(b)} bars, closed hat while open hat sounding {ohr}, both hats on one tick {hat2}"
        )


def t_save(rig, a):
    """Test 8: SAVE in Perform. Presses SAVE, records where it lands, then backs out without saving."""
    rig.fresh_perform()
    rig.play(a.playing)
    rig.pump(2)
    rig.press("SAVE")
    rig.pump(1.5)
    s = rig.state()
    print("after SAVE:", s["ui"], s["root_ui"], s["ui_mode"])
    print(rig.screen("after SAVE"))
    if a.confirm:
        # Accept the default name: SELECT pushes through the save UI.
        rig.press("SELECT_ENC")
        rig.pump(4)
        s = rig.state()
        print("after SELECT:", s)
        print(rig.screen("after SELECT"))
    else:
        rig.press("BACK")
        rig.pump(1)
        s = rig.state()
        print("after BACK:", s["ui"], s["root_ui"])
        print(rig.screen("after BACK"))
    rig.play(False)


TESTS = {
    "newsong": t_newsong,
    "enter": t_enter,
    "busy": t_busy,
    "swing": t_swing,
    "tempo": t_tempo,
    "long": t_long,
    "startstop": t_startstop,
    "choke": t_choke,
    "save": t_save,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("test", choices=TESTS)
    ap.add_argument("--log")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--sweep-every", type=float, default=120)
    ap.add_argument("--playing", action="store_true")
    ap.add_argument("--confirm", action="store_true")
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
