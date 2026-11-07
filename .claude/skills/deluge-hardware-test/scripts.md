# Probe scripts

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` to start a new script, and add
`TEST_EVENT(...)` calls to firmware code you need to observe.

- Start every script with `start_state`, once (include files have none). It brings the device from wherever the last
  script left it (Perform, a menu, session view, playing) to a stopped instrument clip, so scripts run in any order on
  one load. `--events` counts a script's events from after its `start_state`, so what `start_state` itself causes,
  such as leaving Perform, prints for no script: a script that measures a leave leaves Perform itself (as
  `perform_mem_cycle.txt` does).
- Bisecting with old builds: `start_state` first sends `release entry`, which builds older than `hold entry` reject.
  It then prints `WARNING ...: start_state: the build doesn't know release entry` and goes on, so today's scripts
  still run there. Any other rejected line still fails the script.
- A timeout needs the keyword: `expect event gen_left within=15`. A bare trailing number is part of the text to match.
- A failed `expect event` prints the script's last events of the same kind (same first word) as `got ...` lines,
  whatever `--events` says, so one run shows what came instead.
- Scripts that send MIDI clock or transport (`clock`, `midi fa`) depend on the device's clock-in settings:
  [external-clock.md](external-clock.md).
