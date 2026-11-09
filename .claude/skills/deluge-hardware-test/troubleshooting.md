# When the device misbehaves

- **Loads hang now and then.** Loads over a RAM-loaded build hung 3 times on 2026-11-06. On 2026-11-09 one build
  (#115's) hung twice in a row, once over a RAM-loaded build and once straight after a power cycle, then loaded and
  passed every script on the third try, over another RAM-loaded build; the integration branch's build loaded fine in
  between. The first of those hangs drew a crash on the pads: an undefined instruction in `setPinMux()` called from
  `resetprg()`, at boot, before any Generative code ran, at an instruction the ELF has right. So the image in RAM was
  wrong there, not the code. A hang costs a power cycle, so SKILL.md puts every script in one session on one load,
  and after a power cycle the same build is worth one more load before suspecting it.
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
