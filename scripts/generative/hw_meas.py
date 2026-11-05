"""Drive the #37 measurement build on a real Deluge over the sysex test port.

Each argument is one step:
    learn NAME        hold LEARN, press NAME (SYNTH, KIT, MIDI, CV, KEYBOARD), wait for the measurement's last MEAS line
    press NAME        press and release a button
    shift NAME        SHIFT + NAME
    ev "EVENT LINE"   queue one raw test port event line (see parseLine() in test_port.cpp)
    until TEXT        wait for a MEAS line containing TEXT (60 s)
    sleep SECONDS     host-side pause, still collecting log lines
    state             print the state JSON
    oled              print the OLED

Prints the MEAS lines and appends every debug log line to --out. Needs a build with ENABLE_SYSEX_TEST and
GENERATIVE_MEASUREMENTS (Debug or RelWithDebInfo).

    ./dbt exec "python3 scripts/generative/hw_meas.py --out meas.log learn CV learn KIT"
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deluge_probe as dp


class Session:
    def __init__(self, port, out):
        self.port, self.out, self.seen = port, out, 0

    def drain(self):
        """Writes new log lines to the file, prints MEAS lines. Returns the new lines."""
        new = self.port.log[self.seen :]
        self.seen = len(self.port.log)
        with open(self.out, "a") as fh:
            fh.writelines(f"{time.strftime('%H:%M:%S')} {line}\n" for line in new)
        for line in new:
            if "MEAS" in line:
                print(line, flush=True)
        return new

    def until(self, test, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.port.pump(0.1)
            if any(test(line) for line in self.drain()):
                return True
        print(f"!! timed out after {timeout:g} s", flush=True)
        return False

    def press(self, name, hold=None):
        lines = [f"button {hold} down", "wait 100"] if hold else []
        lines += [f"button {name} down", "wait 80", f"button {name} up"]
        lines += ["wait 80", f"button {hold} up"] if hold else []
        for line in lines:
            self.port.inject(line)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()

    port = dp.TestPort(dp.RtMidiLink(quiet=True))
    port.subscribe()
    s = Session(port, a.out)
    steps = list(a.steps)
    ok = True
    try:
        while steps:
            word = steps.pop(0)
            arg = steps.pop(0) if word not in ("state", "oled") else None
            print(f"-- {word} {arg or ''}".rstrip(), flush=True)
            if word == "learn":
                s.press(arg, hold="LEARN")
                ok &= s.until(lambda l: "MEAS" in l and " audio " in l, a.timeout)
            elif word == "press":
                s.press(arg)
            elif word == "shift":
                s.press(arg, hold="SHIFT")
            elif word == "ev":
                port.inject(arg)
            elif word == "until":
                ok &= s.until(lambda l, t=arg: "MEAS" in l and t in l, a.timeout)
            elif word == "sleep":
                port.pump(float(arg))
                s.drain()
            elif word == "state":
                print(json.dumps(port.wait_idle()), flush=True)
            elif word == "oled":
                port.wait_idle()
                img = port.oled()
                print(dp.oled_ascii(img) if img else "no OLED reply", flush=True)
            else:
                raise SystemExit(f"unknown step {word!r}")
        port.pump(0.5)
        s.drain()
    finally:
        port.subscribe(False)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
