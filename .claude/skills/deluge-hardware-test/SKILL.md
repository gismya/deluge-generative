---
name: deluge-hardware-test
description: Test a firmware build on the real Deluge over USB - load it and run test scripts over the sysex test port with `deluge_probe.py session`. Use when verifying a change on hardware or the physical device, or when a PR's test plan has a hardware run.
---

# Testing on the real Deluge

The Deluge on the desk runs a build flashed from its SD card with both `ENABLE_SYSEX_LOAD` and `ENABLE_SYSEX_TEST`.
From there each new build loads into RAM over USB and runs test scripts, with no SD card or human needed. A power cycle
always returns the device to the SD-flashed build. Only the user can power cycle.

One Deluge serves every agent. `deluge_probe.py session` holds the rules for sharing it: the hung marker, the lock,
loading, waiting for boot, and running the scripts. Use it for every hardware run. Tests that need no hardware run
faster in delugemu: `scripts/generative/test_port_emu.py` runs the same scripts there.

To put files on the card or fetch them (a pack, a test song), with no one at the device: [card-files.md](card-files.md).

## Steps

1. **Run the session**: the smoke script first, then the scripts the change needs, all in one session on one load (a
   load that hangs costs a power cycle, see troubleshooting.md). Ask the user to keep their hands off the device
   while it runs. `covers.py --hardware` prints them, smoke first: the scripts a Deluge can run whose `# covers:`
   paths the change touches (`scripts/generative/covers.py` says how it picks). Add `--full` for every hardware
   script: only on a milestone's final run.
   ```shell
   python3 scripts/generative/covers.py --hardware --base <base branch>
   ./dbt exec "python3 scripts/generative/deluge_probe.py session --build hardware --owner '<ticket>' \
     scripts/generative/test_port_smoke.txt scripts/generative/<picked>.txt ..."
   ```
   `--build hardware` first builds `build-hardware/` (Release, with `ENABLE_SYSEX_TEST` and `ENABLE_SYSEX_LOAD`), so
   `build/` keeps the default options that `check_all.py` and `./dbt sizediff` need, and the two can run at once.
   Both options are needed: without `ENABLE_SYSEX_LOAD` the device ignores the next load until a power cycle, and
   without `ENABLE_SYSEX_TEST` nothing answers the scripts.

   Then it waits up to 30 minutes for the lock (`--wait SECONDS` sets another limit, e.g. `--wait 3600`; `--wait`
   alone is an argument error; `--no-wait` fails at once instead), loads the build, waits for a
   fresh boot, and runs each script. Done when every script prints `N expects, 0 failed` and the session exits 0.
   Report any failing line as printed, with the `got ...` lines under it. `--no-load` runs the scripts on the build
   already running.

   Writing or changing a script: read [scripts.md](scripts.md) first.

   To report figures the firmware sends as test events, add `--events PATTERN` (a regex): after each script the
   session prints that script's matching events as `  event ...` lines. For example `--events
   'gen_mem|gen_entry|gen_left'` gives each enter/leave cycle's free memory and entry and leave times. `--events .`
   prints every event.
2. **Read the exit code.**
   - `1`: a script failed, the lock was held past `--wait`, the build wasn't loadable, or the load didn't take (the
     build that was running still answered near the end of the wait, and no fresh one did). The output says which.
     None of these sets the hung marker.
   - `2`: the device hung: nothing answered for the rest of the wait, whatever answered before (an old build about
     to restart into a build that hung), or a script got no reply. The session wrote the hung marker and added the
     run to `pending.md`. Go to [The device hung](#the-device-hung).
   - `3`: the hung marker was already set, so the session refused and added the run to `pending.md`. Go to
     [The device hung](#the-device-hung).

   Anything else odd (an error on the OLED, a shifted picture, a hang near the end of `loadfw`), or a change that
   touches boot, startup or flash storage: read [troubleshooting.md](troubleshooting.md).

State lives in `~/.cache/deluge-generative/hardware/` (`$DELUGE_HARDWARE_DIR` overrides it): `lock/` with its owner,
`hung`, `pending.md`, `loaded.json` (the last load the session saw) and `released/`, where locks and old markers are
moved. Nothing there is ever deleted.

## The device hung

A hang shows as `ready` timing out after a load, or `no reply to test port command` in a script. The session catches
both, writes the marker and stops. A script that hung says how far it got (`N expects, F+1 failed (stopped after
P passed and F failed: ...)`: the stop counts as one more failed expect) and prints its last 10 events of any kind (`  last ...`), whatever `--events` says. Each
PASS and FAIL line starts with the host's time, as does the `HUNG at` line: the hang lies between the last PASS
and the input after it. While the marker exists, the Deluge is off limits to every agent: the session
refuses, and nothing else touches the device.

**When the user is away** (an overnight or unattended run):

1. Run the same scripts in delugemu: the session prints the command, e.g.
   `python3 scripts/generative/test_port_emu.py --build --out <scratch dir> --script <script> ...`.
2. Check the run's entry in `pending.md`: build and SHA, scripts, the command, what passing looks like. The session
   writes it; add what else passing means with `--note` (e.g. which sounds the OLED should name), or edit the entry.
3. Report the hardware run as pending, with the delugemu result beside it. The user power cycles later and runs the
   pending entries.

**When the user is there**: ask for a photo of the whole pad grid and sidebar, and what the OLED shows. A crash draws
code addresses on the pads: decode the photo with [pad-crash.md](pad-crash.md). Then ask the user to power cycle, run
`deluge_probe.py session --clear-hung` (it moves the marker into `released/`), and run the session again.

## Comparing a PR with its base

For a change to timing, CPU or memory, one session's figures say little alone: the device drifts between runs. Run
the base and the PR alternately with `scripts/generative/hw_pairs.py`, the base in its own worktree at the PR's merge
base (`git worktree add <dir> $(git merge-base origin/<base> HEAD)`):

```shell
python3 scripts/generative/hw_pairs.py run --base <base worktree> --pr . --rounds 2 \
  --events 'gen_h1 |gen_h16|gen_transition start' --out <scratch dir> perform_transition.txt \
  > LOG 2>&1; echo "hw_pairs exit=$?" >> LOG
