# When the device misbehaves

- **Loads hung now and then, at boot ([#166](https://github.com/gismya/deluge-generative/issues/166)).** The
  chainloader that `loadfw` runs in the *running* build left its DMA channels on. They re-read their link descriptors
  from memory the new image had just overwritten, and wrote wherever the new bytes pointed, so the new build crashed
  early in `resetprg()` (crash photos: a wild jump right after the first `setPinMux()`). It depended on the layout
  and on timing, not on the image crossing `0x20200000`. Builds with the fix stop every channel before the copy.
  So a load is safe only when the build doing it (the one running) has the fix. A load straight after a power cycle
  goes through the SD-flashed build: until a build with the fix is flashed, such loads can still crash.
  `scripts/generative/test_port_boot_dma.txt` checks this: it fails, listing the channels left running, when the load
  ran exposed. A hang costs a power cycle, so SKILL.md puts every script in one session on one load.
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
- **Why hands off during a run** (SKILL.md step 1): on 2026-11-05 the device hung in the settings menu while the user
  pressed buttons during a script. The same sequence alone passed.
- **Load with the right key only.** A firmware packet with a wrong key freezes the device with E997 until a power
  cycle. The session writes the desk Deluge's key to `.deluge_hex_key` (git ignores it) when a worktree lacks it; the
  key is set in Settings > Community Features > Allow Insecure Develop Sysex Messages.

## What a loaded build doesn't prove

`loadfw` runs the build from RAM, so code that runs at boot can behave differently than after an SD flash. For a change
that touches boot, startup or flash storage, ask the user to also flash the build from the SD card before merging.
