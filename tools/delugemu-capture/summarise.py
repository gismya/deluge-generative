"""Print a Markdown summary of every report.json under a capture output folder.

Usage: summarise.py <out folder>
"""

import json
import os
import sys


def main():
    root = sys.argv[1]
    print(
        "| mode | run | boot s | load s | render s (60 s song) | realtime factor | QEMU CPU s | frames | longest silence ms | SSIF underruns | sha256 |"
    )
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    compares = []
    for sub in sorted(os.listdir(root)):
        path = os.path.join(root, sub, "report.json")
        if not os.path.isfile(path):
            continue
        with open(path) as f:
            r = json.load(f)
        shift = f" shift {r['icount_shift']}" if "icount_shift" in r else ""
        mode = r["mode"] + shift
        for i, run in enumerate(r["runs"], 1):
            p = run["phases"]
            underruns = run.get("ssif_underruns_total", -1)
            boot = p["boot"]["wall_s"] + p["launch"]["wall_s"]
            a = run["audio"]
            print(
                f"| {mode} | {i} | {boot:.1f} | {p['load']['wall_s']:.1f} "
                f"| {p['render']['wall_s']:.1f} | {run['realtime_factor']:.2f} "
                f"| {run['qemu_cpu_s']:.1f} | {a['frames']} "
                f"| {a['longest_silence_ms']:.2f} | {underruns} | `{run['sha256'][:12]}` |"
            )
        if "compare_run1_run2" in r:
            compares.append((mode, r["compare_run1_run2"]))
    print()
    for mode, c in compares:
        print(f"- **{mode}**, run 1 vs run 2: `{json.dumps(c)}`")
    t = os.path.join(root, "timings.txt")
    if os.path.isfile(t):
        print()
        with open(t) as f:
            for line in f:
                print("- " + line.strip())


if __name__ == "__main__":
    main()
