---
name: deluge-hardware-test
description: Test a firmware build on the real Deluge over USB - load it and run test scripts over the sysex test port with `deluge_probe.py session`. Use when verifying a change on hardware or the physical device, or when a PR's test plan has a hardware run.
---

# Testing on the real Deluge

The Deluge on the desk runs a build flashed from its SD card with both `ENABLE_SYSEX_LOAD` and `ENABLE_SYSEX_TEST`.
From there each new build loads into RAM over USB and runs test scripts, with no SD card or human needed. A power cycle
always returns the device to the SD-flashed build. Only the user can power cycle.

One Deluge serves every agent. `deluge_probe.py session` holds the rules for sharing it: the hung marker, the lock,
loading, waiting for boot, and running the scripts. Use it for every hardware run.

## Steps

1. **Build with both options.** Without `ENABLE_SYSEX_LOAD` the device ignores the next load until a power cycle;
   without `ENABLE_SYSEX_TEST` nothing answers the scripts. The session refuses a build missing either.
   Build Release: since #109/#110 a relwithdebinfo test-port build's internal-RAM segment ends past `0x20200000`,
   and builds like that hang after every `loadfw` with all LEDs lit (#166). The session refuses them (`--force-load`
   overrides).
   ```shell
   ./dbt configure -DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON && ./dbt build release
   ```
2. **Run the session**: the smoke script first, then your own, all on one load.
   ```shell
   ./dbt exec "python3 scripts/generative/deluge_probe.py session --build release --owner '<ticket>' \
     scripts/generative/test_port_smoke.txt scripts/generative/<yours>.txt"
   ```
   It waits up to 30 minutes for the lock (`--no-wait` fails at once instead), loads the build, waits for a fresh boot,
   and runs each script. Done when every script prints `N expects, 0 failed` and the session exits 0. Report any
   failing line as printed, with the `got ...` lines under it. `--no-load` runs the scripts on the build already
   running.

   Writing or changing a script: read [scripts.md](scripts.md) first.

   To report figures the firmware sends as test events, add `--events PATTERN` (a regex): after each script the
   session prints that script's matching events as `  event ...` lines. For example `--events
   'gen_mem|gen_entry|gen_left'` gives each enter/leave cycle's free memory and entry and leave times. `--events .`
   prints every event.
3. **Read the exit code.**
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
4. **Restore the default configuration** when you're done, so the next `./dbt build release` and `./dbt sizediff` see
   the default build: `./dbt configure -DENABLE_SYSEX_TEST=OFF -DENABLE_SYSEX_LOAD=OFF`.

State lives in `~/.cache/deluge-generative/hardware/` (`$DELUGE_HARDWARE_DIR` overrides it): `lock/` with its owner,
`hung`, `pending.md`, `loaded.json` (the last load the session saw) and `released/`, where locks and old markers are
moved. Nothing there is ever deleted.

## The device hung

A hang shows as `ready` timing out after a load, or `no reply to test port command` in a script. The session catches
both, writes the marker and stops. While the marker exists, the Deluge is off limits to every agent: the session
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