python3 scripts/generative/hw_pairs.py summary <scratch dir>/*.log
```

`run` calls this skill's session in each worktree in turn, smoke script first, and logs each to `<side><n>.log`;
`hw_pairs.py --help` has the run order and what `summary` prints. `run` keeps the exit codes' rules: on 2 or 3 it stops
(exit 2), and the device is hung as above; it also stops when no MIDI port answers or a session ends before its
scripts. Every session builds and loads, so a run passes Bash's 600 s limit: start it in the background and wait for
the `hw_pairs exit=` line, as `docs/agents/orchestration.md`'s Runner brief does for a session.

Before the run, commit your changes and give the base worktree a path as long as the PR's: the firmware embeds its
source path and a `-dirty` tag, and either one shifts the code, which moves render times. Keep `--rounds` even, so
each side goes first equally often.

How far `summary`'s figures can be trusted (#282, from 3 runs with the same commit on both sides):

- **H1**: a change over about 1% is real; same-commit sides were within 0.3%.
- **Render mean**: the PR side read higher in every same-commit run (+4%, +4%, +17%, the last with a `-dirty`
  build). Read a PR's render mean as changed only past about 20%; that threshold rests on those 3 runs.
- **Render max** is one window's outlier, mostly a window with a preload from the card; same-commit sides differed
  by -9% to +26%. It bounds the worst render and can't tell a PR from its base.

## Late audio (underruns)

To check that the audio never ran dry, add `gen_audio` to `--events`. Every 4 bars in Perform it reports the audio
routine's entries, `underruns=` (entries after the 2.9 ms TX ring ran out), `margin_min_us=` (the least audio left at an
entry, negative after an underrun), and `drift=`, the DMA's move off the timer in samples, which should stay within a
few samples. `gen_audio_late` names the task that kept the routine away, and `gen_audio_ui_timer` the UI timers whose
handler ran over 2 ms (TimerName's value). Don't count spans between render starts, or a render's length (`gen_h16`'s
`render_max_us`): neither one shows the margin. `perform_audio_margin.txt` is the steering load where the margin is
smallest. Spec §20 has its figures (#449).
