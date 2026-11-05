"""Re-analyses H4 glide logs from spec_hw_tests.py: each 16th's sample time against the analytic tempo curve.

    python3 scripts/generative/h4_analyse.py docs/spec-hw-tests/logs/h1-fine-h4.log

The curve is the tempo written once per Generator tick (96 a quarter). `lag` is how many ticks a tempo write takes to
apply: 0 = the tick it was written on, 1 = the next one.
"""

import ast
import re
import sys
from itertools import pairwise


def analyse(label, steps, lag):
    m = re.match(r"(\d+)-(\d+) over 1 (bar|phrase)", label)
    frm, to, bars = int(m[1]), int(m[2]), 1 if m[3] == "bar" else 8
    glide = bars * 384
    # The glide starts on the last logged step still at the start tempo (the probe logs from a step before it).
    first_change = next(i for i, s in enumerate(steps) if s["bpm"] != frm * 100)
    start_step = steps[first_change - 1]["step"]

    def bpm_at(tick):
        tick -= lag
        if tick < 0:
            return frm
        if tick > glide:
            return to
        return frm + (to - frm) * tick / glide

    def spt(bpm):
        return 44100 * 60 / (bpm * 96)

    base = next(s for s in steps if s["step"] == start_step)
    pred, tick, worst, devs = base["sample"], 0, 0.0, []
    for s in steps:
        if s["step"] < start_step:
            continue
        while tick < (s["step"] - start_step) * 24:
            pred += spt(bpm_at(tick))
            tick += 1
        dev = (((s["sample"] - pred) + 2**31) % 2**32) - 2**31
        dt = dev / spt(bpm_at(tick))
        devs.append(dt)
        worst = max(worst, abs(dt))
    return worst, devs


def main(path):
    with open(path) as fh:
        lines = fh.readlines()
    for line in lines:
        m = re.match(r"[\d.]+ h4 (.*?) steps (\[.*?\]) clocks (\[.*?\])", line)
        if not m:
            continue
        steps = ast.literal_eval(m[2])
        nums = [s["step"] for s in steps]
        skipped = sum(1 for a, b in pairwise(nums) if b - a > 1)
        repeated = sum(1 for a, b in pairwise(nums) if b - a < 1)
        w0, d0 = analyse(m[1], steps, 0)
        w1, d1 = analyse(m[1], steps, 1)
        per_step = []
        for a, b in pairwise(steps):
            meas = (b["sample"] - a["sample"]) % 2**32
            exp = 24 * 44100 * 60 / ((a["bpm"] + b["bpm"]) / 200 * 96)
            per_step.append((meas - exp) / (exp / 24))
        print(
            f"{m[1]}: each step against its logged tempo {min(per_step):+.2f} to {max(per_step):+.2f} ticks; "
            f"{len(steps)} steps, skipped {skipped}, repeated {repeated}; worst deviation {w0:.2f} ticks "
            f"(tempo applied the same tick), {w1:.2f} ticks (the next tick); final {d0[-1]:.2f} / {d1[-1]:.2f}"
        )


if __name__ == "__main__":
    main(sys.argv[1])
