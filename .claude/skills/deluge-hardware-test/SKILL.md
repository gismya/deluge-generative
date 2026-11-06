---
name: deluge-hardware-test
description: Test a firmware build on the real Deluge over USB - load it with loadfw, then drive and check it with the sysex test port. Use when verifying a change on hardware or the physical device, or when a PR's test plan has a hardware run.
---

# Testing on the real Deluge

The Deluge on the desk runs a build flashed from its SD card with both `ENABLE_SYSEX_LOAD` and `ENABLE_SYSEX_TEST`.
From there you load each new build into RAM over USB and drive it with test scripts, no SD card or human needed. A
power cycle always returns the device to the SD-flashed build.

The dev-sysex key is `4D750EA4` (Settings > Community Features > Allow Insecure Develop Sysex Messages). `loadfw`
reads it from `.deluge_hex_key` in the current directory, which git ignores, so a fresh worktree lacks it. Without
the file `loadfw` stops at an interactive prompt. Create it first:

```shell
[ -f .deluge_hex_key ] || printf '4D750EA4\n' > .deluge_hex_key
```

## Steps

1. **Build with both options.** Every build you load needs both: without `ENABLE_SYSEX_LOAD` the device ignores the
   next load until a power cycle, and without `ENABLE_SYSEX_TEST` nothing answers the probe.
   ```shell
   ./dbt configure -DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON && ./dbt build relwithdebinfo
   ```
2. **Load it** (about 10 s). The progress bar uses carriage returns, so trim it:
   ```shell
   ./dbt loadfw relwithdebinfo 2>&1 | tr '\r' '\n' | tail -1
   ```
3. **Wait for it to boot.** `ready` reopens the USB port while the Deluge restarts, waits until the song has loaded,
   and prints the state. Done when `sample_timer` is small (under ~200000, a few seconds of uptime): that proves the
   new build is running, not the old one.
   ```shell
   ./dbt exec "python3 scripts/generative/deluge_probe.py ready"
   ```
4. **Run the scripts.** Start with the smoke script, then your own for the change. A run is done when it prints
   `N expects, 0 failed`; report any failing line as it was printed.
   ```shell
   ./dbt exec "python3 scripts/generative/deluge_probe.py run scripts/generative/test_port_smoke.txt"
   ```
   The smoke script expects the transport stopped and the current clip a synth or kit clip, as after boot. Loading a
   build resets the device to that state, so load again whenever a script leaves it somewhere else.

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` as the starting point for a new
script, and add `TEST_EVENT(...)` calls to firmware code you need to observe.

## When the device misbehaves

- **No answer.** If `ready` times out, or the probe prints `no reply to test port command`, the device is hung. Ask
  the user for a photo of the whole pad grid and sidebar, and what the OLED shows. Then ask them to power cycle it
  and go back to step 2. Only the user can power cycle.
- **Crash on the pads.** After a CPU fault or a `FREEZE_WITH_ERROR`, the firmware draws code addresses on the pads
  and the build's commit in the sidebar. Decode the photo with [pad-crash.md](pad-crash.md) before guessing at a
  cause.
- **`ERROR: TERM` on the OLED.** An uncaught C++ exception reached `std::terminate`, which freezes the device. It's
  usually `operator new` throwing `BAD_ALLOC` once general memory is full, so a test that fills memory can freeze
  the next screen that allocates. Decode the pads as for any crash: the stack hints name the allocation site.
- **A hang near the end of `loadfw`.** Builds from before the #55 fix (no `io/midi/sysex_load_progress.h`) write
  their progress bar past `PadLEDs::image`, and the running build can crash. If the running build is one of those,
  check with `arm-none-eabi-nm -n` which symbol follows `PadLEDs::image` in it. If it's live state such as
  `numUIsOpen`, have the user power cycle before the next load.
- **The OLED picture shifted sideways after a load.** It wraps onto the left edge. Seen 2026-11-05 on loads on top
  of a loaded build, never yet on a load straight over the SD-flashed build. It stays until a power cycle, and later
  loads don't clear it. Ask the user to power cycle, and tell them if it happened on a load straight over the
  SD-flashed build.
- **Hands off during a run.** On 2026-11-05 the device hung in the settings menu while the user pressed buttons
  during a script. The same sequence alone passed, so ask the user to keep their hands off the device while a
  script runs.
- **Load with the right key only.** A firmware packet with a wrong key freezes the device with E997 until a power
  cycle. The test port answers a wrong key with `bad key` instead.

## What a loaded build doesn't prove

`loadfw` runs the build from RAM, so code that runs at boot can behave differently than after an SD flash. For a
change that touches boot, startup or flash storage, ask the user to also flash the build from the SD card before
merging.

Tests that need no hardware run faster in delugemu: `scripts/generative/test_port_emu.py` runs the same scripts
there.
