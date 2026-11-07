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
   ```shell
   ./dbt configure -DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON && ./dbt build relwithdebinfo
   ```
2. **Run the session**: the smoke script first, then your own, all on one load.
   ```shell
   ./dbt exec "python3 scripts/generative/deluge_probe.py session --build relwithdebinfo --owner '<ticket>' \
     scripts/generative/test_port_smoke.txt scripts/generative/<yours>.txt"
   ```
   It waits up to 30 minutes for the lock (`--no-wait` fails at once instead), loads the build, waits for a fresh boot,
   and runs each script. Done when every script prints `N expects, 0 failed` and the session exits 0. Report any
   failing line as printed. `--no-load` runs the scripts on the build already running.
3. **Read the exit code.**
   - `1`: a script failed, the lock was held past `--wait`, or the build wasn't loadable. The output says which.
   - `2`: the device hung. The session wrote the hung marker and added the run to `pending.md`. Go to
     [The device hung](#the-device-hung).
   - `3`: the hung marker was already set, so the session refused and added the run to `pending.md`. Go to
     [The device hung](#the-device-hung).
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

## Scripts

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` to start a new script, and add
`TEST_EVENT(...)` calls to firmware code you need to observe.

- Start every script with `start_state`. It brings the device from wherever the last script left it (Perform, a menu,
  session view, playing) to a stopped instrument clip, so scripts run in any order on one load.
- A timeout needs the keyword: `expect event gen_left within=15`. A bare trailing number is part of the text to match.
- Scripts that send MIDI clock or transport (`clock`, `midi fa`) depend on the device's clock-in settings:
  [external-clock.md](external-clock.md).

## When the device misbehaves

- **Loads over a RAM-loaded build are the ones that hang** (3 times on 2026-11-06). One load straight after a power
  cycle has never hung. The session loads anyway, since a hang costs only a power cycle, and prints a one-line note
  when the running build was RAM-loaded. With several runs ahead, put them all in one session on one load.
- **No reply during a long load isn't a hang.** A song load blocks the test port for its whole length, and a state
  request gives up after 2 s. The probe retries reads through a load; the session calls it a hang only after `ready`
  times out (`--timeout`, 90 s) or a script gets no reply at all.
- **`ERROR: TERM` on the OLED.** An uncaught C++ exception reached `std::terminate`, which freezes the device. It's
  usually `operator new` throwing `BAD_ALLOC` once general memory is full, so a test that fills memory can freeze the
  next screen that allocates. Decode the pads as for any crash: the stack hints name the allocation site.
- **A hang near the end of `loadfw`.** Builds from before the #55 fix (no `io/midi/sysex_load_progress.h`) write their
  progress bar past `PadLEDs::image`, and the running build can crash. If the running build is one of those, check
  with `arm-none-eabi-nm -n` which symbol follows `PadLEDs::image` in it. If it's live state such as `numUIsOpen`,
  have the user power cycle before the next load.
- **The OLED picture shifted sideways after a load.** It wraps onto the left edge. Seen 2026-11-05 on loads on top of
  a loaded build, never yet on a load straight over the SD-flashed build. It stays until a power cycle, and later loads
  don't clear it. Ask the user to power cycle, and tell them if it happened on a load straight over the SD-flashed
  build.
- **Hands off during a run.** On 2026-11-05 the device hung in the settings menu while the user pressed buttons during
  a script. The same sequence alone passed, so ask the user to keep their hands off the device while a script runs.
- **Load with the right key only.** A firmware packet with a wrong key freezes the device with E997 until a power
  cycle. The session writes the desk Deluge's key to `.deluge_hex_key` (git ignores it) when a worktree lacks it; the
  key is set in Settings > Community Features > Allow Insecure Develop Sysex Messages.

## What a loaded build doesn't prove

`loadfw` runs the build from RAM, so code that runs at boot can behave differently than after an SD flash. For a change
that touches boot, startup or flash storage, ask the user to also flash the build from the SD card before merging.

Tests that need no hardware run faster in delugemu: `scripts/generative/test_port_emu.py` runs the same scripts there.
