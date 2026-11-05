# Hardware measurement results (#37)

Measured on 2026-11-05 on the OLED Deluge on the desk, driven by an agent over USB with the sysex test port (no hands on the device). Logs: [`logs/o2-session-a.log`](logs/o2-session-a.log), [`logs/o2-session-b.log`](logs/o2-session-b.log).

## How

- **Build**: `task/hardware-measurements-usb` @ `a83ef8ecc` plus the pad-redraw fix below, which is `task/hardware-measurements` with `generative` (the test port) merged in. RelWithDebInfo (`-O2`, LTO), configured with `-DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON`, loaded into RAM with `./dbt loadfw relwithdebinfo`. Each `loadfw` reboots, so the sample cache starts cold.
- **Driver**: [`scripts/generative/hw_meas.py`](../../scripts/generative/hw_meas.py) presses the LEARN triggers and LOAD/SELECT through the test port, and collects the `MEAS` lines from the debug log. The LOAD browser was steered with the select encoder, checked by reading the OLED.
- **Songs**: the card has no factory songs, but it has the user's own. **House 3** stands in for "typical" (3 synths, 2 kits, 20 samples, 194 KB XML). **Ttt** is a heavy one (5 synths, 4 kits, 43 samples, 349 KB XML).
- **Debug build**: the `-Og` Debug build crashed at boot when loaded with `loadfw`. The pads showed a data abort, and the stack trace on them went back to the reset handler, so it happened during startup. The RelWithDebInfo build has the same measurements and is closer to Release, so every number here is `-O2`. The Debug crash is not looked into further.
- **Pad redraw fix**: the first pad numbers were wrong. The OLED shares the PIC UART, and while it waits for the UART to drain, `sendOutMainPadColours()` defers and queues nothing. About 12 of 16 runs sent nothing and timed at ~7 µs. `measurePadRedraw` now waits for a clear UART before each run, and counts a run that still queued nothing as deferred instead of timing it.

**Fast heap** is the on-chip heap: `internal` + `internal_small`. **ext** is the 2 MiB SDRAM general heap (`new`, containers) plus its small region. **steal** is the SDRAM sample memory, and **cache** is the sample data in it that can be reclaimed. The test-port build's internal region is 1,066,264 B, against 1,113,324 B in the POC Release, so **Release free fast ≈ the number here + 46 KB**.

## Results

Times in ms. Memory in KB (1024 B), as free fast / ext / steal.

| Row | Measurement | Result |
|---|---|---|
| M1 | Free memory, no song (boot song, session view) | 1002 / 2006 / 63,127 K (Release ≈ 1048 K fast) |
| M2 | Free memory, House 3 loaded (played 15 s, stopped) | 749 / 1950 / 54,476 K, cache 6006 K |
| M3 | Free memory, right after House 3 swapped in over House 3 | 747 / 1948 / 53,314 K, cache 7393 K |
| M4 | Free memory, House 3 + Perform roster, playing | 694 / 1949 / 52,669 K |
| S1 | **Two songs**: House 3 loaded while House 3 plays | fits: **591 K fast free at the swap** (largest block 478 K), 0 late. Parse 288, samples 41, waiting for loop end 2409, total 2758 |
| S2 | **Two heavy songs**: Ttt loaded while Ttt plays | fits: **273 K fast free at the swap** (largest block 148 K), 0 late. Parse 158, samples 8, waiting for loop end 685, total 872 |
| L1 | Synth preset `047 Basic Dirty Bass`, 3 runs | 5.35 / 5.33 / 5.25; 10 K fast each |
| L3 | Factory kit `000 TR-808`, cold, stopped | 154.8 (parse 26.7, sample files 128.0); 131 K fast, 1.2 MB samples |
| L4 | Same, warm | 27.7 |
| L5 | House 3 song load, cold, stopped | 325 (parse 64, samples 260); 249 K fast |
| L5b | Ttt song load, stopped (House 3 samples cached) | 351 (parse 122, samples 227); 228 K fast |
| L6 | `000 TR-808` while House 3 plays | 148 (parse 129, files 19), 0 late, max gap 1.35 ms |
| K1 | 12-lane kit from single samples (`buildSampleKit`), cold, stopped | 80.8; lane avg 6.7 (4.6–13.2) |
| K2 | Same, warm | 1.2; lane avg 0.09 |
| K3 | Same, cold, while House 3 plays | 179.1; lane avg 14.8, 0 late, max gap 1.99 ms |
| K4 | Entering Perform (4 drum lanes + Bass) while House 3 plays | 67.4 (kit 45.4 at 10.5 per lane, Bass 21.2, link 0.7); 52 K fast; 0 late, max gap 2.03 ms |
| R1 | RAM per kit lane | **8,000–8,200 B fast** (the first lane 11,600 B, with the kit itself), 68 B ext, plus the sample in SDRAM (33–132 K for Linndrum one-shots) |
| P1 | Full pad redraw, session view, empty song | 441 B; render 0.007, queue 0.03, UART 21.1 (20.8–21.3), total 21.2 |
| P2 | Same, House 3 in session view | render 0.95, UART 20.8, total 21.8 |
| P3 | Same, in Perform while playing | render 0.002, UART 22.0 (max 26.0), total 22.1 (max 26.0) |

**Audio.** No measurement had a late render (a gap over the 2.9 ms output buffer). The longest gap seen was 2.19 ms, during 15 s of House 3 playing. Listening by ear wasn't done.

## What it means

- **Two rosters fit.** Even two copies of a heavy user song (5 synths, 4 kits) leave 273 K of fast heap. A Perform roster costs about 52 K for 4 lanes plus Bass. A full one (6 parts, 12-lane kits for Drums and FX) is about 2 × 12 × 8 K + 4 × 10 K ≈ 240 K. So a genre change or Regenerate can build fresh Outputs next to the playing ones. Reusing Outputs isn't needed for memory.
- **Kit builds don't glitch.** Building lanes from cold samples while a song plays costs 10–15 ms per lane, and never pushed an audio gap past 2 ms. A 12-lane kit takes under 0.2 s cold and about 1 ms warm. Even a short Build (one bar is 1.8 s at 130 BPM) has ample time.
- **Loading a song while another plays fits too.** A Compose song the size of a heavy user song (about 230–250 K fast) can be built into `preLoadedSong` while another plays. While audio runs, XML parsing is 2–5× slower (TR-808 parse 27 → 129 ms, House 3 64 → 288 ms).
- **Pad redraw costs 21–22 ms of UART per full grid**, almost all of it wire time for 441 B at 200 kbaud. Rendering is under 1 ms. The OLED shares that UART, and pad sends are deferred while it waits. A full redraw on every 16th step (115 ms at 130 BPM) uses about 19% of the UART. Back to back, the UART tops out at about 45 full redraws a second.
- **Seen in the log**: while a song loads, the "buttons and pads" task runs up to 20 ms without yielding. Perform at 17 voices showed `cpu_direness` up to 13.
